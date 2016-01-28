#include "module.h"
#include <stdlib.h>
#include <string.h>

static bool chans_init(const IRCCoreCtx* ctx);
static void chans_connect(const char*);

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

static inline const char* env_else(const char* env, const char* def){
	const char* c = getenv(env);
	return c ? c : def;
}

static bool chans_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void chans_connect(const char* serv){
	//TODO: read from file instead

	const char* chans = env_else("IRC_CHAN", "#test");	

	char *channels = strdup(chans),
	     *state = NULL,
	     *c = strtok_r(channels, ", ", &state);

	if(strcasecmp(serv, "irc.twitch.tv") == 0 || getenv("IRC_DO_TWITCH_CAP")){
		ctx->send_raw("CAP REQ :twitch.tv/membership");
	}

	char* nspass = getenv("IRC_NICKSERV_PASS");
	if(nspass){
		ctx->send_msg("nickserv", "IDENTIFY %s", nspass);
	}

	do {
		printf("Joining %s\n", c);
		ctx->join(c);
	} while((c = strtok_r(NULL, ", ", &state)));

	free(channels);
}

/*

	\join
	\leave

*/
