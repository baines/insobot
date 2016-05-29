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
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <glob.h>
#include <dlfcn.h>
#include <pthread.h>
#include <libircclient.h>
#include <libirc_rfcnumeric.h>
#include <curl/curl.h>
#include "config.h"
#include "module.h"
#include "stb_sb.h"
#include "utils.h"

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

typedef struct INotifyWatch {
	int wd;
	char* path;
} INotifyWatch;

typedef struct INotifyData_ {
	int fd;
	INotifyWatch module, data, ipc;
} INotifyData;

typedef struct IRCCmd_ {
	int cmd;
	char *chan, *data;
} IRCCmd;

typedef struct IPCAddress_ {
	int id;
	struct sockaddr_un addr;
} IPCAddress;

enum { MOD_GET_SONAME, MOD_GET_CTXNAME };

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

static INotifyData inotify;

static struct timeval idle_time = {};
static int ping_sent;

static int         ipc_socket;
static IPCAddress  ipc_self;
static IPCAddress* ipc_peers;

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

#define IRC_MOD_CALL(mod, ptr, args) ({                                       \
	sb_push(mod_call_stack, mod);                                             \
	__auto_type ret = (mod)->ctx->ptr ?                                       \
		__builtin_choose_expr(                                                \
			__builtin_types_compatible_p(typeof((mod)->ctx->ptr args), void), \
			((mod)->ctx->ptr args, (int)0),                                   \
			(mod)->ctx->ptr args                                              \
		) : 0;                                                                \
	sb_pop(mod_call_stack);                                                   \
	ret;                                                                      \
})

#define IRC_MOD_CALL_ALL(ptr, args)                             \
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){ \
		IRC_MOD_CALL(m, ptr, args);                             \
	}

#define IRC_MOD_CALL_ALL_CHECK(ptr, args, id)                   \
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){ \
		if(                                                     \
			(m->ctx->flags & IRC_MOD_GLOBAL) ||                 \
			util_check_perms(m->ctx->name, params[0], id)       \
		){                                                      \
			IRC_MOD_CALL(m, ptr, args);                         \
		}                                                       \
	}

/*********************************
 * Required forward declarations *
 *********************************/

IRC_STR_CALLBACK(on_join);
IRC_STR_CALLBACK(on_part);

static const char* core_get_datafile(void);
static IPCAddress* util_ipc_add(const char* name);
static void        util_ipc_del(const char* name);

/****************
 * Helper funcs *
 ****************/

static void* util_log_thread_main(void* _arg){
	int* fds = _arg;
	int orig_stdout = fds[0];
	int pipe_fd = fds[1];

	char time_buf[64];
	char c;

	bool do_time = true;

	while(true){
		do {
			ssize_t result = read(pipe_fd, &c, 1);
			if(result <= 0){
				dprintf(orig_stdout, "read: %s\n", strerror(errno));
				continue;
			}

			if(do_time){
				time_t now = time(0);
				size_t time_size = strftime(time_buf, sizeof(time_buf), "[%F %T] ", localtime(&now));
				if(write(orig_stdout, time_buf, time_size) == -1){
					perror("log_thread write");
				}
				do_time = false;
			}

			if(write(orig_stdout, &c, 1) == -1){
				perror("log_thread write");
			}
		} while(c != '\n');

		do_time = true;
	}

	return NULL;
}

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
		if(!m->ctx->on_meta) continue;
		ret &= IRC_MOD_CALL(m, on_meta, (mod, chan, id));
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
				IRC_MOD_CALL(m, on_cmd, (chan, name, msg + sz, cmd_list - m->ctx->commands));
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
				IRC_MOD_CALL_ALL(on_msg_out, (cmd.chan, cmd.data));
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

static void util_module_add(const char* name){
	char path_buf[PATH_MAX];
	const char* path = name;

	if(!strchr(path, '/')){
		if(strlen(inotify.module.path) + strlen(name) + 1 > sizeof(path_buf)) return;

		strcpy(path_buf, inotify.module.path);
		strcat(path_buf, name);
		path = path_buf;
	}

	Module m = {
		.lib_path = strdup(path),
		.needs_reload = true
	};

	sb_push(irc_modules, m);
}

static void util_module_save(Module* m){
	if(!m->ctx || !m->ctx->on_save) return;

	// change the inotify data watch to something we don't care about to disable it temporarily
	inotify.data.wd = inotify_add_watch(inotify.fd, inotify.data.path, IN_DELETE_SELF);

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

	inotify.data.wd = inotify_add_watch(inotify.fd, inotify.data.path, IN_CLOSE_WRITE | IN_MOVED_TO);
}

