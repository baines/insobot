#include "module.h"
#include "config.h"
#include "stb_sb.h"
#include <regex.h>
#include <string.h>
#include <ctype.h>

static bool filter_init    (const IRCCoreCtx*);
static void filter_exec    (size_t, const char*, char*, size_t);
static void filter_mod_msg (const char*, const IRCModMsg*);
static void filter_msg     (const char*, const char*, const char*);
static void filter_quit    (void);

const IRCModuleCtx irc_mod_ctx = {
	.name       = "filter",
	.desc       = "Outgoing message filter",
	.flags      = IRC_MOD_GLOBAL,
	.on_init    = &filter_init,
	.on_filter  = &filter_exec,
	.on_msg     = &filter_msg,
	.on_mod_msg = &filter_mod_msg,
	.on_quit    = &filter_quit,
};

static const IRCCoreCtx* ctx;
static regex_t* regexen;
static size_t* permits;

static bool caps_convert;

static bool filter_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	if(ctx->api_version < 2){
		fprintf(stderr, "mod_filter: insobot version too old (%d, need >= 2), exiting.\n", (int)ctx->api_version);
		return false;
	}

	char line[1024];
	FILE* f = fopen(ctx->get_datafile(), "r");

	while(fgets(line, sizeof(line), f)){
		regex_t rx;
		char* p;

		if((p = strrchr(line, '\n'))){
			*p = 0;
		}

		int err;
		if((err = regcomp(&rx, line, REG_ICASE | REG_EXTENDED)) == 0){
			sb_push(regexen, rx);
		} else {
			char errbuf[256];
			regerror(err, &rx, errbuf, sizeof(errbuf));
			fprintf(stderr, "mod_filter: bad regex [%s]: %s\n", line, errbuf);
		}
	}

	fclose(f);

	return true;
}

static void filter_exec(size_t msg_id, const char* chan, char* msg, size_t len){
	sb_each(p, permits){
		if(*p == msg_id){
			sb_erase(permits, p - permits);
			caps_convert = false;
			return;
		}
	}

	regmatch_t match;

	sb_each(r, regexen){
		char* p = msg;
		while(regexec(r, p, 1, &match, 0) == 0){
			for(int i = match.rm_so; i < match.rm_eo; ++i){
				p[i] = '*';
			}
			p += match.rm_eo;
		}
	}

	if(caps_convert){
		for(char* c = msg; *c; ++c){
			*c = toupper(*c);
		}
		caps_convert = false;
	}

	if(getenv("IRC_IS_TWITCH")){
		ctx->strip_colors(msg);
	}
}

static void filter_msg(const char* chan, const char* nick, const char* msg){
	bool all_caps = false;

	if(*msg == *CONTROL_CHAR || *msg == *CONTROL_CHAR_2){
		all_caps = true;

		for(const char* m = msg; *m && *m != ' '; ++m){
			if(islower(*m)){
				all_caps = false;
				break;
			}
		}
	}

	if(all_caps){
		caps_convert = true;
	}
}

static void filter_mod_msg(const char* sender, const IRCModMsg* msg){

	if(strcmp(msg->cmd, "filter_permit") == 0){
		bool exists = false;
		size_t id = (size_t)msg->arg;

		sb_each(p, permits){
			if(*p == id){
				exists = true;
				break;
			}
		}

		if(!exists){
			sb_push(permits, id);
		}
	}
}

static void filter_quit(void){
	sb_each(r, regexen){
		regfree(r);
	}
	sb_free(regexen);
	sb_free(permits);
}
