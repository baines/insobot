#include "module.h"
#include "inso_utils.h"
#include "stb_sb.h"
#include <sys/types.h>
#include <sys/wait.h>

static void units_cmd  (const char*, const char*, const char*, int);
static bool units_init (const IRCCoreCtx*);

enum { CONVERT };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "units",
	.desc     = "Convert units",
	.flags    = IRC_MOD_GLOBAL,
	.on_cmd   = &units_cmd,
	.on_init  = &units_init,
	.commands = DEFINE_CMDS(
		[CONVERT] = CMD("units") CMD("convert") CMD("cvt")
	),
	.cmd_help = DEFINE_CMDS(
		[CONVERT] = "<X> to <Y> | Convert from unit X to Y"
	),
};

static const IRCCoreCtx* ctx;

static void units_cmd(const char* chan, const char* name, const char* arg, int cmd){
	const char *from = NULL, *to = NULL;

	static const char* delims[] = {
		" to ", " TO ", " in ", " IN ", " -> "
	};

	for(size_t i = 0; i < ARRAY_SIZE(delims); ++i){
		const char* c;
		if((c = strstr(arg, delims[i]))){
			from = strndupa(arg+1, c - (arg+1));
			to = c + 4;
			break;
		}
	}

	if(!from){
		ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "cvt <unit_a> -> <unit_b>", name);
		return;
	}

	int p[2];
	if(pipe(p) == -1){
		perror("pipe");
		return;
	}

	pid_t pid = vfork();
	if(pid == -1){
		perror("vfork");
	} else if(pid == 0){
		dup2(p[1], STDOUT_FILENO);
		if(execlp("units", "units", "-l", "en_US.utf-8", "-t", "--", from, to, NULL) == -1){
			perror("exec");
			_exit(1);
		}
	} else {
		int status = -1;
		waitpid(pid, &status, 0);
		char buf[256];

		if(status == 0){
			ssize_t n = read(p[0], buf, sizeof(buf));
			if(n == -1){
				perror("read");
			} else if(n > 0){
				buf[n-1] = 0;
				ctx->send_msg(chan, "%s: %s %s", name, buf, to);
			}
		} else {
			ctx->send_msg(chan, "%s: Unknown or mismatched units :(", name);
		}
	}

	close(p[0]);
	close(p[1]);
}

static bool units_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}
