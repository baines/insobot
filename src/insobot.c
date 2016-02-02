#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <time.h>
#include <sys/time.h>
#include <sys/inotify.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <glob.h>
#include <dlfcn.h>
#include <libircclient.h>
#include "config.h"
#include "module.h"
#include "stb_sb.h"

typedef struct Module_ {
	char* lib_path;
	void* lib_handle;
	IRCModuleCtx* ctx;
	bool needs_reload;
} Module;

struct INotifyInfo {
	int fd, module_watch, data_watch;
	char *module_path, *data_path;
} inotify_info;

static Module* irc_modules;
static IRCModuleCtx** channel_modules;

static const char *user, *pass, *serv, *port;

static inline const char* env_else(const char* env, const char* def){
	const char* c = getenv(env);
	return c ? c : def;
}

#define IRC_CALLBACK_BASE(name, event_type) static void irc_##name ( \
	irc_session_t* session, \
	event_type     event,   \
	const char*    origin,  \
	const char**   params,  \
	unsigned int   count    \
)

#define IRC_STR_CALLBACK(name) IRC_CALLBACK_BASE(name, const char*)
#define IRC_NUM_CALLBACK(name) IRC_CALLBACK_BASE(name, unsigned int)

#define IRC_MOD_CALL(mod, ptr, args) \
	if((mod)->ctx->ptr){\
		sb_push(mod_call_stack, (mod));\
		(mod)->ctx->ptr args;\
		sb_pop(mod_call_stack);\
	}

#define IRC_MOD_CALL_ALL(ptr, args) \
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){\
		IRC_MOD_CALL(m, ptr, args);\
	}

#define IRC_MOD_CALL_ALL_CHECK(ptr, args, id) \
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){\
		if((m->ctx->flags & IRC_MOD_GLOBAL) || irc_check_perms(m->ctx->name, params[0], id)){\
			IRC_MOD_CALL(m, ptr, args);\
		}\
	}

static Module** mod_call_stack;

static bool irc_check_perms(const char* mod, const char* chan, int id){
	bool ret = true;
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(m->ctx->on_meta){
			sb_push(mod_call_stack, m);
			bool r = m->ctx->on_meta(mod, chan, id);
			ret = ret & r;
			printf("checking %s:%s:%d --> %d\n", mod, chan, id, ret);
			sb_pop(mod_call_stack);
		}
	}
	return ret;
}

IRC_STR_CALLBACK(on_connect) {
	IRC_MOD_CALL_ALL(on_connect, (serv));
}

IRC_STR_CALLBACK(on_chat_msg) {
	if(count < 2 || !params[0] || !params[1]) return;
	IRC_MOD_CALL_ALL_CHECK(on_msg, (params[0], origin, params[1]), IRC_CB_MSG);
}

IRC_STR_CALLBACK(on_join) {
	if(count < 1 || !origin || !params[0]) return;
	fprintf(stderr, "Join: %s %s\n", params[0], origin);
	//XXX: can't use CHECK here unless our own name bypasses it FIXME
	IRC_MOD_CALL_ALL(on_join, (params[0], origin));
}

IRC_STR_CALLBACK(on_part) {
	if(count < 1 || !origin || !params[0]) return;
	IRC_MOD_CALL_ALL_CHECK(on_part, (params[0], origin), IRC_CB_PART);
}

static struct timeval idle_time = {};
static int ping_sent = 0;

IRC_STR_CALLBACK(on_unknown) {
	if(strcmp(event, "PONG") == 0){
		timerclear(&idle_time);
		ping_sent = 0;
	} else {
		printf("Unknown event: %s.\n", event);
	}
}

static const char nick_start_symbols[] = "[]\\`_^{|}";

IRC_NUM_CALLBACK(on_numeric) {
	if(event == LIBIRC_RFC_RPL_NAMREPLY && count >= 4 && params[3]){
		char *names = strdup(params[3]), 
		     *state = NULL,
		     *n     = strtok_r(names, " ", &state);

		do {
			if(!isalpha(*n) && !strchr(nick_start_symbols, *n)){
				++n;
			}
			irc_on_join(session, "join", n, (const char**)(params + 2), 1);
		} while((n = strtok_r(NULL, " ", &state)));
		
		free(names);
	} else {
		printf(". . . Numeric [%u]\n", event);
	}
}

static irc_session_t* irc_ctx = NULL;

static sig_atomic_t running = 1;
static void handle_sig(int n){
	running = 0;
}

static const char* get_username(void){
	return user;
}

