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
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <glob.h>
#include <dlfcn.h>
#include <libircclient.h>
#include <libirc_rfcnumeric.h>
#include "config.h"
#include "module.h"
#include "stb_sb.h"

#ifndef LIBIRC_OPTION_SSL_NO_VERIFY
	#define LIBIRC_OPTION_SSL_NO_VERIFY (1 << 3)
#endif

/******************************
 * Types, global vars, macros *
 * ****************************/

typedef struct Module_ {
	char* lib_path;
	void* lib_handle;
	IRCModuleCtx* ctx;
	bool needs_reload, data_modified;
} Module;

typedef struct INotifyInfo_ {
	int fd, module_watch, data_watch;
	char *module_path, *data_path;
} INotifyInfo;

typedef struct IRCCmd_ {
	int cmd;
	char *chan, *data;
} IRCCmd;

enum { IRC_CMD_JOIN, IRC_CMD_PART, IRC_CMD_MSG, IRC_CMD_RAW };

static IRCCmd* cmd_queue;
static uint32_t prev_cmd_ms;

static irc_session_t* irc_ctx;

static Module* irc_modules;
static Module** mod_call_stack;
static IRCModuleCtx** chan_mod_list;
static IRCModuleCtx** global_mod_list;
static bool mod_list_dirty = true;

static const char *user, *pass, *serv, *port;
static char* bot_nick;

static char**  channels;
static char*** chan_nicks;

static INotifyInfo inotify_info;

static struct timeval idle_time = {};
static int ping_sent;

static sig_atomic_t running = 1;

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
		if((m->ctx->flags & IRC_MOD_GLOBAL) || util_check_perms(m->ctx->name, params[0], id)){\
			IRC_MOD_CALL(m, ptr, args);\
		}\
	}

/*********************************
 * Required forward declarations *
 *********************************/

IRC_STR_CALLBACK(on_join);
IRC_STR_CALLBACK(on_part);

static const char* core_get_datafile(void);

/****************
 * Helper funcs *
 ****************/

static void util_handle_sig(int n){
	running = 0;
}

static inline const char* util_env_else(const char* env, const char* def){
	const char* c = getenv(env);
	return c ? c : def;
}

static bool util_check_perms(const char* mod, const char* chan, int id){
	bool ret = true;
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(m->ctx->on_meta){
			sb_push(mod_call_stack, m);
			bool r = m->ctx->on_meta(mod, chan, id);
			ret = ret & r;
			//printf("checking %s:%s:%d --> %d\n", mod, chan, id, ret);
			sb_pop(mod_call_stack);
		}
	}
	return ret;
}

static void util_dispatch_cmds(Module* m, const char* chan, const char* name, const char* msg){
	if(!m->ctx || !m->ctx->commands || !m->ctx->on_cmd) return;

	for(const char** cmd_list = m->ctx->commands; *cmd_list; ++cmd_list){
		const char *cmd = *cmd_list, *cmd_end;

		do {
			cmd_end = strchrnul(cmd, ' ');
			const size_t sz = cmd_end - cmd;

			if(strncasecmp(msg, cmd, sz) == 0 && (msg[sz] == ' ' || msg[sz] == '\0')){
				sb_push(mod_call_stack, m);
				m->ctx->on_cmd(chan, name, msg + sz, cmd_list - m->ctx->commands);
				sb_pop(mod_call_stack);
				return;
			}

			while(*cmd_end == ' ') ++cmd_end;
			cmd = cmd_end;
		} while(*cmd_end);
	}
}

static void util_cmd_enqueue(int cmd, const char* chan, const char* data){
	if(sb_count(cmd_queue) > CMD_QUEUE_MAX) return;

	IRCCmd c = {
		.cmd  = cmd,
		.chan = chan ? strdup(chan) : NULL,
		.data = data ? strdup(data) : NULL
	};

	sb_push(cmd_queue, c);
}

