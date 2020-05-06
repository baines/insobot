#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "stb_sb.h"
#include "assert.h"
#include "module_msgs.h"
#include <ctype.h>
#include <regex.h>

static void psa_add    (const char*, const char*, bool);
static bool psa_init   (const IRCCoreCtx*);
static void psa_cmd    (const char*, const char*, const char*, int);
static void psa_msg    (const char*, const char*, const char*);
static void psa_tick   (time_t);
static bool psa_save   (FILE*);
static void psa_quit   (void);
static void psa_reload (void);

enum { PSA_ADD, PSA_DEL, PSA_LIST };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "psa",
	.desc     = "Create perodic channel messages",
	.on_init  = &psa_init,
	.on_cmd   = &psa_cmd,
	.on_msg   = &psa_msg,
	.on_tick  = &psa_tick,
	.on_save  = &psa_save,
	.on_quit  = &psa_quit,
	.on_modified = &psa_reload,
	.commands = DEFINE_CMDS (
		[PSA_ADD]  = CMD("psa+"),
		[PSA_DEL]  = CMD("psa-"),
		[PSA_LIST] = CMD("psa")
	),
	.cmd_help = DEFINE_CMDS (
		[PSA_ADD]  = "<name> [+live] [+trigger '<str>'] <N>m <text> | Adds/updates a PSA named <name> to occur every <N> mins."
		             "With +live, only show when the channel is live. With +trigger, only show when <str> is said.",
		[PSA_DEL]  = "<name> | Remove the psa identified by <name>.",
		[PSA_LIST] = "[id] | Show info about a PSA or list them all."
	),
	.help_url = "https://insobot.handmade.network/forums/t/2393",
};

static const IRCCoreCtx* ctx;

typedef struct {
	char* channel;
	char* id;
	char* trigger;
	regex_t trig_rx;
	char* message;
	char* cmdline;
	time_t last_posted;
	int freq_mins;
	bool when_live;
} PSAData;

static PSAData* psa_data;
static time_t psa_last_update;

static void psa_reload(void){
	FILE* file = fopen(ctx->get_datafile(), "r");
	assert(file);

	char cmdline[512];
	char chan[64];

	while(fscanf(file, "%63s %511[^\n]", chan, cmdline) == 2){
		psa_add(chan, cmdline, true);
		printf("mod_psa: loaded [%s] [%s]\n", chan, cmdline);
	}

	fclose(file);
}

static bool psa_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	psa_reload();
	return true;
}

static bool psa_delete(const char* chan, const char* id){
	sb_each(p, psa_data){
		if(strcmp(p->channel, chan) != 0 || strcmp(p->id, id) != 0){
			continue;
		}

		free(p->channel);
		free(p->message);
		free(p->id);
		free(p->trigger);
		regfree(&p->trig_rx);
		free(p->cmdline);
		sb_erase(psa_data, p - psa_data);

		return true;
	}

	return false;
}

static void psa_add(const char* chan, const char* arg, bool silent){

	enum {
		S_BAD_REGEX = -5,
		S_NO_ID  = -4,
		S_NO_MSG = -3,
		S_NEGATIVE_FREQ = -2,
		S_UNKNOWN_TOKEN = -1,
		S_SUCCESS = 0,
		S_ID,
		S_MAIN,
		S_OPT_LIVE,
		S_OPT_TRIGGER,
		S_MSG
	} state = S_ID;

	static const char* errs[] = {
		[-S_BAD_REGEX]     = "Error compiling regex",
		[-S_NO_ID]         = "No psa id/name given",
		[-S_NO_MSG]        = "No message given",
		[-S_NEGATIVE_FREQ] = "The minute frequency must be > 0",
		[-S_UNKNOWN_TOKEN] = "Could not parse command",
	};

	static struct {
		const char* name;
		int state;
	} opts[] = {
		{ "live"   , S_OPT_LIVE },
		{ "trigger", S_OPT_TRIGGER }
	};

	char regerr_buf[256];

	char token[128];
	int len;
	const char* p = arg;
	PSAData psa = {
		.cmdline = (char*)arg
	};

	do {
		switch(state){
			case S_ID: {
				if(sscanf(p, " %127s%n", token, &len) == 1){
					p += len;
					psa.id = strdup(token);
					state = S_MAIN;
				} else {
					state = S_NO_ID;
				}
			} break;

			case S_MAIN: {
				if(sscanf(p, " +%127s%n", token, &len) == 1){
					bool got_opt = false;

					for(size_t i = 0; i < ARRAY_SIZE(opts); ++i){
						if(strcmp(opts[i].name, token) == 0){
							state = opts[i].state;
							got_opt = true;
							break;
						}
					}

					if(got_opt){
						p += len;
					} else {
						state = S_UNKNOWN_TOKEN;
					}
				} else if (sscanf(p, " %dm%n", &psa.freq_mins, &len) == 1){
					if(psa.freq_mins > 0){
						p += len;
						state = S_MSG;
					} else {
						state = S_NEGATIVE_FREQ;
					}
				} else {
					state = S_UNKNOWN_TOKEN;
				}
			} break;

			case S_OPT_LIVE: {
				psa.when_live = true;
				state = S_MAIN;
			} break;

			case S_OPT_TRIGGER: {
				if(sscanf(p, " '%127[^']'%n", token, &len) == 1 || sscanf(p, " %127s%n", token, &len) == 1){
					p += len;

					int err;
					if((err = regcomp(&psa.trig_rx, token, REG_ICASE | REG_EXTENDED | REG_NOSUB))){
						regerror(err, &psa.trig_rx, regerr_buf, sizeof(regerr_buf));
						state = S_BAD_REGEX;
					} else {
						psa.trigger = strdup(token);
						state = S_MAIN;
					}
				} else {
					state = S_NO_MSG;
				}
			} break;

			case S_MSG: {
				if(*p++){
					psa.message = strdup(p);
					state = S_SUCCESS;
				} else {
					state = S_NO_MSG;
				}
			} break;

			default: {
				state = S_UNKNOWN_TOKEN;
			} break;
		}
	} while(state > 0);

	if(state == S_SUCCESS){
		for(char* c = psa.id; *c; ++c){
			*c = tolower(*c);
		}

		psa_delete(chan, psa.id);

		psa.cmdline = strdup(psa.cmdline);
		psa.channel = strdup(chan);
		psa.last_posted = (time(0) + 5) - (psa.freq_mins * 60);
		sb_push(psa_data, psa);

		if(!silent){
			ctx->send_msg(chan, "PSA [%s] Added.", psa.id);
		}

	} else {
		free(psa.id);
		free(psa.trigger);
		regfree(&psa.trig_rx);

		if(!silent){
			if(state == S_BAD_REGEX){
				ctx->send_msg(chan, "Error compiling regex: %s.", regerr_buf);
			} else {
				ctx->send_msg(chan, "Error adding PSA: %s. Use !psa+ with no arguments for usage info.", errs[-state]);
			}
		}
	}
}

