#include <stdlib.h>
#include <string.h>
#include "module.h"
#include "config.h"

static bool chans_init    (const IRCCoreCtx* ctx);
static void chans_msg     (const char*, const char*, const char*);
static void chans_join    (const char*, const char*);
static bool chans_save    (FILE*);
static void chans_connect (const char*);

const IRCModuleCtx irc_mod_ctx = {
	.name       = "chans",
	.desc       = "Allow people to add/remove the bot to/from their channel",
	.flags      = IRC_MOD_GLOBAL,
	.on_init    = &chans_init,
	.on_msg     = &chans_msg,
	.on_join    = &chans_join,
	.on_save    = &chans_save,
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

static void admin_check_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
}

static void chans_msg(const char* chan, const char* name, const char* msg){

	const char* arg = msg;
	enum { CHAN_JOIN, CHAN_LEAVE };
	int cmd = ctx->check_cmds(&arg, "\\join", "\\part,\\leave", NULL);

	bool in_chan = false;
	for(const char** c = ctx->get_channels(); *c; ++c){
		if(strcasecmp(name, (*c)+1) == 0){
			in_chan = true;
			break;
		}
	}

	size_t name_len = strlen(name);
	char* name_chan = alloca(name_len + 2);
	name_chan[0] = '#';
	memcpy(name_chan + 1, name, name_len + 1);

	bool can_use_leave = strcasecmp(chan + 1, name) == 0;
	MOD_MSG(ctx, "check_admin", name, &admin_check_cb, &can_use_leave);

	switch(cmd){
		case CHAN_JOIN: {
			if(in_chan){
				ctx->send_msg(chan, "%s: I should already be there, I'll try rejoining.", name);
			} else {
				ctx->send_msg(chan, "%s: Joining your channel.", name);
			}
			ctx->join(name_chan);
		} break;

		case CHAN_LEAVE: {
			if(!can_use_leave) break;

			if(in_chan){
				ctx->send_msg(chan, "Goodbye, %s.", name);
			} else {
				ctx->send_msg(chan, "Wait, I'm still here? I'll try leaving again...");
			}
			ctx->part(chan);
		} break;
	}
}

static bool suppress_join_save = false;

static void chans_join(const char* chan, const char* name){
	if(strcmp(name, ctx->get_username()) != 0 || suppress_join_save) return;
	ctx->save_me();
}

static bool chans_save(FILE* file){
	for(const char** c = ctx->get_channels(); *c; ++c){
		fprintf(file, "%s\n", *c);
	}
	return true;
}

static void chans_connect(const char* serv){

	suppress_join_save = true;

	//TODO: maybe spread the joins out over some time?

	if(strcasecmp(serv, "irc.twitch.tv") == 0 || getenv("IRC_DO_TWITCH_CAP")){
		ctx->send_raw("CAP REQ :twitch.tv/membership");
	}

	char* nspass = getenv("IRC_NICKSERV_PASS");
	if(nspass){
		ctx->send_msg("nickserv", "IDENTIFY %s", nspass);
	}

	const char* env_chans = env_else("IRC_CHAN", "#" BOT_OWNER);

	char *channels = strdup(env_chans),
	     *state = NULL,
	     *c = strtok_r(channels, ", ", &state);

	do {
		printf("mod_chans: Joining %s (env)\n", c);
		ctx->join(c);
	} while((c = strtok_r(NULL, ", ", &state)));

	free(channels);

	char file_chan[256];

	FILE* f = fopen(ctx->get_datafile(), "rb");
	while(fscanf(f, "%255s", file_chan) == 1){
		printf("mod_chans: Joining %s (file)\n", file_chan);
		ctx->join(file_chan);
	}

	suppress_join_save = false;
	ctx->save_me();
}