static Module* util_module_get(const char* name, int type){
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		const char* base_path = basename(m->lib_path);
		const char* comparison
			= (type == MOD_GET_CTXNAME)	? m->ctx->name
			: (type == MOD_GET_SONAME)  ? base_path
			: NULL;

		if(strcmp(name, comparison) == 0){
			return m;
		}
	}
	return NULL;
}

static int util_mod_sort(const void* a, const void* b){
	return ((Module*)b)->ctx->priority - ((Module*)a)->ctx->priority;
}

static void util_reload_modules(const IRCCoreCtx* core_ctx){

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(!m->needs_reload) continue;
		mod_list_dirty = true;

		const char* mod_name = basename(m->lib_path);
		printf("Loading module %s\n", mod_name);

		if(m->lib_handle){
			util_module_save(m);
			IRC_MOD_CALL(m, on_quit, ());
			dlclose(m->lib_handle);
		}

		dlerror();
		m->lib_handle = dlopen(m->lib_path, RTLD_LAZY | RTLD_LOCAL);

		const char* errmsg = "NULL lib handle";
		if(m->lib_handle && !(errmsg = dlerror())){
			m->ctx = dlsym(m->lib_handle, "irc_mod_ctx");
			errmsg = dlerror();
		}

		if(errmsg){
			fprintf(stderr, "** Error loading module %s: %s\n", mod_name, errmsg);
			if(m->lib_handle){
				dlclose(m->lib_handle);
				m->lib_handle = NULL;
			}
			m->ctx = NULL;
			continue;
		}
	}

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(!m->needs_reload) continue;
		m->needs_reload = false;

		const char* mod_name = basename(m->lib_path);
		printf("Init %s...\n", mod_name);

		if(!IRC_MOD_CALL(m, on_init, (core_ctx))){
			printf("** Init failed for %s.\n", mod_name);
			sb_erase(irc_modules, m - irc_modules);
			--m;
			continue;
		}

		for(int i = 0; i < sb_count(channels) - 1; ++i){
			const char** c = (const char**)channels + i;

			irc_on_join(irc_ctx, "join", bot_nick, c, 1);
			for(int j = 0; j < sb_count(chan_nicks[i]); ++j){
				irc_on_join(irc_ctx, "join", chan_nicks[i][j], c, 1);
			}
		}
		puts("... Init successful.");
	}

	qsort(irc_modules, sb_count(irc_modules), sizeof(*irc_modules), &util_mod_sort);
}

static void util_inotify_add(INotifyWatch* watch, const char* path, uint32_t flags){

	if(access(path, W_OK) != 0){
		printf("Creating dir [%s] for inotify.\n", path);
		if(mkdir(path, 00750) == -1){
			perror("Error creating dir");
		}
	}

	watch->path = strdup(path);
	watch->wd   = inotify_add_watch(inotify.fd, path, flags);
}

static void util_inotify_check(const IRCCoreCtx* core_ctx){
	struct inotify_event* ev;
	char buff[sizeof(*ev) + NAME_MAX + 1];

	ssize_t num_read = read(inotify.fd, &buff, sizeof(buff));

	for(char* p = buff; (p - buff) < num_read; p += (sizeof(*ev) + ev->len)){
		ev = (struct inotify_event*)p;

		if(ev->wd == inotify.module.wd){
			Module* m = util_module_get(ev->name, MOD_GET_SONAME);
			if(m){
				m->needs_reload = true;
			} else {
				util_module_add(ev->name);
			}
		} else if(ev->wd == inotify.data.wd){
			if(!memmem(ev->name, ev->len, ".data", 6)) continue;
			fprintf(stderr, "%s was modified.\n", ev->name);

			Module* m = util_module_get(strndupa(ev->name, strlen(ev->name) - 5), MOD_GET_CTXNAME);
			if(m){
				m->data_modified = true;
			}
		} else if(ev->wd == inotify.ipc.wd){
			struct sockaddr_un addr;

			assert(strlen(ev->name) + strlen(inotify.ipc.path) + 1 < sizeof(addr.sun_path));

			strcpy(addr.sun_path, inotify.ipc.path);
			strcat(addr.sun_path, ev->name);

			if(ev->mask & IN_DELETE){
				util_ipc_del(addr.sun_path);
			} else {
				util_ipc_add(addr.sun_path);
			}
		}
	}

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(!m->data_modified) continue;
		m->data_modified = false;

		fprintf(stderr, "Calling on_data_modified for %s\n", m->ctx->name);
		IRC_MOD_CALL(m, on_modified, ());
	}

	util_reload_modules(core_ctx);
}

