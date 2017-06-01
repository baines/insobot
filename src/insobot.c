#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <glob.h>
#include <dlfcn.h>
#include <link.h>
#include <execinfo.h>

#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <libircclient.h>
#include <libirc_rfcnumeric.h>
#include <curl/curl.h>

#include "config.h"
#include "module.h"
#include "stb_sb.h"
#include "inso_utils.h"

#ifndef LIBIRC_OPTION_SSL_NO_VERIFY
	#define LIBIRC_OPTION_SSL_NO_VERIFY (1 << 3)
#endif

// XXX: hack for older gcc
#if !defined(__GNUC__) || __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 9)
	#define __auto_type intptr_t
#endif

_Static_assert(sizeof(intptr_t)   == sizeof(void*), "uh oh");
_Static_assert(sizeof(void (*)()) == sizeof(void*), "uh oh");

/******************************
 * Types, global vars, macros *
 * ****************************/

typedef struct Module_ {
	char* lib_path;
	void* lib_handle;
	IRCModuleCtx* ctx;
	size_t ctx_size;
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
	size_t id;
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
static size_t last_cmd_id;

static irc_session_t* irc_ctx;

static Module* irc_modules;
static Module** mod_call_stack;
static IRCModuleCtx** chan_mod_list;
static IRCModuleCtx** global_mod_list;

static char* modules_include;
static char* modules_exclude;

static const char *user, *pass, *serv, *port;
static char* bot_nick;

static char**  channels;
static char*** chan_nicks;

static INotifyData inotify;

static struct timeval idle_tv;
static bool ping_sent;

static int         ipc_socket;
static IPCAddress  ipc_self;
static IPCAddress* ipc_peers;

static sig_atomic_t running = 1;

static bool send_msg_called;

static char   irc_tag_buf[512];
static char** irc_tag_ptrs;
static bool   have_tag_hack;

static int pipe_fds[2];
static int debug_pipe[2];
static const char* debug_chan;

static char* insobot_path;

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

#define IRC_MOD_CALL_ALL_ABI(ptr, args, abi)                    \
	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){ \
		if(ABI_CHECK(m, abi)) IRC_MOD_CALL(m, ptr, args);       \
	}

#define ABI_FILTER  24
#define ABI_UNKNOWN 25
#define ABI_HELP    27
#define ABI_CHECK(m, abi) ((m)->ctx_size >= (sizeof(void*)*(abi)))

/*********************************
 * Required forward declarations *
 *********************************/

IRC_STR_CALLBACK(on_join);
IRC_STR_CALLBACK(on_part);

static const char* core_get_datafile(void);
static IPCAddress* util_ipc_add(const char* name);
static void        util_ipc_del(const char* name);
static void        util_module_filter_update(void);
static bool        util_module_filter_allowed(const char*);


/****************
 * Helper funcs *
 ****************/

static void util_log_proc(int fd){
	char text_buf[1024];
	char time_buf[64];

	FILE* f = fdopen(dup(fd), "r");

	while(running){
		if(!fgets(text_buf, sizeof(text_buf), f)) break;
		time_t now = time(0);
		strftime(time_buf, sizeof(time_buf), "[%F %T]", localtime(&now));
		printf("%s %s", time_buf, text_buf);
	}

	fclose(f);
}

static void util_handle_sig(int n){
	if(n == SIGSEGV){
		void* buf[32];
		int size = backtrace(buf, 32);
		fputs("########## SIGSEGV BACKTRACE ##########\n", stderr);
		backtrace_symbols_fd(buf, size, STDERR_FILENO);
		fputs("##########        END        ##########\n", stderr);

		signal(SIGSEGV, SIG_DFL);

		// notify our future self about how we died... 🤔
		if(getenv("INSOBOT_DEBUG_CHAN") && size > 2){
			backtrace_symbols_fd(buf + 2, 1, debug_pipe[1]);
		}
	} else {
		running = 0;
	}
}