static void util_process_pending_cmds(void){

	if(!sb_count(cmd_queue)) return;

	struct timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t cmd_ms = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

	if((cmd_ms - prev_cmd_ms) > CMD_RATE_LIMIT_MS){
		prev_cmd_ms = cmd_ms;

		IRCCmd cmd = cmd_queue[0];
		switch(cmd.cmd){

			case IRC_CMD_JOIN: {
				irc_cmd_join(irc_ctx, cmd.chan, cmd.data);
				irc_on_join(irc_ctx, "join", cmd.data, (const char**)&cmd.chan, 1);
			} break;

			case IRC_CMD_PART: {
				irc_cmd_part(irc_ctx, cmd.chan);
				irc_on_part(irc_ctx, "part", cmd.data, (const char**)&cmd.chan, 1);
			} break;

			case IRC_CMD_MSG: {
				irc_cmd_msg(irc_ctx, cmd.chan, cmd.data);
			} break;

			case IRC_CMD_RAW: {
				irc_send_raw(irc_ctx, "%s", cmd.data);
			} break;
		}

		if(cmd.chan) free(cmd.chan);
		if(cmd.data) free(cmd.data);
		sb_erase(cmd_queue, 0);
	}
}

static void util_module_save(Module* m){
	if(!m->ctx || !m->ctx->on_save) return;

	// change the inotify data watch to something we don't care about to disable it temporarily
	inotify_info.data_watch = inotify_add_watch(
		inotify_info.fd,
		inotify_info.data_path,
		IN_DELETE_SELF
	);

	sb_push(mod_call_stack, m);
	
	const char*  save_fname = core_get_datafile();
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
		bool saved = m->ctx->on_save(tmp_file);
		fclose(tmp_file);

		if(saved && rename(tmp_fname, save_fname) < 0){
			fprintf(stderr, "Error saving file for %s: %s\n", m->ctx->name, strerror(errno));
		} else if(!saved){
			unlink(tmp_fname);
		}
	}

	sb_pop(mod_call_stack);

	inotify_info.data_watch = inotify_add_watch(
		inotify_info.fd,
		inotify_info.data_path,
		IN_CLOSE_WRITE | IN_MOVED_TO
	);
}

static void util_check_inotify(const IRCCoreCtx* core_ctx){
	char buff[sizeof(struct inotify_event) + NAME_MAX + 1];
	struct inotify_event* ev;
	char* p = buff;

	//TODO: clean this ugly function up & merge common code with the initial loading in main

	ssize_t sz = read(inotify_info.fd, &buff, sizeof(buff));
	for(; (p - buff) < sz; p += (sizeof(*ev) + ev->len)){
		ev = (struct inotify_event*)p;

		if(!(ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))){
			continue;
		}

		if(ev->wd == inotify_info.module_watch){

			mod_list_dirty = true;

			bool module_currently_loaded = false;
			for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
				//XXX: requires basename not modify its arg, only GNU impl guarantees this
				const char* mod_name = basename(m->lib_path);
				if(strcmp(mod_name, ev->name) == 0){
					printf("Found loaded module to be reloaded: %s\n", ev->name);
					m->needs_reload = true;
					module_currently_loaded = true;
					break;
				}
			}

			if(!module_currently_loaded){

				printf("Found new module to load: %s\n", ev->name);

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
			size_t len = strlen(ev->name);
			if(len <= 5 || memcmp(ev->name + (len - 5), ".data", 6) != 0){
				continue;
			}

			for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
				if(strlen(m->ctx->name) == len - 5 && strncmp(m->ctx->name, ev->name, len - 5) == 0){
					fprintf(stderr, "%s was modified.\n", ev->name);
					m->data_modified = true;
					break;
				}
			}
		}
	}

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(m->needs_reload){
			m->needs_reload = false;
			m->data_modified = false;
			
			const char* mod_name = basename(m->lib_path);
			fprintf(stderr, "Reloading module %s...\n", mod_name);

			bool success = true;

			util_module_save(m);

			if(m->lib_handle){
				IRC_MOD_CALL(m, on_quit, ());
				dlclose(m->lib_handle);
			}
			
			m->lib_handle = dlopen(m->lib_path, RTLD_LAZY | RTLD_LOCAL);
			if(!m->lib_handle){
				fprintf(stderr, "Error reloading module: %s\n", dlerror());
				success = false;
			} else {
				m->ctx = dlsym(m->lib_handle, "irc_mod_ctx");
				if(!m->ctx || dlerror()){
					fprintf(stderr, "Can't reload %s, no irc_mod_ctx\n", basename(m->lib_path));
					dlclose(m->lib_handle);
					success = false;
				}
			}

			if(success && m->ctx->on_init){
				sb_push(mod_call_stack, m);
				if(!m->ctx->on_init(core_ctx)){
					fprintf(stderr, "Init function returned false on reload for %s.\n", basename(m->lib_path));
					success = false;
					dlclose(m->lib_handle);
				}
				sb_pop(mod_call_stack);
			}

			if(success){
				for(int i = 0; i < sb_count(channels) - 1; ++i){
					const char** c = (const char**)channels + i;

					irc_on_join(irc_ctx, "join", bot_nick, c, 1);
					for(int j = 0; j < sb_count(chan_nicks[i]); ++j){
						irc_on_join(irc_ctx, "join", chan_nicks[i][j], c, 1);
					}
				}

				fprintf(stderr, "Reload successful.\n");
			} else {
				free(m->lib_path);
				sb_erase(irc_modules, m - irc_modules);
				--m;
			}
		} else if(m->data_modified){
			m->data_modified = false;
			fprintf(stderr, "Calling on_data_modified for %s\n", m->ctx->name);
			IRC_MOD_CALL(m, on_modified, ());
		}
	}
}