//FIXME: would it be better to return a malloc'd string instead?
static char datafile_buff[PATH_MAX];
static const char* get_datafile(void){
	Module* caller = sb_last(mod_call_stack);

	strncpy(datafile_buff, caller->lib_path, sizeof(datafile_buff) - 1);
	datafile_buff[sizeof(datafile_buff) - 1] = 0;

	char* end_ptr = strrchr(datafile_buff, '/');
	if(!end_ptr){
		end_ptr = datafile_buff + 1;
		*datafile_buff = '.';
	}

	const size_t sz = sizeof(datafile_buff) - (end_ptr - datafile_buff);
	snprintf(end_ptr, sz - 1, "/data/%s.data", caller->ctx->name);

	return datafile_buff;
}

static IRCModuleCtx** get_modules(void){
	return channel_modules;
}

static char** channels;

static void join(const char* chan){
	irc_cmd_join(irc_ctx, chan, NULL);
	irc_on_join(irc_ctx, "join", user, &chan, 1);

	for(char** c = channels; c < sb_end(channels); ++c){
		if(strcmp(*c, chan) == 0){
			return;
		}
	}

	sb_push(channels, strdup(chan));
}

static void part(const char* chan){
	irc_cmd_part(irc_ctx, chan);
}

static void send_msg(const char* chan, const char* fmt, ...){
	char buff[1024];
	va_list v;

	va_start(v, fmt);
	if(vsnprintf(buff, sizeof(buff), fmt, v) > 0){
		irc_cmd_msg(irc_ctx, chan, buff);
	}
	va_end(v);
}

static void send_raw(const char* raw){
	irc_send_raw(irc_ctx, raw);
}

static void send_mod_msg(IRCModMsg* msg){
	//FIXME: should mod_msg update the call stack?
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		const char* sender = sb_last(mod_call_stack)->ctx->name;
		if(m->ctx->on_mod_msg) m->ctx->on_mod_msg(sender, msg);
	}
}

static int check_cmds(const char* msg, ...) {
	va_list v;
	va_start(v, msg);

	int count = 0, result = -1;
	const char* str;

	while((str = va_arg(v, const char*))){
		size_t sz = strlen(str);
		if(strncmp(msg, str, sz) == 0 && (msg[sz] == ' ' || msg[sz] == '\0')){
			result = count;
			break;
		}
		++count;
	}
	va_end(v);

	return result;;
}

static void do_module_save(Module* m){
	if(!m->ctx || !m->ctx->on_save) return;
	
	sb_push(mod_call_stack, m);
	
	const char*  save_fname = get_datafile();
	const size_t save_fsz   = strlen(save_fname);
	const char   tmp_end[]  = ".XXXXXX";
	char*        tmp_fname  = alloca(save_fsz + sizeof(tmp_end));

	memcpy(tmp_fname, save_fname, save_fsz);
	memcpy(tmp_fname + save_fsz, tmp_end, sizeof(tmp_end));

	int tmp_fd = mkstemp(tmp_fname);
	if(tmp_fd < 0){
		fprintf(stderr, "Error saving file for %s: %s\n", m->ctx->name, strerror(errno));
	} else {
		FILE* tmp_file = fdopen(tmp_fd, "wb");
		m->ctx->on_save(tmp_file);
		fclose(tmp_file);

		if(rename(tmp_fname, save_fname) < 0){
			fprintf(stderr, "Error saving file for %s: %s\n", m->ctx->name, strerror(errno));
		}
	}

	sb_pop(mod_call_stack);
}