static void util_multiprocess_init(void){

	if(getenv("INSOBOT_DEBUG_CHAN")){
		if(pipe(debug_pipe) == -1){
			perror("pipe failed");
			exit(1);
		}
	}

restart:
	if(pipe(pipe_fds) == -1){
		perror("pipe failed");
		exit(1);
	}

	pid_t pid = fork();
	if(pid == -1){
		perror("fork failed");
		exit(1);
	}

	// parent process -> will loop in util_log_proc, then either exit or goto restart.
	if(pid != 0){
		signal(SIGINT , SIG_IGN);
		signal(SIGPIPE, &util_handle_sig);

		close(pipe_fds[1]);
		prctl(PR_SET_NAME, "ib-parent");

		util_log_proc(pipe_fds[0]);
		close(pipe_fds[0]);

		int status;
		wait(&status);

		int exitnum = 0;

		if(WIFEXITED(status)){
#if __SANITIZE_ADDRESS__
			exitnum = 0;
#else
			exitnum = WEXITSTATUS(status);
#endif
			printf("insobot exited. (%d)\n", exitnum);

		} else if(WIFSIGNALED(status)){
			int sig = WTERMSIG(status);
			printf("Somebody set up us the bomb. We get signal: %d (%s).\n", sig, sys_siglist[sig]);

			if(WCOREDUMP(status)){
				puts("On the bright side, we apparently dumped a core somewhere.");
			}

			exitnum = (sig == SIGTERM) ? 0 : sig;
		}

		if(!getenv("INSOBOT_NO_AUTO_RESTART") && exitnum != 0){
			puts("Gonna try to auto restart...");
			signal(SIGINT , SIG_DFL);
			if(usleep(5000000) == -1){
				exit(1);
			}
			goto restart;
		}

		exit(exitnum);
	}

	// child process -> points stdout/stderr into the parent pipe, and returns back to main.

	prctl(PR_SET_NAME, "insobot");
	prctl(PR_SET_PDEATHSIG, SIGINT);

	close(pipe_fds[0]);
	dup2(pipe_fds[1], STDOUT_FILENO);
	dup2(pipe_fds[1], STDERR_FILENO);

	setlinebuf(stdout);
	setlinebuf(stderr);
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
				break;
			}

			while(*cmd_end == ' ') ++cmd_end;
			cmd = cmd_end;
		} while(*cmd_end);
	}
}

