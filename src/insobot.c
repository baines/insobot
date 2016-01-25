#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <time.h>
#include <sys/time.h>
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
	const char* lib_path;
	void* lib_handle;
	IRCModuleCtx* ctx;
} Module;

static Module* irc_modules;
static IRCModuleCtx** channel_modules;

static const char *user, *pass, *serv, *chan, *port;

static const char* env_else(const char* env, const char* def){
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

static Module** mod_call_stack;

IRC_STR_CALLBACK(on_connect) {
	IRC_MOD_CALL_ALL(on_connect, ());
}

IRC_STR_CALLBACK(on_chat_msg) {
	if(count < 2 || !params[0] || !params[1]) return;
	IRC_MOD_CALL_ALL(on_msg, (params[0], origin, params[1]));
}

IRC_STR_CALLBACK(on_join) {
	if(count < 1 || !origin || !params[0]) return;
	IRC_MOD_CALL_ALL(on_join, (params[0], origin));
}

IRC_STR_CALLBACK(on_part) {
	if(count < 1 || !origin || !params[0]) return;
	IRC_MOD_CALL_ALL(on_part, (params[0], origin));
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

IRC_NUM_CALLBACK(on_numeric) {
	if(event == LIBIRC_RFC_RPL_NAMREPLY && count >= 4 && params[3]){
		char *names = strdup(params[3]), 
		     *state = NULL,
		     *n     = strtok_r(names, " ", &state);
		
		do {
			irc_on_join(session, "join", origin, (const char**)&n, 1);
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

static void join(const char* chan){
	irc_cmd_join(irc_ctx, chan, NULL);
	//TODO: send immediate join for our name
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
		if(strncmp(msg, str, sz) == 0){
			result = count;
			break;
		}
		++count;
	}
	va_end(v);

	return result;;
}

int main(int argc, char** argv){

	srand(time(0));
	signal(SIGINT, &handle_sig);
	
	user = env_else("IRC_USER", BOT_NAME);
	pass = env_else("IRC_PASS", BOT_PASS);
	serv = env_else("IRC_SERV", "irc.nonexistent.domain");
	chan = env_else("IRC_CHAN", "#nowhere");
	port = env_else("IRC_PORT", "6667");
	
	char our_path[PATH_MAX];
	ssize_t sz = readlink("/proc/self/exe", our_path, sizeof(our_path));
	if(sz < 0){
		err(errno, "Can't read path");
	}
	
	char* path_end = strrchr(our_path, '/');
	if(!path_end){
		path_end = our_path + 1;
		*our_path = '.';
	}

	const char suffix[] = "/modules/*.so";
	if(path_end + sizeof(suffix) >= our_path + sizeof(our_path)){
		errx(1, "Path too long!");
	}
	memcpy(path_end, suffix, sizeof(suffix));

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
		.send_mod_msg = &send_mod_msg,
		.join         = &join,
		.part         = &part,
		.check_cmds   = &check_cmds,
	};

	printf("Found %zu modules\n", glob_data.gl_pathc);

	for(int i = 0; i < glob_data.gl_pathc; ++i){
		Module m;

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
		sb_push(mod_call_stack, m);
		if(m->ctx->on_save) m->ctx->on_save();
		sb_pop(mod_call_stack);
	}
	
	return 0;
}