static void check_inotify(const IRCCoreCtx* core_ctx){
	char buff[sizeof(struct inotify_event) + NAME_MAX + 1];
	char* p = buff;

	//TODO: clean this ugly function up & merge common code with the initial loading in main

	ssize_t sz = read(inotify_info.fd, &buff, sizeof(buff));
	while((p - buff) < sz){
		struct inotify_event* ev = (struct inotify_event*)p;

		if(ev->wd == inotify_info.module_watch){

			bool module_currently_loaded = false;
			for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
				//XXX: requires basename not modify its arg, only GNU impl guarantees this
				const char* mod_name = basename(m->lib_path);
				if(strcmp(mod_name, ev->name) == 0){
					m->needs_reload = true;
					module_currently_loaded = true;
					break;
				}
			}

			if(!module_currently_loaded){
				char full_path[PATH_MAX];
				size_t base_len = strlen(inotify_info.module_path);

				assert(base_len + ev->len + 1 < PATH_MAX);

				memcpy(full_path, inotify_info.module_path, base_len);
				memcpy(full_path + base_len, ev->name, ev->len);
				full_path[base_len + ev->len] = 0;

				Module new_mod = {
					.lib_path = strdup(full_path),
					.needs_reload = true,
				};

				sb_push(irc_modules, new_mod);
			}
		} else if(ev->wd == inotify_info.data_watch){
			//TODO: notify modules their data file was modified
			//XXX: avoid doing this when they save it themselves somehow...
			size_t len = strlen(ev->name);
			if(len > 5 && memcmp(ev->name + (len - 5), ".data", 6) == 0){
				fprintf(stderr, "%s got modified. TODO: do something...\n", ev->name);
			}
		}

		p += (sizeof(struct inotify_event) + ev->len);
	}

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(m->needs_reload){
			m->needs_reload = false;
			
			const char* mod_name = basename(m->lib_path);
			fprintf(stderr, "Reloading module %s...\n", mod_name);

			bool success = true;

			do_module_save(m);
			if(m->lib_handle){
				dlclose(m->lib_handle);
			}
			
			IRCModuleCtx* prev_ctx = m->ctx;

			m->lib_handle = dlopen(m->lib_path, RTLD_LAZY);
			if(!m->lib_handle){
				fprintf(stderr, "Error reloading module: %s\n", dlerror());
				success = false;
			} else {
				m->ctx = dlsym(m->lib_handle, "irc_mod_ctx");
				if(!m->ctx){
					fprintf(stderr, "Can't reload %s, no irc_mod_ctx\n", basename(m->lib_path));
					dlclose(m->lib_handle);
					success = false;
				}
			}

			if(success){
				if(m->ctx->on_init){
					//TODO: check return value
					sb_push(mod_call_stack, m);
					m->ctx->on_init(core_ctx);
					sb_pop(mod_call_stack);
				}

				if(!(m->ctx->flags & IRC_MOD_GLOBAL)){
					bool found_ctx = false;
					for(IRCModuleCtx** c = channel_modules; *c; ++c){
						if(*c == prev_ctx){
							*c = m->ctx;
							found_ctx = true;
							break;
						}
					}

					if(!found_ctx){
						sb_last(channel_modules) = m->ctx;
						sb_push(channel_modules, 0);
					}
				}

				for(char** c = channels; c < sb_end(channels); ++c){
					irc_on_join(irc_ctx, "join", user, (const char**)c, 1);
				}

				fprintf(stderr, "Reload successful.\n");
			} else {
				for(IRCModuleCtx** c = channel_modules; *c; ++c){
					if(*c == prev_ctx){
						sb_erase(channel_modules, c - channel_modules);
						break;
					}
				}

				free(m->lib_path);
				sb_erase(irc_modules, m - irc_modules);
				--m;
			}
		}
	}
}