static size_t util_cmd_enqueue(int cmd, const char* chan, const char* data){
	if(sb_count(cmd_queue) > CMD_QUEUE_MAX) return 0;

	size_t id = ++last_cmd_id;
	if(!id) ++id;

	IRCCmd c = {
		.id   = id,
		.cmd  = cmd,
		.chan = chan ? strdup(chan) : NULL,
		.data = data ? strdup(data) : NULL
	};

	sb_push(cmd_queue, c);

	return c.id;
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
//				irc_on_join(irc_ctx, "join", cmd.data, (const char**)&cmd.chan, 1);
			} break;

			case IRC_CMD_PART: {
				irc_cmd_part(irc_ctx, cmd.chan);
//				irc_on_part(irc_ctx, "part", cmd.data, (const char**)&cmd.chan, 1);
			} break;

			case IRC_CMD_MSG: {
				size_t len = strlen(cmd.data);
				IRC_MOD_CALL_ALL_ABI(on_filter, (cmd.id, cmd.chan, cmd.data, len), ABI_FILTER);
				if(*cmd.data){
					printf("send: [%s] [%s]\n", cmd.chan, cmd.data);
					irc_cmd_msg(irc_ctx, cmd.chan, cmd.data);
					IRC_MOD_CALL_ALL(on_msg_out, (cmd.chan, cmd.data));
				}
			} break;

			case IRC_CMD_RAW: {
				size_t len = strlen(cmd.data);
				IRC_MOD_CALL_ALL_ABI(on_filter, (cmd.id, NULL, cmd.data, len), ABI_FILTER);
				if(*cmd.data){
					irc_send_raw(irc_ctx, "%s", cmd.data);
				}
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
		if(snprintf(path_buf, PATH_MAX, "%s%s", inotify.module.path, name) >= PATH_MAX) return;
		path = path_buf;
	}

	if(util_module_filter_allowed(path)){
		Module m = {
			.lib_path = strdup(path),
			.needs_reload = true
		};

		sb_push(irc_modules, m);
	} else {
		printf("NOTE: Loading module '%s' cancelled due to filter.\n", basename(path));
	}
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

		const char* mod_name = basename(m->lib_path);

		if(m->lib_handle){
			util_module_save(m);
			IRC_MOD_CALL(m, on_quit, ());
			dlclose(m->lib_handle);
		}

		if(!util_module_filter_allowed(mod_name)){
			printf("Module '%s' is now filtered. Unloading.\n", mod_name);
			free(m->lib_path);
			sb_erase(irc_modules, m - irc_modules);
			--m;
			continue;
		}

		dlerror();
		m->lib_handle = dlopen(m->lib_path, RTLD_LAZY | RTLD_LOCAL);

		printf("Loading module %-20s", mod_name);

		const char* errmsg = dlerror();
		if(!errmsg && m->lib_handle){
			m->ctx = dlsym(m->lib_handle, "irc_mod_ctx");
			errmsg = dlerror();
		} else if(!errmsg && !m->lib_handle){
			errmsg = "NULL lib handle.";
		}

		if(!errmsg){
			const ElfW(Sym)* sym = NULL;
			Dl_info unused;
			if(dladdr1(m->ctx, &unused, (void**)&sym, RTLD_DL_SYMENT) == 0){
				errmsg = "dladdr1 failed.";
			} else if(sym->st_size < (sizeof(void*)*23)) {
				
				// NOTE: ABI Table for IRCModuleCtx:
				//
				//       | sizeof(void*) | last field |
				//       +---------------+------------+
				//       |      x23      | on_ipc     |
				//       |      x24      | on_filter  |
				//       |      x25      | on_unknown |
				//       |      x27      | help_url   |

				errmsg = "version mismatch (wrong size irc_mod_ctx)";
			} else {
				m->ctx_size = sym->st_size;
			}
		}

		if(errmsg){
			puts("");
			fprintf(stderr, "** Error loading module %s:\n  %s\n", mod_name, errmsg);
			if(m->lib_handle){
				dlclose(m->lib_handle);
				m->lib_handle = NULL;
			}
			free(m->lib_path);
			sb_erase(irc_modules, m - irc_modules);
			--m;
		} else {
			struct link_map* mod_info = m->lib_handle;
			printf("[0x%zx]\n", (size_t)mod_info->l_addr);
		}
	}

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		if(!m->needs_reload) continue;
		m->needs_reload = false;

		const char* mod_name = basename(m->lib_path);
		printf("Init %s...\n", mod_name);

		if(!IRC_MOD_CALL(m, on_init, (core_ctx))){
			printf("** Init failed for %s.\n", mod_name);
			dlclose(m->lib_handle);
			m->lib_handle = NULL;
			free(m->lib_path);
			sb_erase(irc_modules, m - irc_modules);
			--m;
			continue;
		}

		if(irc_ctx && irc_is_connected(irc_ctx)){
			IRC_MOD_CALL(m, on_connect, (serv));
		}

		for(size_t i = 0; i < sb_count(channels) - 1; ++i){
			const char** c = (const char**)channels + i;

			IRC_MOD_CALL(m, on_join, (*c, bot_nick));
			for(size_t j = 0; j < sb_count(chan_nicks[i]); ++j){
				IRC_MOD_CALL(m, on_join, (*c, chan_nicks[i][j]));
			}
		}
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
	bool reload = false;
	bool updated_filter = false;

	ssize_t num_read = read(inotify.fd, &buff, sizeof(buff));

	for(char* p = buff; (p - buff) < num_read; p += (sizeof(*ev) + ev->len)){
		ev = (struct inotify_event*)p;

		if(ev->wd == inotify.module.wd){
			if(!updated_filter){
				updated_filter = true;
				util_module_filter_update();
			}

			Module* m = util_module_get(ev->name, MOD_GET_SONAME);
			if(m){
				m->needs_reload = true;
			} else {
				util_module_add(ev->name);
			}
			reload = true;
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

	if(reload){
		util_reload_modules(core_ctx);
	}
}

static void util_ipc_init(void){
	char ipc_dir[128];
	struct stat st;

	// get dir to store ipc sockets

	const char* ipc_dir_prefix   = getenv("XDG_RUNTIME_DIR");
	const char  ipc_dir_suffix[] = "/insobot/";

	if(!ipc_dir_prefix || stat(ipc_dir_prefix, &st) == -1 || !S_ISDIR(st.st_mode) || access(ipc_dir_prefix, W_OK) == -1){
		ipc_dir_prefix = "/run/shm";
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

	// check for stale peers

	for(IPCAddress* p = ipc_peers; p < sb_end(ipc_peers); ++p){
		if(connect(ipc_socket, &p->addr, sizeof(p->addr)) == -1){
			if(errno == ECONNREFUSED){
				unlink(p->addr.sun_path);
			}
			sb_erase(ipc_peers, p - ipc_peers);
			--p;
		} else {
			struct sockaddr sa = { .sa_family = AF_UNSPEC };
			connect(ipc_socket, &sa, sizeof(sa));
		}
	}
}

static IPCAddress* util_ipc_add(const char* name){
	for(size_t i = 0; i < sb_count(ipc_peers); ++i){
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
	for(size_t i = 0; i < sb_count(ipc_peers); ++i){
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
			printf("Got IPC msg from %d for %s\n", peer->id, m->ctx->name);
			const size_t off = strlen(m->ctx->name) + 1;
			IRC_MOD_CALL(m, on_ipc, (peer->id, (uint8_t*)(buffer + off), num - off));
		}
	}
}

static void util_find_chan_nick(const char* chan, const char* nick, int* chan_idx, int* nick_idx){

	if(chan_idx) *chan_idx = -1;
	if(nick_idx) *nick_idx = -1;

	for(size_t i = 0; i < sb_count(channels) - 1; ++i){
		if(strcasecmp(channels[i], chan) != 0) continue;

		if(chan_idx) *chan_idx = i;

		for(size_t j = 0; j < sb_count(chan_nicks[i]); ++j){
			if(strcasecmp(chan_nicks[i][j], nick) == 0){
				if(nick_idx) *nick_idx = j;
				break;
			}
		}

		break;
	}
}

static void util_trim_end_spaces(char* msg, size_t len){
	if(len > 0){
		for(char* p = msg + len - 1; p >= msg && *p == ' '; --p){
			*p = 0;
		}
	}
}

static void util_update_tags(const char** params){
	if(!have_tag_hack) return;
	strncpy(irc_tag_buf, params[-1], sizeof(irc_tag_buf)-1);

	if(irc_tag_ptrs){
		stb__sbn(irc_tag_ptrs) = 0;
	}

	char* state;
	char* k = strtok_r(irc_tag_buf, ";", &state);

	for(; k; k = strtok_r(NULL, ";", &state)){
		char* v = k;

		while(*v && *v != '=') ++v;

		if(*v == '='){
			*v++ = '\0';

			for(char* p = v; *p; ++p){
				if(*p != '\\') continue;

				if(p[1] == ':') *p = ';';
				else if(p[1] == 's') *p = ' ';
				else if(p[1] == 'r') *p = '\r';
				else if(p[1] == 'n') *p = '\n';

				for(char* q = p + 1; *q; ++q){
					q[0] = q[1];
				}
			}
		}

		sb_push(irc_tag_ptrs, k);
		sb_push(irc_tag_ptrs, v);
	}
}

static char* util_file_read(const char* name){
	char* mem = NULL;
	long len = 0;
	FILE* f = fopen(name, "r");
	if(!f) return NULL;

	if(fseek(f, 0, SEEK_END) == -1) goto end;
	if((len = ftell(f)) == -1) goto end;
	if(fseek(f, 0, SEEK_SET) == -1) goto end;

	mem = malloc(len + 1);
	assert(mem);

	fread(mem, len, 1, f);
	mem[len] = 0;

end:
	fclose(f);
	return mem;
}

static bool util_module_filter_update_file(const char* name, char** list){
	char pathbuf[PATH_MAX];
	snprintf(pathbuf, sizeof(pathbuf), "%s/%s", insobot_path, name);

	if(*list) stb__sbn(*list) = 0;

	char* mem = util_file_read(pathbuf);
	if(mem){
		char* state;

		for(char* line = strtok_r(mem, "\r\n", &state); line; line = strtok_r(NULL, "\r\n", &state)){
			if(strchr(line, '/')) continue;

			size_t len = strlen(line);
			sb_push(*list, '/');
			memcpy(sb_add(*list, len), line, len);
		}

		if(sb_count(*list)){
			sb_push(*list, 0);
		}

		free(mem);
	} else if(errno != ENOENT){
		fprintf(stderr, "Error reading %s: %m\n", name);
	}

	return sb_count(*list) > 0;
}

static void util_module_filter_update(void){
	bool inc = util_module_filter_update_file("modules.include", &modules_include);
	bool exc = util_module_filter_update_file("modules.exclude", &modules_exclude);
	if(inc && exc){
		printf("WARNING: Both modules.include and modules.exclude exist. Only modules.include will be used.\n");
	}
}

static bool util_module_filter_allowed(const char* module){
	char* base = strdupa(basename(module));
	if(strncmp(base, "mod_", 4) == 0){
		base += 4;
	}

	{
		char* p = strrchr(base, '.');
		if(p && strcmp(p, ".so") == 0){
			*p = 0;
		}
	}

	size_t mod_len = strlen(base);
	char* p = sb_count(modules_include) ? modules_include : modules_exclude;
	bool ret = p == modules_include;

	if(p){
		while((p = strstr(p, base))){
			if(p && p[-1] == '/' && (p[mod_len] == '/' || !p[mod_len])){
				return ret;
			}
			++p;
		}
		return !ret;
	} else {
		return true;
	}
}

void util_check_data_migrate(const char* path){
	char oldpath[PATH_MAX];
	char newpath[PATH_MAX];

	snprintf(oldpath, sizeof(oldpath), "%s/modules/data", path);
	snprintf(newpath, sizeof(newpath), "%s/data", path);

	if(access(newpath, F_OK) == 0){
		// new location already exists, skip.
		return;
	}

	struct stat st;
	if(stat(oldpath, &st) == 0 && S_ISDIR(st.st_mode)){
		printf("******************************************************************************\n"
		       "* The location of module data files has changed from /modules/data to /data. *\n"
		       "* This allows the modules directory to be symlinked and used by multiple bot *\n"
		       "*           instances, while each bot keeps its own set of data.             *\n"
		       "*   Press Y to move this directory, or N to keep it where it is currently.   *\n"
		       "*     If not moved, the bot will start with a blank set of module data.      *\n"
		       "******************************************************************************");
	}

	int opt = 0;
	if(isatty(STDOUT_FILENO)){
		do {
			if(opt != '\n'){
				printf("\nMove the data directory? (Y/N) > ");
				fflush(stdout);
			}
			opt = getchar();
		} while(!strchr("yYnN", opt));
	} else {
		printf("stdout isn't a tty, assuming Y.\n");
		opt = 'y';
	}

	if(opt == 'y' || opt == 'Y'){
		if(rename(oldpath, newpath) == -1){
			perror("rename");
		} else {
			printf("Successfully moved data directory.\n");
		}
	} else {
		printf("Keeping old data directory in place.\n");
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

	util_update_tags(params);

	const char *_chan = params[0], *_name = origin;

	size_t msglen = strlen(params[1]);
	char*  msgbuf = alloca(msglen+2);
	char*  _msg   = msgbuf + 1;

	// null-prefix the msg, so that cmds can walk backwards to see the full msg
	*msgbuf = 0;
	memcpy(_msg, params[1], msglen+1);

	util_trim_end_spaces(_msg, msglen);

	send_msg_called = false;

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

IRC_STR_CALLBACK(on_action) {
	if(count < 2 || !params[0] || !params[1]) return;

	util_update_tags(params);

	const char *_chan = params[0], *_name = origin;
	char* _msg = strdupa(params[1]);
	util_trim_end_spaces(_msg, strlen(_msg));

	IRC_MOD_CALL_ALL_CHECK(on_action, (_chan, _name, _msg), IRC_CB_ACTION);
}

IRC_STR_CALLBACK(on_pm){
	if(count < 2 || !params[1] || !origin) return;

	util_update_tags(params);

	const char* _name = origin;
	char* _msg = strdupa(params[1]);
	util_trim_end_spaces(_msg, strlen(_msg));

	IRC_MOD_CALL_ALL(on_pm, (_name, _msg));
}

IRC_STR_CALLBACK(on_join) {
	if(count < 1 || !origin || !params[0]) return;
	fprintf(stderr, "JOIN: %s %s\n", params[0], origin);

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

	// if we're joining the debug channel, set the global so we know we can now send stuff
	const char* c = getenv("INSOBOT_DEBUG_CHAN");
	if(c && strcmp(origin, bot_nick) == 0 && strcmp(params[0], c) == 0){
		debug_chan = c;
	}

	//XXX: can't use CHECK here unless our own name bypasses it FIXME
	IRC_MOD_CALL_ALL(on_join, (params[0], origin));
}

IRC_STR_CALLBACK(on_part) {
	if(count < 1 || !origin || !params[0]) return;

	int chan_i, nick_i;
	util_find_chan_nick(params[0], origin, &chan_i, &nick_i);

	printf("PART: %s %s\n", params[0], origin);

	if(chan_i != -1 && strcasecmp(origin, bot_nick) == 0){
		free(channels[chan_i]);
		sb_erase(channels, chan_i);

		for(size_t i = 0; i < sb_count(chan_nicks[chan_i]); ++i){
			free(chan_nicks[chan_i][i]);
		}
		sb_free(chan_nicks[chan_i]);
		sb_erase(chan_nicks, chan_i);

	} else if(nick_i != -1){
		free(chan_nicks[chan_i][nick_i]);
		sb_erase(chan_nicks[chan_i], nick_i);
	}

	IRC_MOD_CALL_ALL_CHECK(on_part, (params[0], origin), IRC_CB_PART);
}

IRC_STR_CALLBACK(on_quit) {
	if(!origin) return;

	util_update_tags(params);

	printf("QUIT: %s\n", origin);

	for(size_t i = 0; i < sb_count(channels) - 1; ++i){
		for(size_t j = 0; j < sb_count(chan_nicks[i]); ++j){
			if(strcmp(origin, chan_nicks[i][j]) == 0){
				free(chan_nicks[i][j]);
				sb_erase(chan_nicks[i], j);

				IRC_MOD_CALL_ALL_CHECK(on_part, (channels[i], origin), IRC_CB_PART);
				break;
			}
		}
	}
}

IRC_STR_CALLBACK(on_nick) {
	if(count < 1 || !origin || !params[0]) return;

	util_update_tags(params);

	if(strcmp(origin, bot_nick) == 0){
		printf("We changed nicks! new nick: %s\n", params[0]);
		free(bot_nick);
		bot_nick = strdup(params[0]);
	}

	for(size_t i = 0; i < sb_count(channels) - 1; ++i){
		for(size_t j = 0; j < sb_count(chan_nicks[i]); ++j){
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
//		printf(":: PONG");
		return;
	} else {
		printf("Unknown event:\n:: %s :: %s", event, origin);
	}

	for(size_t i = 0; i < count; ++i){
		printf(" :: %s", params[i]);
	}
	puts("");

	IRC_MOD_CALL_ALL_ABI(on_unknown, (event, origin, params, count), ABI_UNKNOWN);
}

IRC_NUM_CALLBACK(on_numeric) {
	static const char nick_start_symbols[] = "[]\\`_^{|}";
	
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
		for(size_t i = 0; i < count; ++i){
			printf(" :: %s", params[i]);
		}
		puts("");
	}
}

/********************
 * IRCCoreCtx funcs *
 ********************/

static intptr_t core_get_info(int id){
	switch(id){
		case IRC_INFO_CAN_PARSE_TAGS: {
			return have_tag_hack;
		} break;

		default: {
			return 0;
		} break;
	}
}

static const char* core_get_username(void){
	return bot_nick;
}

//FIXME: would it be better to return a malloc'd string instead?
static char datafile_buff[PATH_MAX];

static const char* core_get_datafile(void){
	Module* caller = sb_last(mod_call_stack);

	snprintf(datafile_buff, sizeof(datafile_buff), "%s/data/%s.data", insobot_path, caller->ctx->name);

	if(access(datafile_buff, F_OK) != 0){
		inotify.data.wd = inotify_add_watch(inotify.fd, inotify.data.path, IN_DELETE_SELF);
		close(creat(datafile_buff, 00600));
		inotify.data.wd = inotify_add_watch(inotify.fd, inotify.data.path, IN_CLOSE_WRITE | IN_MOVED_TO);
	}

	if(access(datafile_buff, R_OK | W_OK) != 0){
		fprintf(stderr, "FATAL: We don't have rw permission on '%s'! Please chown/chmod it.\n", datafile_buff);
		exit(0); // use 0 so we don't get auto-restarted..
	}

	return datafile_buff;
}

static IRCModuleCtx** core_get_modules(bool chan_only){
	Module* caller = sb_last(mod_call_stack);

	if(chan_mod_list) stb__sbn(chan_mod_list) = 0;
	if(global_mod_list) stb__sbn(global_mod_list) = 0;

	for(Module* m = irc_modules; m < sb_end(irc_modules); ++m){
		// XXX: don't return modules with a lower ABI than the caller for safety.
		if(caller && caller->ctx_size > m->ctx_size) continue;

		sb_push(global_mod_list, m->ctx);
		if(!(m->ctx->flags & IRC_MOD_GLOBAL)){
			sb_push(chan_mod_list, m->ctx);
		}
	}

	sb_push(chan_mod_list, 0);
	sb_push(global_mod_list, 0);

	return chan_only ? chan_mod_list : global_mod_list;
}

static const char** core_get_channels(void){
	return (const char**)channels;
}

static const char** core_get_nicks(const char* chan, int* count){
	assert(count);

	int index = -1;
	for(size_t i = 0; i < sb_count(channels) - 1; ++i){
		if(strcasecmp(channels[i], chan) == 0){
			index = i;
			break;
		}
	}

	if(index >= 0){
		*count = sb_count(chan_nicks[index]);
		return (const char**)chan_nicks[index];
	} else {
		*count = 0;
		return NULL;
	}
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

static size_t core_send_msg(const char* chan, const char* fmt, ...){
	if(!chan || !fmt) return 0;

	size_t id = 0;
	char buff[1024];
	va_list v;

	va_start(v, fmt);
	if(vsnprintf(buff, sizeof(buff), fmt, v) > 0){
		id = util_cmd_enqueue(IRC_CMD_MSG, chan, buff);
	}
	va_end(v);

	send_msg_called = true;

	return id;
}

static size_t core_send_raw(const char* raw){
	return util_cmd_enqueue(IRC_CMD_RAW, NULL, raw);
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

		printf("Sending IPC msg to %d for %s\n", p->id, name);

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

	if(getenv("INSOBOT_NO_FORK")){
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

static void core_strip_colors(char* msg){
	char* stripped = irc_color_strip_from_mirc(msg);
	assert(strlen(stripped) <= strlen(msg));

	memcpy(msg, stripped, strlen(stripped) + 1);
	free(stripped);
}

static bool core_responded(void){
	return send_msg_called;
}

static bool core_get_tag(size_t index, const char** k, const char** v){
	index <<= 1;

	if(index >= sb_count(irc_tag_ptrs)){
		return false;
	}

	if(k) *k = irc_tag_ptrs[index+0];
	if(v) *v = irc_tag_ptrs[index+1];

	return true;
}

static void core_gen_event(int which, ...){
	va_list va;
	va_start(va, which);

	// TODO: get tags in here?
	const char* pbuf[3] = { "" };
	const char** params = pbuf + 1;
	const char* origin;

	// TODO: queue this up in events like cmd_queue, and process later

	switch(which){
		case IRC_CB_MSG: {
			params[0] = va_arg(va, const char*); // chan
			origin    = va_arg(va, const char*); // name
			params[1] = va_arg(va, const char*); // msg

			irc_on_chat_msg(irc_ctx, "", origin, params, 2);
		} break;

		case IRC_CB_JOIN: {
			params[0] = va_arg(va, const char*); // chan;
			origin    = va_arg(va, const char*); // name;

			irc_on_join(irc_ctx, "", origin, params, 1);
		} break;

		case IRC_CB_PART: {
			params[0] = va_arg(va, const char*); // chan;
			origin    = va_arg(va, const char*); // name;

			irc_on_part(irc_ctx, "", origin, params, 1);
		} break;

		case IRC_CB_ACTION: {
			params[0] = va_arg(va, const char*); // chan
			origin    = va_arg(va, const char*); // name
			params[1] = va_arg(va, const char*); // msg

			irc_on_action(irc_ctx, "", origin, params, 2);
		} break;

		case IRC_CB_NICK: {
			origin    = va_arg(va, const char*); // prev_nick
			params[0] = va_arg(va, const char*); // new_nick

			irc_on_nick(irc_ctx, "", origin, params, 1);
		} break;

		case IRC_CB_PM: {
			// params[0] unused?
			origin    = va_arg(va, const char*); // name
			params[1] = va_arg(va, const char*); // msg

			irc_on_pm(irc_ctx, "", origin, params, 2);
		} break;
	}

	va_end(va);
}

/***************
 * entry point *
 * *************/

int main(int argc, char** argv){

	// path setup

	static char our_path[PATH_MAX] = {};

	if(readlink("/proc/self/exe", our_path, sizeof(our_path) - 1) == -1){
		err(errno, "Can't read path");
	}

	char* path_end = strrchr(our_path, '/');
	if(!path_end){
		path_end = our_path + 1;
		*our_path = '.';
	}
	*path_end = 0;

	static const char in_mod_suffix[] = "/modules/";
	static const char in_dat_suffix[] = "/data/";
	static const char glob_suffix[]   = "/modules/*.so";

	if(path_end + sizeof(in_dat_suffix) >= our_path + sizeof(our_path)){
		errx(1, "Path too long!");
	}

	util_check_data_migrate(our_path);

	// parent process setup

	if(!getenv("INSOBOT_NO_FORK")){
		util_multiprocess_init(); // *** NOTE: only the child process will return from this function ***
	}

	srand(time(0));
	signal(SIGSEGV, &util_handle_sig);
	signal(SIGINT , &util_handle_sig);
	signal(SIGPIPE, SIG_IGN);
	assert(setlocale(LC_CTYPE, "C.UTF-8"));

	insobot_path = strdup(our_path);

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

	util_module_filter_update();
	for(size_t i = 0; i < glob_data.gl_pathc; ++i){
		util_module_add(glob_data.gl_pathv[i]);
	}

	globfree(&glob_data);

	// modules init

	static const IRCCoreCtx core_ctx = {
		.api_version  = INSO_CORE_API_VERSION,
		.get_info     = &core_get_info,
		.get_username = &core_get_username,
		.get_datafile = &core_get_datafile,
		.get_modules  = &core_get_modules,
		.get_channels = &core_get_channels,
		.get_nicks    = &core_get_nicks,
		.send_msg     = &core_send_msg,
		.send_raw     = &core_send_raw,
		.send_ipc     = &core_send_ipc,
		.send_mod_msg = &core_send_mod_msg,
		.join         = &core_join,
		.part         = &core_part,
		.save_me      = &core_self_save,
		.log          = &core_log,
		.strip_colors = &core_strip_colors,
		.responded    = &core_responded,
		.get_tag      = &core_get_tag,
		.gen_event    = &core_gen_event,
	};

	sb_push(channels, 0);

	// check for patched lib with ircv3 tag parsing hack
	{
		unsigned irc_maj = 0, irc_min = 0;
		irc_get_version(&irc_maj, &irc_min);
		if(irc_maj == 1 && irc_min == 0x1b07){
			have_tag_hack = 1;
		}
	}

	// initial load of modules

	util_reload_modules(&core_ctx);
	
	if(sb_count(irc_modules) == 0){
		errx(0, "No modules could be loaded.");
	}

	// irc init

	user = util_env_else("IRC_USER", DEFAULT_BOT_NAME);
	pass = util_env_else("IRC_PASS", NULL);
	serv = util_env_else("IRC_SERV", "irc.nonexistent.domain");
	port = util_env_else("IRC_PORT", "6667");
	bot_nick = strdup(user);

	static irc_callbacks_t callbacks = {
		.event_connect     = irc_on_connect,
		.event_channel     = irc_on_chat_msg,
		.event_privmsg     = irc_on_pm,
		.event_join        = irc_on_join,
		.event_part        = irc_on_part,
		.event_quit        = irc_on_quit,
		.event_nick        = irc_on_nick,
		.event_ctcp_action = irc_on_action,
		.event_numeric     = irc_on_numeric,
		.event_unknown     = irc_on_unknown,
	};

	// outer main loop, (re)set irc state

	do {
		if(!(irc_ctx = irc_create_session(&callbacks))){
			fprintf(stderr, "Failed to create irc session.\n");
			exit(1);
		}

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

			//TODO: check on_meta & better timing for on_tick?
			time_t now = time(0);
			IRC_MOD_CALL_ALL(on_tick, (now));

			int max_fd = 0;
			fd_set in, out;
	
			FD_ZERO(&in);
			FD_ZERO(&out);

			FD_SET(STDIN_FILENO, &in);
			FD_SET(ipc_socket  , &in);
			FD_SET(inotify.fd  , &in);

			max_fd = INSO_MAX(max_fd, STDIN_FILENO);
			max_fd = INSO_MAX(max_fd, ipc_socket);
			max_fd = INSO_MAX(max_fd, inotify.fd);

			if(debug_chan && debug_pipe[0]){
				FD_SET(debug_pipe[0], &in);
				max_fd = INSO_MAX(max_fd, debug_pipe[0]);
			}

			if(irc_add_select_descriptors(irc_ctx, &in, &out, &max_fd) != 0){
				fprintf(stderr, "Error adding select fds: %s\n", irc_strerror(irc_errno(irc_ctx)));
			}

			struct timeval tv = {
				.tv_sec  = 0,
				.tv_usec = 250000,
			}, orig_tv = tv;

			int select_status = select(max_fd + 1, &in, &out, NULL, &tv);

			if(select_status > 0){

				if(FD_ISSET(STDIN_FILENO, &in)){
					FD_CLR(STDIN_FILENO, &in);
					char stdin_buf[1024];
					ssize_t n = read(STDIN_FILENO, stdin_buf, sizeof(stdin_buf));
					if(n > 0){
						stdin_buf[n-1] = 0; // remove \n
						IRC_MOD_CALL_ALL(on_stdin, (stdin_buf));
					}
				}

				if(FD_ISSET(ipc_socket, &in)){
					FD_CLR(ipc_socket, &in);
					util_ipc_recv();
				}

				if(FD_ISSET(inotify.fd, &in)){
					FD_CLR(inotify.fd, &in);
					util_inotify_check(&core_ctx);
				}

				if(FD_ISSET(debug_pipe[0], &in)){
					FD_CLR(debug_pipe[0], &in);
					char buf[256];
					char* fname;
					int off;
					size_t n = read(debug_pipe[0], buf, sizeof(buf)-1);

					if(n > 0 && sscanf(buf, "%m[^(]%n", &fname, &off) == 1){
						buf[n-1] = 0; // remove \n
						core_send_msg(debug_chan, "Recovered from crash: %s%s", basename(fname), buf + off);
						free(fname);
					}
				}

				for(int i = 0; i < max_fd + 1; ++i){
					if(FD_ISSET(i, &in)){
						timerclear(&idle_tv);
						ping_sent = 0;
						break;
					}
				}

				if(irc_process_select_descriptors(irc_ctx, &in, &out) != 0){
					fprintf(stderr, "Error processing select fds: %s\n", irc_strerror(irc_errno(irc_ctx)));
				}

			} else if(select_status == 0){

				struct timeval ping_tv    = { .tv_sec = 60 };
				struct timeval restart_tv = { .tv_sec = 90 };

				timeradd(&orig_tv, &idle_tv, &idle_tv);

				if(!ping_sent && timercmp(&idle_tv, &ping_tv, >)){
					irc_send_raw(irc_ctx, "PING %s", serv);
					ping_sent = 1;
				} else if(ping_sent && timercmp(&idle_tv, &restart_tv, >)){
					puts("Reached 'no PONG' threshold, disconnecting."); 
					irc_disconnect(irc_ctx);
				}

			} else {
				perror("select");
			}
		}
	
		irc_destroy_session(irc_ctx);
		timerclear(&idle_tv);
		ping_sent = 0;

		if(running){
			puts("Restarting.");
			if(getenv("INSOBOT_NO_AUTO_RESTART")){
				puts("(when you press a key...)");
				getchar();
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
		m->lib_handle = NULL;
	}

	sb_free(irc_modules);
	sb_free(chan_mod_list);
	sb_free(global_mod_list);
	sb_free(mod_call_stack);
	sb_free(cmd_queue);
	sb_free(irc_tag_ptrs);

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

	if(pipe_fds[1]){
		close(pipe_fds[1]);
	}

	free(insobot_path);

	return 0;
}