static int util_mod_sort(const void* a, const void* b){
	return ((Module*)b)->ctx->priority - ((Module*)a)->ctx->priority;
}

static void util_find_chan_nick(const char* chan, const char* nick, int* chan_idx, int* nick_idx){

	if(chan_idx) *chan_idx = -1;
	if(nick_idx) *nick_idx = -1;

	for(int i = 0; i < sb_count(channels) - 1; ++i){
		if(strcasecmp(channels[i], chan) != 0) continue;

		if(chan_idx) *chan_idx = i;

		for(int j = 0; j < sb_count(chan_nicks[i]); ++j){
			if(strcasecmp(chan_nicks[i][j], nick) == 0){
				if(nick_idx) *nick_idx = j;
				break;
			}
		}

		break;
	}
}

/*****************
 * IRC Callbacks *
 *****************/

IRC_STR_CALLBACK(on_connect) {
	printf("Our nick is %s\n", params[0]);
	free(bot_nick);
	bot_nick = strdup(params[0]);

	IRC_MOD_CALL_ALL(on_connect, (serv));
}

IRC_STR_CALLBACK(on_chat_msg) {
	if(count < 2 || !params[0] || !params[1]) return;
	const char *_chan = params[0], *_name = origin;

	char* _msg = strdupa(params[1]);
	size_t len = strlen(_msg);

	// trim spaces at end of messages
	if(len > 0){
		for(char* p = _msg + len - 1; *p == ' '; --p){
			*p = 0;
		}
	}

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		bool global = m->ctx->flags & IRC_MOD_GLOBAL;
		if(global || util_check_perms(m->ctx->name, _chan, IRC_CB_CMD)){
			util_dispatch_cmds(m, _chan, _name, _msg);
		}
		if(global || util_check_perms(m->ctx->name, _chan, IRC_CB_MSG)){
			IRC_MOD_CALL(m, on_msg, (_chan, _name, _msg));
		}
	}
}

IRC_STR_CALLBACK(on_join) {
	if(count < 1 || !origin || !params[0]) return;
	fprintf(stderr, "Join: %s %s\n", params[0], origin);

	int chan_i, nick_i;
	util_find_chan_nick(params[0], origin, &chan_i, &nick_i);

	if(chan_i == -1){
		sb_last(channels) = strdup(params[0]);
		chan_i = sb_count(channels) - 1;
		
		sb_push(channels, 0);
		sb_push(chan_nicks, 0);
	}

	if(nick_i == -1){
		sb_push(chan_nicks[chan_i], strdup(origin));
	}

	//XXX: can't use CHECK here unless our own name bypasses it FIXME
	IRC_MOD_CALL_ALL(on_join, (params[0], origin));
}

