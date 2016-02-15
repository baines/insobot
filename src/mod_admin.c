#include "module.h"
#include "string.h"
#include "config.h"

static bool admin_init (const IRCCoreCtx*);
static void admin_msg  (const char*, const char*, const char*);

const IRCModuleCtx irc_mod_ctx = {
	.name    = "admin",
	.desc    = "Miscellaneous admin commands",
	.flags   = IRC_MOD_GLOBAL,
	.on_init = admin_init,
	.on_msg  = &admin_msg,
};

const IRCCoreCtx* ctx;

static bool admin_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void admin_msg(const char* chan, const char* name, const char* msg){
	if(strcmp(name, BOT_OWNER) != 0) return;

	enum { FORCE_JOIN };

	const char* arg = msg;
	int i = ctx->check_cmds(&arg, "\\fjoin", NULL);

	switch(i){
		case FORCE_JOIN: {
			if(!*arg++) break;
			ctx->send_msg(chan, "@%s: joining %s.", BOT_OWNER, arg);
			ctx->join(arg);
		} break;
	}
}