static void util_ipc_init(void){

	char ipc_dir[128];
	struct stat st;

	// get dir to store ipc sockets

	const char* ipc_dir_prefix   = getenv("XDG_RUNTIME_DIR");
	const char  ipc_dir_suffix[] = "/insobot/";

	if(!ipc_dir_prefix || stat(ipc_dir_prefix, &st) == -1 || !S_ISDIR(st.st_mode)){
		ipc_dir_prefix = "/tmp";
	}

	if(strlen(ipc_dir_prefix) + sizeof(ipc_dir_suffix) + 8 > sizeof(ipc_self.addr.sun_path)){
		fprintf(stderr, "IPC dir name too long!");
		return;
	}

	strcpy(ipc_dir, ipc_dir_prefix);
	strcat(ipc_dir, ipc_dir_suffix);

	if(stat(ipc_dir, &st) == -1){
		if(mkdir(ipc_dir, 0777) == -1){
			perror("ipc_init: mkdir");
		}
	}

	// create our socket

	ipc_self.id = getpid();
	ipc_self.addr.sun_family = AF_UNIX;

	snprintf(ipc_self.addr.sun_path, sizeof(ipc_self.addr.sun_path), "%s%d", ipc_dir, ipc_self.id);
	unlink(ipc_self.addr.sun_path);

	printf("IPC socket: %s\n", ipc_self.addr.sun_path);

	ipc_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
	if(ipc_socket == -1){
		perror("ipc_init: socket");
	}

	if(bind(ipc_socket, &ipc_self.addr, sizeof(ipc_self.addr)) == -1){
		perror("ipc_init: bind");
	}

	// get all the peer addresses in the dir

	util_inotify_add(&inotify.ipc, ipc_dir, IN_CREATE | IN_DELETE | IN_MOVED_TO);

	strcat(ipc_dir, "*");

	glob_t glob_data;
	if(glob(ipc_dir, 0, NULL, &glob_data) != 0){
		perror("ipc_init: glob");
	} else {
		for(size_t i = 0; i < glob_data.gl_pathc; ++i){
			if(strcmp(ipc_self.addr.sun_path, glob_data.gl_pathv[i]) == 0) continue;

			IPCAddress peer = {
				.id   = atoi(basename(glob_data.gl_pathv[i])),
				.addr = { .sun_family = AF_UNIX }
			};

			printf("ipc_init: adding peer %d [%s]\n", peer.id, glob_data.gl_pathv[i]);

			strcpy(peer.addr.sun_path, glob_data.gl_pathv[i]);
			sb_push(ipc_peers, peer);
		}

		globfree(&glob_data);
	}
}

static IPCAddress* util_ipc_add(const char* name){
	for(int i = 0; i < sb_count(ipc_peers); ++i){
		if(strcmp(ipc_peers[i].addr.sun_path, name) == 0){
			return ipc_peers + i;
		}
	}

	IPCAddress peer = {
		.id   = atoi(basename(name)),
		.addr = {
			.sun_family = AF_UNIX
		}
	};

	strcpy(peer.addr.sun_path, name);
	sb_push(ipc_peers, peer);

	printf("ipc_add: %d: [%s]\n", peer.id, peer.addr.sun_path);

	return &sb_last(ipc_peers);
}

static void util_ipc_del(const char* name){
	for(int i = 0; i < sb_count(ipc_peers); ++i){
		if(strcmp(ipc_peers[i].addr.sun_path, name) == 0){
			sb_erase(ipc_peers, i);
			break;
		}
	}
}

