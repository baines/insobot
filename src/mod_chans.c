#include <stdlib.h>
#include <string.h>
#include "module.h"
#include "config.h"
#include "stb_sb.h"
#include "inso_utils.h"

static bool chans_init    (const IRCCoreCtx* ctx);
static void chans_cmd     (const char*, const char*, const char*, int);
static void chans_join    (const char*, const char*);
static bool chans_save    (FILE*);
static void chans_connect (const char*);
static void chans_quit    (void);

enum { CHAN_JOIN, CHAN_LEAVE };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "chans",
	.desc       = "Allow people to add/remove the bot to/from their channel",
	.flags      = IRC_MOD_GLOBAL,
	.on_init    = &chans_init,
	.on_cmd     = &chans_cmd,
	.on_join    = &chans_join,
	.on_save    = &chans_save,
	.on_connect = &chans_connect,
	.on_quit    = &chans_quit,
	.commands   = DEFINE_CMDS (
		[CHAN_JOIN]  = CONTROL_CHAR "join",
		[CHAN_LEAVE] = CONTROL_CHAR "leave " CONTROL_CHAR "part"
	)
};

static const IRCCoreCtx* ctx;

static char** join_list;

static inline const char* env_else(const char* env, const char* def){
	const char* c = getenv(env);
	return c ? c : def;
}

static bool chans_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void chans_quit(void){
	for(size_t i = 0; i < sb_count(join_list); ++i){
		free(join_list[i]);
	}
	sb_free(join_list);
}

static void chans_cmd(const char* chan, const char* name, const char* arg, int cmd){

	size_t name_len = strlen(name);
	char* name_chan = alloca(name_len + 2);
	name_chan[0] = '#';
	memcpy(name_chan + 1, name, name_len + 1);

	bool can_use_leave = strcasecmp(chan + 1, name) == 0 || inso_is_admin(ctx, name);

	switch(cmd){
		case CHAN_JOIN: {
			if(*arg) break;

			bool in_chan = false;
			for(const char** c = ctx->get_channels(); *c; ++c){
				if(strcasecmp(name, (*c)+1) == 0){
					in_chan = true;
					break;
				}
			}

			if(in_chan){
				ctx->send_msg(chan, "%s: I should already be there, I'll try rejoining.", name);
			} else {
				ctx->send_msg(chan, "%s: Joining your channel. Use " CONTROL_CHAR "leave in there to undo.", name);
			}
			ctx->join(name_chan);
		} break;

		case CHAN_LEAVE: {
			if(!can_use_leave) break;

			bool in_chan = false;
			for(const char** c = ctx->get_channels(); *c; ++c){
				if(strcasecmp(chan, *c) == 0){
					in_chan = true;
					break;
				}
			}

			if(in_chan){
				ctx->send_msg(chan, "Goodbye, %s.", name);
			} else {
				ctx->send_msg(chan, "Wait, I'm still here? I'll try leaving again...");
			}
			ctx->part(chan);
		} break;
	}
}

static void chans_join(const char* chan, const char* name){
	if(strcmp(name, ctx->get_username()) != 0) return;

	for(int i = 0; i < sb_count(join_list); ++i){
		if(strcmp(join_list[i], chan) == 0){
			free(join_list[i]);
			sb_erase(join_list, i);
			break;
		}
	}

	if(sb_count(join_list)){
		char* c = sb_last(join_list);
		ctx->join(c);
		free(c);
		sb_pop(join_list);
	}

	if(!sb_count(join_list)){
		ctx->save_me();
	}
}

static bool chans_save(FILE* file){
	for(const char** c = ctx->get_channels(); *c; ++c){
		fprintf(file, "%s\n", *c);
	}
	return true;
}

static void chans_connect(const char* serv){

	if(strcasecmp(serv, "irc.chat.twitch.tv") == 0 || getenv("IRC_IS_TWITCH")){
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
		sb_push(join_list, strdup(c));
	} while((c = strtok_r(NULL, ", ", &state)));

	free(channels);

	char file_chan[256];

	FILE* f = fopen(ctx->get_datafile(), "rb");
	while(fscanf(f, "%255s", file_chan) == 1){
		printf("mod_chans: Joining %s (file)\n", file_chan);
		sb_push(join_list, strdup(file_chan));
	}
	fclose(f);

	if(sb_count(join_list)){
		char* c = sb_last(join_list);
		ctx->join(c);
		free(c);
		sb_pop(join_list);
	}
}
