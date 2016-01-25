#include "module.h"

static bool chans_init(const IRCCoreCtx* ctx);
static void chans_connect(void);

const IRCModuleCtx irc_mod_ctx = {
	.name       = "chans",
	.desc       = "Allow people to add/remove the bot to/from their channel",
	.flags      = IRC_MOD_GLOBAL,
	.on_init    = &chans_init,
//	.on_msg     = &chans_msg,
//	.on_save    = &chans_save,
	.on_connect = &chans_connect,
};

static const IRCCoreCtx* ctx;

static bool chans_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void chans_connect(void){
	ctx->join("#test");
}

/*

	\join
	\leave

*/