static void util_ipc_recv(void){
	char buffer[4096];
	struct sockaddr_un addr;
	socklen_t addr_len = sizeof(addr);

	ssize_t num = recvfrom(ipc_socket, buffer, sizeof(buffer), 0, &addr, &addr_len);
	if(num == -1){
		perror("ipc_recv: recvfrom");
		return;
	}

	assert(addr.sun_family == AF_UNIX);

	IPCAddress* peer = util_ipc_add(addr.sun_path);

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(strncmp(buffer, m->ctx->name, num) == 0){
			const size_t off = strlen(m->ctx->name) + 1;
			IRC_MOD_CALL(m, on_ipc, (peer->id, (uint8_t*)(buffer + off), num - off));
		}
	}
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
		printf("Unknown event:\n:: %s :: %s", event, origin);
		for(int i = 0; i < count; ++i){
			printf(" :: %s", params[i]);
		}
		puts("");
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
		printf(":: [%03u] :: %s", event, origin);
		for(int i = 0; i < count; ++i){
			printf(" :: %s", params[i]);
		}
		puts("");
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
		inotify.data.wd = inotify_add_watch(inotify.fd, inotify.data.path, IN_DELETE_SELF);
		close(creat(datafile_buff, 00600));
		inotify.data.wd = inotify_add_watch(inotify.fd, inotify.data.path, IN_CLOSE_WRITE | IN_MOVED_TO);
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

static void core_send_ipc(int target, const void* data, size_t data_len){
	if(ipc_socket <= 0) return;

	Module* m = sb_count(mod_call_stack) ? sb_last(mod_call_stack) : NULL;
	const char* name = m ? m->ctx->name : "core";
	const size_t name_len = strlen(name);
	const size_t total_len = data_len + name_len + 1;

	// why doesn't AF_UNIX support MSG_MORE? :(
	char* buffer = malloc(total_len);
	memcpy(buffer, name, name_len + 1);
	memcpy(buffer + name_len + 1, data, data_len);

	for(IPCAddress* p = ipc_peers; p < sb_end(ipc_peers); ++p){
		if(target != 0 && p->id != target) continue;

		if(sendto(ipc_socket, buffer, total_len, 0, &p->addr, sizeof(p->addr)) == -1){
			bool remove = false;

			if(errno == ECONNREFUSED){
				unlink(p->addr.sun_path);
				remove = true;
			} else if(errno == ENOENT){
				remove = true;
			} else {
				perror("send_ipc: sendto");
			}

			if(remove){
				printf("removing [%s]\n", p->addr.sun_path);
				sb_erase(ipc_peers, p - ipc_peers);
				--p;
			}
		}
	}

	free(buffer);
}

static void core_send_mod_msg(IRCModMsg* msg){
	const char* sender = sb_last(mod_call_stack)->ctx->name;
	IRC_MOD_CALL_ALL(on_mod_msg, (sender, msg));
}

static void core_self_save(void){
	util_module_save(sb_last(mod_call_stack));
}