int main(int argc, char** argv){

	srand(time(0));
	signal(SIGINT, &handle_sig);
	
	user = env_else("IRC_USER", BOT_NAME);
	pass = env_else("IRC_PASS", BOT_PASS);
	serv = env_else("IRC_SERV", "irc.nonexistent.domain");
	port = env_else("IRC_PORT", "6667");
	
	char our_path[PATH_MAX];
	memset(our_path, 0, sizeof(our_path));

	ssize_t sz = readlink("/proc/self/exe", our_path, sizeof(our_path));
	if(sz < 0){
		err(errno, "Can't read path");
	}
	
	char* path_end = strrchr(our_path, '/');
	if(!path_end){
		path_end = our_path + 1;
		*our_path = '.';
	}

	const char in_mod_suffix[] = "/modules/";
	const char in_dat_suffix[] = "/modules/data/";
	const char glob_suffix[]   = "/modules/*.so";

	if(path_end + sizeof(in_dat_suffix) >= our_path + sizeof(our_path)){
		errx(1, "Path too long!");
	}
	inotify_info.fd = inotify_init1(IN_NONBLOCK);
	
	memcpy(path_end, in_mod_suffix, sizeof(in_mod_suffix));
	inotify_info.module_path  = strdup(our_path);
	inotify_info.module_watch = inotify_add_watch(
		inotify_info.fd,
		our_path,
		IN_CLOSE_WRITE
	);

	memcpy(path_end, in_dat_suffix, sizeof(in_dat_suffix));
	inotify_info.data_path  = strdup(our_path);
	inotify_info.data_watch = inotify_add_watch(
		inotify_info.fd,
		our_path,
		IN_CLOSE_WRITE
	);

	memcpy(path_end, glob_suffix, sizeof(glob_suffix));
	
	glob_t glob_data = {};
	int glob_ret = 0;

	if((glob_ret = glob(our_path, 0, NULL, &glob_data)) != 0){
		switch(glob_ret){
			case GLOB_NOSPACE: errx(1, "No memory for glob!"); break;
			case GLOB_ABORTED: errx(1, "Glob read error!");    break;
			case GLOB_NOMATCH: errx(1, "No modules found!");   break;
		}
	}

	const IRCCoreCtx core_ctx = {
		.get_username = &get_username,
		.get_datafile = &get_datafile,
		.get_modules  = &get_modules,
		.send_msg     = &send_msg,
		.send_raw     = &send_raw,
		.send_mod_msg = &send_mod_msg,
		.join         = &join,
		.part         = &part,
		.check_cmds   = &check_cmds,
	};

	printf("Found %zu modules\n", glob_data.gl_pathc);

	for(int i = 0; i < glob_data.gl_pathc; ++i){
		Module m = {};

		void* handle = dlopen(glob_data.gl_pathv[i], RTLD_LAZY);
		char* mname  = basename(glob_data.gl_pathv[i]); //XXX: relies on GNU version of basename?
		
		if(!handle){
			printf("Error: %s: dlopen error: %s\n", mname, dlerror());
			continue;
		}
		
		m.ctx = dlsym(handle, "irc_mod_ctx");
		if(!m.ctx){
			printf("Error: %s: No irc_mod_ctx symbol!\n", mname);
			dlclose(handle);
			continue;
		}

		m.lib_path = strdup(glob_data.gl_pathv[i]);
		m.lib_handle = handle;

		sb_push(irc_modules, m);

		if(!(m.ctx->flags & IRC_MOD_GLOBAL)){
			sb_push(channel_modules, m.ctx);
		}

		printf("Loaded module %s\n", mname);
	}

	sb_push(channel_modules, 0);

	globfree(&glob_data);

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		sb_push(mod_call_stack, m);
		if(m->ctx->on_init){
			//FIXME: remove modules that return false
			bool ok = m->ctx->on_init(&core_ctx);
		}
		sb_pop(mod_call_stack);
	}

	if(sb_count(irc_modules) == 0){
		errx(1, "No modules could be loaded.");
	}

			
	//TODO: qsort modules by priority
	
	irc_callbacks_t callbacks = {
		.event_connect = irc_on_connect,
		.event_channel = irc_on_chat_msg,
		.event_join    = irc_on_join,
		.event_part    = irc_on_part,
		.event_numeric = irc_on_numeric,
		.event_unknown = irc_on_unknown,
	};

	do {
		irc_ctx = irc_create_session(&callbacks);
		irc_option_set(irc_ctx, LIBIRC_OPTION_STRIPNICKS);

		if(irc_connect(irc_ctx, serv, atoi(port), pass, user, user, user) != 0){
			errx(1, "couldn't connect");
		}
				
		while(running && irc_is_connected(irc_ctx)){
	
			check_inotify(&core_ctx);

			int max_fd = 0;
			fd_set in, out;
	
			FD_ZERO(&in);
			FD_ZERO(&out);

			irc_add_select_descriptors(irc_ctx, &in, &out, &max_fd);
	
			struct timeval tv = {
				.tv_sec  = 0,
				.tv_usec = 250000,
			}, tv_orig = tv;
	
			int ret = select(max_fd + 1, &in, &out, NULL, &tv);
				
			if(ret < 0){
				perror("select");
			} else if(ret == 0){
				timersub(&tv_orig, &tv, &tv);
				timeradd(&tv, &idle_time, &idle_time);
			
				if(idle_time.tv_sec >= 30 && !ping_sent){
					irc_send_raw(irc_ctx, "PING %s", serv);
					ping_sent = 1;
				}
			
				if(idle_time.tv_sec >= 60){
					puts("Timeout > 60... quitting");
					irc_disconnect(irc_ctx);
				}
			
			} else if(ret > 0){

				int i;
				for(i = 0; i <= max_fd; ++i){
					if(FD_ISSET(i, &in)){
						timerclear(&idle_time);
						break;
					}
				}
			
				irc_process_select_descriptors(irc_ctx, &in, &out);
			}
		}
	
		irc_destroy_session(irc_ctx);
	
		if(running){
			puts("Restarting");
			usleep(5000000);
		}
	} while(running);

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		do_module_save(m);
	}
	
	return 0;
}
