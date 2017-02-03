#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "assert.h"

static bool help_init (const IRCCoreCtx*);
static void help_cmd  (const char*, const char*, const char*, int);

enum { CMD_HELP };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "help",
	.desc     = "Shows available commands",
	.flags    = IRC_MOD_GLOBAL,
	.on_init  = help_init,
	.on_cmd   = &help_cmd,
	.commands = DEFINE_CMDS (
		[CMD_HELP] = CONTROL_CHAR "help"
	)
};

static const IRCCoreCtx* ctx;

static bool help_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void get_mod_cmds(IRCModuleCtx* mod, char* buf, size_t sz){
	if(!mod->commands || !*mod->commands){
		inso_strcat(buf, sz, "<none>.");
		return;
	}

	char cmd_buf[256];

	//XXX: only prints the first command alias, is that a good idea?
	for(const char** cmd = mod->commands; *cmd; ++cmd){
		const char* end = strchrnul(*cmd, ' ');
		assert(end - *cmd < isizeof(cmd_buf));
		memcpy(cmd_buf, *cmd, end - *cmd);
		cmd_buf[end - *cmd] = 0;

		inso_strcat(buf, sz, cmd_buf);
		inso_strcat(buf, sz, " ");
	}

}

static void help_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(cmd != CMD_HELP) return;

	//TODO: description of each command?

	IRCModuleCtx** mods = ctx->get_modules(false);
	char mod_names[256] = {};

	for(IRCModuleCtx** m = mods; *m; ++m){
		if((*m)->commands && (*m)->commands[0]){
			inso_strcat(mod_names, sizeof(mod_names), (*m)->name);
			if(m[1]){
				inso_strcat(mod_names, sizeof(mod_names), ", ");
			} else {
				inso_strcat(mod_names, sizeof(mod_names), ".");
			}
		}
	}

	if(!*arg++){
		ctx->send_msg(chan, "%s: Use " CONTROL_CHAR "help <module> to list its commands. Available modules: %s", name, mod_names);
	} else {
		const char* mod_end = strchrnul(arg, ' ');
		char* mod = strndupa(arg, mod_end - arg);

		bool found = false;
		for(IRCModuleCtx** m = mods; *m; ++m){
			if(strcasecmp(mod, (*m)->name) == 0){
				char cmds[256] = {};
				get_mod_cmds(*m, cmds, sizeof(cmds));
				ctx->send_msg(chan, "%s: Commands for %s: %s", name, mod, cmds);
				found = true;
				break;
			}
		}

		if(!found){
			ctx->send_msg(chan, "%s: I haven't heard of that module.", name);
		}
	}
}