static void core_log(const char* fmt, ...){

	if(getenv("INSOBOT_NO_CRAZY_TIMESTAMPS")){
		char time_buf[64];
		time_t now = time(0);
		struct tm* now_tm = localtime(&now);
		strftime(time_buf, sizeof(time_buf), "[%F][%T]", now_tm);

		const char* mod_name = sb_count(mod_call_stack) ? sb_last(mod_call_stack)->ctx->name : "CORE";
		fprintf(stderr, "%s %s: ", time_buf, mod_name);
	}

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

	// timestamp thread setup

	pthread_t log_thread;
	
	if(!getenv("INSOBOT_NO_CRAZY_TIMESTAMPS")){
		int fds[3] = { dup(STDOUT_FILENO) };

		if(pipe(fds + 1) == -1){
			perror("pipe");
			abort();
		}

		dup2(fds[2], STDOUT_FILENO);
		dup2(fds[2], STDERR_FILENO);

		setlinebuf(stdout);
		setlinebuf(stderr);

		pthread_create(&log_thread, NULL, &util_log_thread_main, fds);
	}

	// path setup

	char our_path[PATH_MAX] = {};

	if(readlink("/proc/self/exe", our_path, sizeof(our_path) - 1) == -1){
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

	// inotify init

	inotify.fd = inotify_init1(IN_NONBLOCK);

	memcpy(path_end, in_mod_suffix, sizeof(in_mod_suffix));
	util_inotify_add(&inotify.module, our_path, IN_CLOSE_WRITE | IN_MOVED_TO);

	memcpy(path_end, in_dat_suffix, sizeof(in_dat_suffix));
	util_inotify_add(&inotify.data, our_path, IN_CLOSE_WRITE | IN_MOVED_TO);

	// ipc & curl init

	util_ipc_init();

	curl_global_init(CURL_GLOBAL_ALL);

	// find modules

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

	printf("Found %zu modules\n", glob_data.gl_pathc);

	for(int i = 0; i < glob_data.gl_pathc; ++i){
		util_module_add(glob_data.gl_pathv[i]);
	}

	globfree(&glob_data);

	// modules init

	const IRCCoreCtx core_ctx = {
		.get_username = &core_get_username,
		.get_datafile = &core_get_datafile,
		.get_modules  = &core_get_modules,
		.get_channels = &core_get_channels,
		.send_msg     = &core_send_msg,
		.send_raw     = &core_send_raw,
		.send_ipc     = &core_send_ipc,
		.send_mod_msg = &core_send_mod_msg,
		.join         = &core_join,
		.part         = &core_part,
		.save_me      = &core_self_save,
		.log          = &core_log,
	};

	sb_push(channels, 0);

	util_reload_modules(&core_ctx);
	
	if(sb_count(irc_modules) == 0){
		errx(1, "No modules could be loaded.");
	}

	// irc init

	user = util_env_else("IRC_USER", DEFAULT_BOT_NAME);
	pass = util_env_else("IRC_PASS", NULL);
	serv = util_env_else("IRC_SERV", "irc.nonexistent.domain");
	port = util_env_else("IRC_PORT", "6667");
	bot_nick = strdup(user);

	irc_callbacks_t callbacks = {
		.event_connect = irc_on_connect,
		.event_channel = irc_on_chat_msg,
		.event_join    = irc_on_join,
		.event_part    = irc_on_part,
		.event_nick    = irc_on_nick,
		.event_numeric = irc_on_numeric,
		.event_unknown = irc_on_unknown,
	};

	// outer main loop, (re)set irc state

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
			asprintf_check(&libirc_serv, "#%s", serv);

			//XXX: you might not want this!
			irc_option_set(irc_ctx, LIBIRC_OPTION_SSL_NO_VERIFY);
		} else {
			libirc_serv = strdup(serv);
		}

		if(irc_connect(irc_ctx, libirc_serv, atoi(port), pass, user, user, user) != 0){
			fprintf(stderr, "Unable to connect: %s\n", irc_strerror(irc_errno(irc_ctx)));
		}

		free(libirc_serv);

		// inner main loop

		while(running && irc_is_connected(irc_ctx)){

			util_process_pending_cmds();

			util_inotify_check(&core_ctx);

			//TODO: check on_meta & better timing for on_tick?
			IRC_MOD_CALL_ALL(on_tick, ());

			int max_fd = 0;
			fd_set in, out;
	
			FD_ZERO(&in);
			FD_ZERO(&out);
			FD_SET(STDIN_FILENO, &in);
			FD_SET(ipc_socket, &in);

			if(irc_add_select_descriptors(irc_ctx, &in, &out, &max_fd) != 0){
				fprintf(stderr, "Error adding select fds: %s\n", irc_strerror(irc_errno(irc_ctx)));
			}

			struct timeval tv = {
				.tv_sec  = 0,
				.tv_usec = 250000,
			};
	
			int ret = select(max_fd + 1, &in, &out, NULL, &tv);
				
			if(ret > 0){
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

				if(FD_ISSET(ipc_socket, &in)){
					util_ipc_recv();
				}
			} else if(ret){
				perror("select");
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
			usleep(10000000L);
		}
	} while(running);

	// clean stuff up so real leaks are more obvious in valgrind

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		util_module_save(m);
		IRC_MOD_CALL(m, on_quit, ());
		free(m->lib_path);
		dlclose(m->lib_handle);
	}

	sb_free(irc_modules);
	sb_free(chan_mod_list);
	sb_free(global_mod_list);
	sb_free(mod_call_stack);
	sb_free(cmd_queue);

	curl_global_cleanup();

	for(size_t i = 0; i < sb_count(channels) - 1; ++i){
		free(channels[i]);
		for(size_t j = 0; j < sb_count(chan_nicks[i]); ++j){
			free(chan_nicks[i][j]);
		}
		sb_free(chan_nicks[i]);
	}
	sb_free(channels);
	sb_free(chan_nicks);

	free(bot_nick);

	free(inotify.module.path);
	free(inotify.data.path);
	free(inotify.ipc.path);

	if(ipc_socket > 0){
		close(ipc_socket);
		unlink(ipc_self.addr.sun_path);
	}
	sb_free(ipc_peers);

	if(!getenv("INSOBOT_NO_CRAZY_TIMESTAMPS")){
		pthread_cancel(log_thread);
		pthread_join(log_thread, NULL);
	}

	return 0;
}