IRC_STR_CALLBACK(on_part) {
	if(count < 1 || !origin || !params[0]) return;
	
	int chan_i, nick_i;
	util_find_chan_nick(params[0], origin, &chan_i, &nick_i);

	printf("PART: %s %s %d %d\n", params[0], origin, chan_i, nick_i);

	if(chan_i != -1 && strcasecmp(origin, bot_nick) == 0){
		puts("free all nicks");
		free(channels[chan_i]);
		sb_erase(channels, chan_i);

		for(int i = 0; i < sb_count(chan_nicks[chan_i]); ++i){
			free(chan_nicks[chan_i][i]);
		}
		sb_free(chan_nicks[chan_i]);
		sb_erase(chan_nicks, chan_i);

	} else if(nick_i != -1){
		puts("free nick");
		free(chan_nicks[chan_i][nick_i]);
		sb_erase(chan_nicks[chan_i], nick_i);
	}

	IRC_MOD_CALL_ALL_CHECK(on_part, (params[0], origin), IRC_CB_PART);
}

IRC_STR_CALLBACK(on_nick) {
	if(count < 1 || !origin || !params[0]) return;
	if(strcmp(origin, bot_nick) == 0){
		printf("We changed nicks! new nick: %s\n", params[0]);
		free(bot_nick);
		bot_nick = strdup(params[0]);
	}

	for(int i = 0; i < sb_count(channels) - 1; ++i){
		for(int j = 0; j < sb_count(chan_nicks[i]); ++j){
			if(strcasecmp(chan_nicks[i][j], origin) == 0){
				free(chan_nicks[i][j]);
				chan_nicks[i][j] = strdup(params[0]);
				break;
			}
		}
	}

	IRC_MOD_CALL_ALL(on_nick, (origin, params[0]));
}

IRC_STR_CALLBACK(on_unknown) {
	if(strcmp(event, "PONG") == 0){
		timerclear(&idle_time);
		ping_sent = 0;
	} else {
		printf("Unknown event: %s.\n", event);
		for(int i = 0; i < count; ++i){
			printf(". . %s\n", params[i]);
		}
	}
}

IRC_NUM_CALLBACK(on_numeric) {
	const char nick_start_symbols[] = "[]\\`_^{|}";
	
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
		for(int i = 0; i < count; ++i){
			printf(". . %s\n", params[i]);
		}
	}
}

/********************
 * IRCCoreCtx funcs *
 ********************/

static const char* core_get_username(void){
	return bot_nick;
}

//FIXME: would it be better to return a malloc'd string instead?
static char datafile_buff[PATH_MAX];

static const char* core_get_datafile(void){
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

	if(access(datafile_buff, F_OK) != 0){
		// change the inotify data watch to something we don't care about to disable it temporarily
		inotify_info.data_watch = inotify_add_watch(
			inotify_info.fd,
			inotify_info.data_path,
			IN_DELETE_SELF
		);

		close(creat(datafile_buff, 00600));

		inotify_info.data_watch = inotify_add_watch(
			inotify_info.fd,
			inotify_info.data_path,
			IN_CLOSE_WRITE | IN_MOVED_TO
		);
	}

	return datafile_buff;
}

static IRCModuleCtx** core_get_modules(bool chan_only){

	if(mod_list_dirty){
		while(sb_count(chan_mod_list) > 0) sb_pop(chan_mod_list);
		while(sb_count(global_mod_list) > 0) sb_pop(global_mod_list);

		for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
			sb_push(global_mod_list, m->ctx);
			if(!(m->ctx->flags & IRC_MOD_GLOBAL)){
				sb_push(chan_mod_list, m->ctx);
			}
		}

		sb_push(chan_mod_list, 0);
		sb_push(global_mod_list, 0);

		mod_list_dirty = false;
	}

	return chan_only ? chan_mod_list : global_mod_list;
}

static const char** core_get_channels(void){
	return (const char**)channels;
}