static void psa_info(const char* chan, const char* name, const char* id) {
	PSAData* psa = NULL;

	sb_each(p, psa_data) {
		if(strcmp(p->channel, chan) == 0 && strcmp(p->id, id) == 0) {
			psa = p;
			break;
		}
	}

	if(!psa) {
		ctx->send_msg(chan, "%s: psa [%s] not found.", name, id);
		return;
	}

	char buf[1024] = "";
	char* p = buf;
	size_t sz = sizeof(buf);

	if(psa->trigger)
		snprintf_chain(&p, &sz, " (trigger:%s)", psa->trigger);
	if(psa->when_live)
		snprintf_chain(&p, &sz, " (live)");
	if(*psa->message == '!')
		snprintf_chain(&p, &sz, " (alias:%.*s)", (int)strcspn(psa->message, " "), psa->message);

	ctx->send_msg(chan, "psa [%s]: %dm%s", psa->id, psa->freq_mins, buf);

}

static void psa_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(!inso_is_wlist(ctx, name)) return;

	switch(cmd){
		case PSA_ADD: {
			if(*arg){
				psa_add(chan, arg, false);
				ctx->save_me();
			} else {
				ctx->send_msg(chan, "%s: Usage: !psa+ <psa_name> [+live] [+trigger 'phrase'] <freq>m <message>", name);
			}
		} break;

		case PSA_DEL: {
			if(*arg++){
				if(psa_delete(chan, arg)){
					ctx->send_msg(chan, "%s: Deleted psa '%s'", name, arg);
					ctx->save_me();
				} else {
					ctx->send_msg(chan, "%s: Can't find a PSA with id '%s' for this channel.", name, arg);
				}
			} else {
				ctx->send_msg(chan, "%s: Usage: !psa- <psa_name>", name);
			}
		} break;

		case PSA_LIST: {

			if(*arg++) {
				psa_info(chan, name, arg);
				break;
			}

			char psa_buf[512] = "";
			char* psa_ptr = psa_buf;
			size_t psa_sz = sizeof(psa_buf);

			sb_each(p, psa_data){
				if(strcmp(p->channel, chan) != 0)
					continue;
				snprintf_chain(&psa_ptr, &psa_sz, "%s ", p->id);
			}

			if(!*psa_buf)
				strcpy(psa_buf, "(none)");

			ctx->send_msg(chan, "Current PSAs: %s", psa_buf);

		} break;
	};
}

static void psa_post(PSAData* psa, const char* user, time_t now){
	if(*psa->message == '!'){
		AliasReq req = {
			.alias = psa->message,
			.chan  = psa->channel,
			.user  = user,
		};

		MOD_MSG(ctx, "alias_exec", &req, NULL, NULL);
	} else {
		ctx->send_msg(psa->channel, "%s", psa->message);
	}

	psa->last_posted = now;
}

static intptr_t psa_twitch_cb(intptr_t result, intptr_t arg){
	*(bool*)arg = result;
	return 0;
}

static void psa_msg(const char* chan, const char* name, const char* msg){
	time_t now = time(0);

	sb_each(p, psa_data){
		if(strcmp(p->channel, chan) != 0) continue;

		if(
			now - p->last_posted > p->freq_mins * 60 && p->trigger &&
			regexec(&p->trig_rx, msg, 0, NULL, 0) == 0
		){
			bool post = true;
			if(p->when_live){
				MOD_MSG(ctx, "twitch_is_live", p->channel, &psa_twitch_cb, &post);
			}

			if(post){
				psa_post(p, name, now);
			}
			break;
		}
	}

}

static void psa_tick(time_t now){
	if(now - psa_last_update < 60) return;
	psa_last_update = now;

	sb_each(p, psa_data){
		if(now - p->last_posted > p->freq_mins * 60 && !p->trigger){
			bool post = true;
			if(p->when_live){
				MOD_MSG(ctx, "twitch_is_live", p->channel, &psa_twitch_cb, &post);
			}

			if(post){
				psa_post(p, "", now);
			}
			break;
		}
	}
}

static bool psa_save(FILE* file){
	sb_each(p, psa_data){
		fprintf(file, "%s %s\n", p->channel, p->cmdline);
	}
	return true;
}

static void psa_quit(void){
	sb_each(p, psa_data){
		free(p->channel);
		free(p->id);
		free(p->message);
		free(p->trigger);
		free(p->cmdline);
		regfree(&p->trig_rx);
	}
	sb_free(psa_data);
}
