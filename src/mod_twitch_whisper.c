#include "module.h"
#include "config.h"
#include "stb_sb.h"
#include <regex.h>
#include <string.h>

// TODO: relay in the other direction too (whisper -> irc pm)

static bool whisper_init    (const IRCCoreCtx*);
static void whisper_filter  (size_t, const char*, char*, size_t);
static void whisper_connect (const char*);
static void whisper_unknown (const char*, const char*, const char**, size_t);

const IRCModuleCtx irc_mod_ctx = {
	.name       = "whisper",
	.desc       = "Relay normal IRC PMs to Twitch's whisper system",
	.priority   = -100,
	.flags      = IRC_MOD_GLOBAL,
	.on_init    = &whisper_init,
	.on_filter  = &whisper_filter,
	.on_connect = &whisper_connect,
	.on_unknown = &whisper_unknown,
};

static const IRCCoreCtx* ctx;

static bool whisper_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	const char* id = getenv("INSOBOT_ID");

	if(!id || !strstr(id, "twitch")){
		fprintf(stderr, "mod_twitch_whisper: This doesn't look like a twitch server, exiting.\n");
		return false;
	} else {
		return true;
	}
}

static void whisper_connect(const char* serv){
	ctx->send_raw("CAP REQ :twitch.tv/commands");
}

static void whisper_filter(size_t id, const char* chan, char* msg, size_t len){
	if(chan && *chan != '#'){
		ctx->send_msg("#jtv", "/w %s %s", chan, msg);
		*msg = 0;
	}
}

static void whisper_unknown(const char* event, const char* origin, const char** params, size_t n){
	if(strcmp(event, "WHISPER") == 0 && origin && n >= 2){
		ctx->gen_event(IRC_CB_PM, origin, params[1]);
	}
}