static void core_join(const char* chan){

	util_cmd_enqueue(IRC_CMD_JOIN, chan, NULL); //TODO: password protected channels?

	int chan_i, nick_i;
	util_find_chan_nick(chan, bot_nick, &chan_i, &nick_i);

	if(chan_i == -1){
		sb_last(channels) = strdup(chan);
		chan_i = sb_count(channels) - 1;
		
		sb_push(channels, 0);

		sb_push(chan_nicks, 0);
		sb_push(chan_nicks[chan_i], strdup(bot_nick));
	}

}

static void core_part(const char* chan){
	
	util_cmd_enqueue(IRC_CMD_PART, chan, NULL);

	for(char** c = channels; *c; ++c){
		if(strcmp(*c, chan) == 0){
			free(*c);
			sb_erase(channels, c - channels);
			break;
		}
	}
}

static void core_send_msg(const char* chan, const char* fmt, ...){
	char buff[1024];
	va_list v;

	va_start(v, fmt);
	if(vsnprintf(buff, sizeof(buff), fmt, v) > 0){
		util_cmd_enqueue(IRC_CMD_MSG, chan, buff);
	}
	va_end(v);
}

static void core_send_raw(const char* raw){
	util_cmd_enqueue(IRC_CMD_RAW, NULL, raw);
}

static void core_send_mod_msg(IRCModMsg* msg){
	const char* sender = sb_last(mod_call_stack)->ctx->name;
	IRC_MOD_CALL_ALL(on_mod_msg, (sender, msg));
}

static void core_self_save(void){
	util_module_save(sb_last(mod_call_stack));
}

static void core_log(const char* fmt, ...){
	char time_buf[64];
	time_t now = time(0);
	struct tm* now_tm = localtime(&now);
	strftime(time_buf, sizeof(time_buf), "[%F][%T]", now_tm);

	const char* mod_name = sb_count(mod_call_stack) ? sb_last(mod_call_stack)->ctx->name : "CORE";
	fprintf(stderr, "%s %s: ", time_buf, mod_name);

	va_list v;
	va_start(v, fmt);
	vfprintf(stderr, fmt, v);
	va_end(v);
}

/***************
 * entry point *
 * *************/

