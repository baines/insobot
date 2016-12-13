#include "module.h"
#include "config.h"
#include "inso_utils.h"
#include <string.h>
#include <ctype.h>

static bool admin_init (const IRCCoreCtx*);
static void admin_cmd  (const char*, const char*, const char*, int);
static void admin_stdin(const char*);

enum { FORCE_JOIN };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "admin",
	.desc     = "Miscellaneous admin commands",
	.flags    = IRC_MOD_GLOBAL,
	.on_init  = admin_init,
	.on_cmd   = &admin_cmd,
	.on_stdin = &admin_stdin,
	.commands = DEFINE_CMDS (
		[FORCE_JOIN] = CONTROL_CHAR "fjoin " CONTROL_CHAR "join"
	)
};

static const IRCCoreCtx* ctx;

static bool admin_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void admin_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool real_admin = strcmp(name, BOT_OWNER) == 0 || inso_is_admin(ctx, name);
	if(!real_admin) return;

	switch(cmd){
		case FORCE_JOIN: {
			if(!*arg++) break;

			size_t arg_len = strlen(arg);
			char* join_chan = alloca(arg_len + 2);
			memcpy(join_chan + 1, arg, arg_len + 1);
			*join_chan = '#';

			if(!isalnum(*arg)){
				++join_chan;
			}

			ctx->send_msg(chan, "@%s: joining %s.", name, join_chan);
			ctx->join(join_chan);

		} break;
	}
}

static void admin_stdin(const char* text){
	// TODO: administrative stdin commands
}
