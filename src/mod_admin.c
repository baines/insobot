#include "module.h"
#include "string.h"
#include "config.h"

static bool admin_init (const IRCCoreCtx*);
static void admin_cmd  (const char*, const char*, const char*, int);

enum { FORCE_JOIN };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "admin",
	.desc     = "Miscellaneous admin commands",
	.flags    = IRC_MOD_GLOBAL,
	.on_init  = admin_init,
	.on_cmd   = &admin_cmd,
	.commands = DEFINE_CMDS (
		[FORCE_JOIN] = CONTROL_CHAR "fjoin"
	)
};

static const IRCCoreCtx* ctx;

static bool admin_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void admin_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
}

static void admin_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool real_admin = strcmp(name, BOT_OWNER) == 0;
	if(!real_admin) MOD_MSG(ctx, "check_admin", name, &admin_cb, &real_admin);
	if(!real_admin) return;

	switch(cmd){
		case FORCE_JOIN: {
			if(!*arg++) break;
			ctx->send_msg(chan, "@%s: joining %s.", BOT_OWNER, arg);
			ctx->join(arg);
		} break;
	}
}