int main(int argc, char** argv){

	srand(time(0));
	signal(SIGINT, &util_handle_sig);
	
	user = util_env_else("IRC_USER", DEFAULT_BOT_NAME);
	pass = util_env_else("IRC_PASS", NULL);
	serv = util_env_else("IRC_SERV", "irc.nonexistent.domain");
	port = util_env_else("IRC_PORT", "6667");
	bot_nick = strdup(user);

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

	if(access(our_path, W_OK) != 0){
		puts("No modules dir, creating it");
		if(mkdir(our_path, 00750) == -1){
			perror("Error creating modules dir");
		}
	}

	inotify_info.module_path  = strdup(our_path);
	inotify_info.module_watch = inotify_add_watch(
		inotify_info.fd,
		our_path,
		IN_CLOSE_WRITE | IN_MOVED_TO
	);

	memcpy(path_end, in_dat_suffix, sizeof(in_dat_suffix));

	if(access(our_path, W_OK) != 0){
		puts("No modules/data dir, creating it");
		if(mkdir(our_path, 00750) == -1){
			perror("Error creating modules/data dir");
		}
	}

	inotify_info.data_path  = strdup(our_path);
	inotify_info.data_watch = inotify_add_watch(
		inotify_info.fd,
		our_path,
		IN_CLOSE_WRITE | IN_MOVED_TO
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
		.get_username = &core_get_username,
		.get_datafile = &core_get_datafile,
		.get_modules  = &core_get_modules,
		.get_channels = &core_get_channels,
		.send_msg     = &core_send_msg,
		.send_raw     = &core_send_raw,
		.send_mod_msg = &core_send_mod_msg,
		.join         = &core_join,
		.part         = &core_part,
		.save_me      = &core_self_save,
		.log          = &core_log,
	};

	printf("Found %zu modules\n", glob_data.gl_pathc);

	for(int i = 0; i < glob_data.gl_pathc; ++i){
		Module m = {};

		void* handle = dlopen(glob_data.gl_pathv[i], RTLD_LAZY | RTLD_LOCAL);
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

		printf("Loaded module %s\n", mname);
	}

	globfree(&glob_data);

	sb_push(channels, 0);

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		bool success = true;

		sb_push(mod_call_stack, m);
		if(m->ctx->on_init){
			success = m->ctx->on_init(&core_ctx);
		}
		sb_pop(mod_call_stack);

		if(!success){
			sb_erase(irc_modules, m - irc_modules);
			--m;
		}
	}
	
	if(sb_count(irc_modules) == 0){
		errx(1, "No modules could be loaded.");
	}

	qsort(irc_modules, sb_count(irc_modules), sizeof(*irc_modules), &util_mod_sort);
	
	irc_callbacks_t callbacks = {
		.event_connect = irc_on_connect,
		.event_channel = irc_on_chat_msg,
		.event_join    = irc_on_join,
		.event_part    = irc_on_part,
		.event_nick    = irc_on_nick,
		.event_numeric = irc_on_numeric,
		.event_unknown = irc_on_unknown,
	};

	do {
		irc_ctx = irc_create_session(&callbacks);
		if(!irc_ctx){
			fprintf(stderr, "Failed to create irc session.\n");
		}

		irc_option_set(irc_ctx, LIBIRC_OPTION_DEBUG);
		irc_option_set(irc_ctx, LIBIRC_OPTION_STRIPNICKS);

		char* libirc_serv;
		if(getenv("IRC_ENABLE_SSL")){
			puts("Using ssl connection...");
			asprintf(&libirc_serv, "#%s", serv);

			//XXX: you might not want this!
			irc_option_set(irc_ctx, LIBIRC_OPTION_SSL_NO_VERIFY);
		} else {
			libirc_serv = strdup(serv);
		}

		if(irc_connect(irc_ctx, libirc_serv, atoi(port), pass, user, user, user) != 0){
			fprintf(stderr, "Unable to connect: %s\n", irc_strerror(irc_errno(irc_ctx)));
		}

		free(libirc_serv);

		while(running && irc_is_connected(irc_ctx)){

			util_process_pending_cmds();

			util_check_inotify(&core_ctx);

			//TODO: check on_meta & better timing for on_tick?
			IRC_MOD_CALL_ALL(on_tick, ());

			int max_fd = 0;
			fd_set in, out;
	
			FD_ZERO(&in);
			FD_ZERO(&out);
			FD_SET(STDIN_FILENO, &in);

			if(irc_add_select_descriptors(irc_ctx, &in, &out, &max_fd) != 0){
				fprintf(stderr, "Error adding select fds: %s\n", irc_strerror(irc_errno(irc_ctx)));
			}

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

				//TODO: figure out why this doesn't work properly over ssl
#if 0	
				if(idle_time.tv_sec >= 30 && !ping_sent){
					irc_send_raw(irc_ctx, "PING %s", serv);
					ping_sent = 1;
				}
			
				if(idle_time.tv_sec >= 60){
					puts("Timeout > 60... quitting");
					irc_disconnect(irc_ctx);
					ping_sent = 0;
				}
#endif		
			} else if(ret > 0){
#if 0
				int i;
				for(i = 0; i <= max_fd; ++i){
					if(FD_ISSET(i, &in)){
						timerclear(&idle_time);
						break;
					}
				}
#endif
				if(irc_process_select_descriptors(irc_ctx, &in, &out) != 0){
					fprintf(stderr, "Error processing select fds: %s\n", irc_strerror(irc_errno(irc_ctx)));
				}

				if(FD_ISSET(STDIN_FILENO, &in)){
					char stdin_buf[1024];
					ssize_t n =  read(STDIN_FILENO, stdin_buf, sizeof(stdin_buf));
					if(n > 0){
						stdin_buf[n-1] = 0; // remove \n
						IRC_MOD_CALL_ALL(on_stdin, (stdin_buf));
					}
				}
			}
		}
	
		irc_destroy_session(irc_ctx);
	
		if(running){
			if(getenv("INSOBOT_NO_AUTO_RESTART")){
				puts("Restarting when you press a key...");
				getchar();
			} else {
				puts("Restarting");
			}
			usleep(5000000);
		}
	} while(running);

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		util_module_save(m);
	}
	
	return 0;
}
