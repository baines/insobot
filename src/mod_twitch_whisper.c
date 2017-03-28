#include "module.h"
#include "config.h"
#include "stb_sb.h"
#include <regex.h>
#include <string.h>

// TODO: relay in the other direction too (whisper -> irc pm)

static bool whisper_init    (const IRCCoreCtx*);
static void whisper_filter  (size_t, const char*, char*, size_t);

const IRCModuleCtx irc_mod_ctx = {
	.name      = "whisper",
	.desc      = "Relay normal IRC PMs to Twitch's whisper system",
	.priority  = -100,
	.flags     = IRC_MOD_GLOBAL,
	.on_init   = &whisper_init,
	.on_filter = &whisper_filter,
};

static const IRCCoreCtx* ctx;

static bool whisper_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	const char* id = getenv("INSOBOT_ID");

	if(!id || !strstr(id, "twitch")){
		fprintf(stderr, "mod_twitch_whisper: This doesn't look like a twitch server, exiting.\n");
		return false;
	} else {
		ctx->send_raw("CAP REQ :twitch.tv/commands");
		return true;
	}
}

static void whisper_filter(size_t id, const char* chan, char* msg, size_t len){
	if(chan && *chan != '#'){
		ctx->send_msg("#jtv", "/w %s %s", chan, msg);
		*msg = 0;
	}
}
