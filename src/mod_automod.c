#include "module.h"

#include <regex.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <time.h>
#include "stb_sb.h"
#include "inso_utils.h"

//#define TRIGGER_HAPPY

static void automod_msg     (const char*, const char*, const char*);
static void automod_cmd     (const char*, const char*, const char*, int);
static bool automod_init    (const IRCCoreCtx*);
static void automod_join    (const char*, const char*);
static void automod_connect (const char*);
static void automod_quit    (void);

enum { AUTOMOD_TIMEOUT, AUTOMOD_UNBAN };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "automod",
	.desc       = "Moderates the chat.",
	.on_msg     = &automod_msg,
	.on_cmd     = &automod_cmd,
	.on_action  = &automod_msg,
	.on_init    = &automod_init,
	.on_connect = &automod_connect,
	.on_join    = &automod_join,
	.on_quit    = &automod_quit,
	.commands   = DEFINE_CMDS(
		[AUTOMOD_TIMEOUT] = "!b \\b !to !ko \\ko",
		[AUTOMOD_UNBAN]   = "!ub \\ub"
	),
	.cmd_help = DEFINE_CMDS(
		[AUTOMOD_TIMEOUT] = "<user> [time] | Times out <user> for [time] minutes (default 10)",
		[AUTOMOD_UNBAN]   = "<user> | Removes a ban/timeout on <user>"
	)
};

static const IRCCoreCtx* ctx;

typedef struct {
	char*  name;
	int    score;
	time_t join;
	time_t last_msg;
	int    num_offences;
} Suspect;

static char**    channels;
static Suspect** suspects;

static time_t init_time;
static bool is_twitch;
static regex_t url_regex;

static bool automod_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	init_time = time(0);
	is_twitch = true;
	return regcomp(
		&url_regex,
        "\\b(https?://[^[:space:]]+|[a-zA-Z0-9][a-zA-Z0-9\\-_]*\\.[A-Za-z]{2,5}(\\.[A-Za-z]{2,5})*([:space:]|$|/|#|:|\\?))",
		REG_ICASE | REG_EXTENDED | REG_NEWLINE
	) == 0;
}

static void automod_quit(void){
	regfree(&url_regex);

	for(char** c = channels; c < sb_end(channels); ++c){
		free(*c);
	}
	sb_free(channels);

	for(Suspect** slist = suspects; slist < sb_end(suspects); ++slist){
		for(Suspect* s = *slist; s < sb_end(*slist); ++s){
			free(s->name);
		}
		sb_free(*slist);
	}
	sb_free(suspects);
}

static void automod_connect(const char* serv){
	is_twitch = strcasestr(serv, "twitch.tv") || getenv("IRC_IS_TWITCH");
}

static Suspect* get_suspect(const char* chan, const char* name){

	int index = -1;
	for(size_t i = 0; i < sb_count(channels); ++i){
		if(strcmp(chan, channels[i]) == 0){
			index = i;
			break;
		}
	}

	if(index == -1){
		sb_push(channels, strdup(chan));
		sb_push(suspects, NULL);
		index = sb_count(channels) - 1;
	}

	for(size_t i = 0; i < sb_count(suspects[index]); ++i){
		if(strcmp(suspects[index][i].name, name) == 0){
			return suspects[index] + i;
		}
	}

	Suspect s = {
		.name = strdup(name)
	};

	sb_push(suspects[index], s);
	return &sb_last(suspects[index]);
}

static void automod_join(const char* chan, const char* name){

	if(strcmp(name, ctx->get_username()) == 0){
		for(size_t i = 0; i < sb_count(channels); ++i){
			if(strcmp(channels[i], chan)) return;
		}

		sb_push(channels, strdup(chan));
		sb_push(suspects, NULL);
	} else {
		Suspect* s = get_suspect(chan, name);
		if(!s->join) s->join = time(0);
	}
}

#ifdef TRIGGER_HAPPY
static int am_score_caps(const Suspect* s, const char* msg, size_t len){
	size_t num_caps = 0;

	if(len < 10) return 0;

	for(const char* p = msg; *p; ++p){
		if(isupper(*p)) num_caps++;
	}

	return (num_caps / (float)len) > 0.80 ? 45 : 0;
}
#endif

#ifndef __STDC_ISO_10646__
	#error "Your OS/compiler doesn't store a Unicode / UCS4 codepoint in a wchar_t :("
#endif

static int am_score_ascii_art(const Suspect* s, const char* msg, size_t len){
	const char *ptr = msg, *end = msg + len;
	mbstate_t state = {};
	wchar_t codepoint, prev_codepoint = 0;

	int bad_char_score = 0, punct = 0, same = 0, max_same = 0;

	int ret;
	while((ret = mbrtowc(&codepoint, ptr, end - ptr, &state)) > 0){

		if(codepoint == prev_codepoint){
			same++;
			max_same = INSO_MAX(max_same, same);
		} else {
			same = 0;
		}
		prev_codepoint = codepoint;

		// box drawing glyphs
		if(codepoint >= 0x2500 && codepoint < 0x2600){
			bad_char_score += 1;
		}

		// hexagrams
		if(codepoint >= 0x4DC0 && codepoint < 0x4E00){
			bad_char_score += 1;
		}

		// reserved (utf-16 implementation)
		if(codepoint >= 0xD800 && codepoint < 0xE000){
			bad_char_score += 1;
		}

		// private use
		if(codepoint >= 0xE000 && codepoint < 0xF900){
			bad_char_score += 1;
		}

		// misc symbols / pictographs
		if(codepoint >= 0x1F300 && codepoint < 0x1F600){
			bad_char_score += 1;
		}

		// supplemental symbols / pictographs
		if(codepoint >= 0x1F900 && codepoint < 0x1FA00){
			bad_char_score += 1;
		}

		// emoticons
		if(codepoint >= 0x1F600 && codepoint < 0x1F650){
			bad_char_score += 1;
		}

		// transport / map symbols
		if(codepoint >= 0x1F680 && codepoint < 0x1F700){
			bad_char_score += 1;
		}

		// misc symbols
		if(codepoint >= 0x2600 && codepoint < 0x2700){
			bad_char_score += 1;
		}

		// dingbats
		if(codepoint >= 0x2700 && codepoint < 0x27C0){
			bad_char_score += 1;
		}

		if(ispunct(codepoint)){
			punct++;
		}

		ptr += ret;
	}

	bad_char_score *= 13;

	// invalid sequences?
	if(ret == -1){
		bad_char_score += 60;
	}

#ifdef TRIGGER_HAPPY
	// same char spam
	if(max_same >= 8){
		bad_char_score += (60 + (max_same - 8) * 5);
	}
#endif

	return bad_char_score;
}

static intptr_t get_karma_cb(intptr_t result, intptr_t arg){
	if(result) *(int*)arg = result;
	return 0;
}

static intptr_t get_user_cb(time_t result, time_t* arg){
	if(result) *arg = result;
	return 0;
}

static int am_score_links(const Suspect* s, const char* msg, size_t len){
	bool is_url = false;
	regmatch_t match;

	if(regexec(&url_regex, msg, 1, &match, 0) == 0){
		is_url = true;
	}

	time_t now = time(0);

	// give twitch some time to give us the joins
	if(is_twitch && (now - init_time) < 30) return 0;

	// look for urls as first message
	if(is_url && !s->last_msg){

		// has been ++'d before?
		int karma = 0;
		MOD_MSG(ctx, "karma_get", s->name, &get_karma_cb, &karma);
		if(karma > 0){
			return 0;
		}

		// new account?
		if(is_twitch){
			time_t user_created_date = now;
			MOD_MSG(ctx, "twitch_get_user_date", s->name, &get_user_cb, &user_created_date);

			printf("twitch user time: %zu\n", (size_t)(now - user_created_date));

			if((now - user_created_date) < (24*60*60)){
				return 500;
			}
		}
	}

	return 0;
}

#ifdef TRIGGER_HAPPY
static int am_score_flood(const Suspect* s, const char* msg, size_t len){
	time_t now = time(0);

	if((now - s->last_msg) < 5){
		return 25;
	} else {
		return 0;
	}
}
#endif

static int am_score_emotes(const Suspect* s, const char* msg, size_t len){
	int emote_count = 0;
	int i = 0;
	const char *k, *v;

	while(ctx->get_tag(i++, &k, &v)){
		if(strcmp(k, "emotes") != 0) continue;
		for(; *v; ++v){
			if(*v == ':' || *v == ',') ++emote_count;
		}
		break;
	}

	return emote_count >= 5 ? 100 : emote_count * 10;
}

static void automod_discipline(Suspect* s, const char* chan, const char* reason){

	s->num_offences += INSO_MIN(1, (s->score / 100));
	s->score = INSO_MIN(s->score / 2, 50);

#if 0
	ctx->send_msg(chan, "[automod-test] Would've timed out %s.\n", s->name);
#else
	if(is_twitch){
		int timeout = s->num_offences <= 1
			? 10
			: (s->num_offences - 1) * (s->num_offences - 1) * 60
			;

		ctx->send_msg(chan, ".timeout %s %d %s", s->name, timeout, reason);
		ctx->send_msg(chan, "Timed out %s (%s)", inso_dispname(ctx, s->name), reason);
	} else {
		char buf[512];
		snprintf(buf, sizeof(buf), "KICK %s %s :%s", chan, s->name, reason);
		ctx->send_raw(buf);

		if(s->num_offences >= 2){
			//TODO: proper mask
			snprintf(buf, sizeof(buf), "MODE %s +b %s!*@*", chan, s->name);
			ctx->send_raw(buf);
		}
	}
#endif

}

static void automod_msg(const char* chan, const char* name, const char* msg){
	if(inso_is_wlist(ctx, name)) return;
	Suspect* susp = get_suspect(chan, name);
	bool discipline = false;
	int score = 0;

#ifdef TRIGGER_HAPPY
	const char* rules[] = { "caps", "symbol spam", "flood", "emotes", "spambot?" };

	int (*score_fns[])(const Suspect*, const char*, size_t) = {
		am_score_caps,
		am_score_ascii_art,
		am_score_flood,
		am_score_emotes,
		am_score_links
	};
#else
	const char* rules[] = { "symbol spam", "emotes", "spambot?" };

	int (*score_fns[])(const Suspect*, const char*, size_t) = {
		am_score_ascii_art,
		am_score_emotes,
		am_score_links
	};
#endif

	size_t len = strlen(msg);

	printf("AM: <%s> ", name);

	size_t i;
	for(i = 0; i < ARRAY_SIZE(score_fns); ++i){
		score += score_fns[i](susp, msg, len);
		printf("[%s: %d] ", rules[i], score);

		if(score && susp->score + score >= 100){
			discipline = true;
			break;
		}
	}

	if(score == 0){
		score = -25;
	}

	susp->score = INSO_MAX(0, susp->score + score);
	susp->last_msg = time(0);

	printf("[%d]\n", susp->score);

	if(i >= ARRAY_SIZE(rules)) i = ARRAY_SIZE(rules) - 1;

	if(discipline){
		automod_discipline(susp, chan, rules[i]);
	}
}

static void automod_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(!inso_is_admin(ctx, name)) return;

	//TODO: make commands work with standard IRC kick/ban protocol, not just twitch

	if(cmd == AUTOMOD_TIMEOUT){

		// no manual moderation for this channel
		if(strcmp(chan, "#handmade_hero") == 0) return;

		char victim[32] = {};
		int duration = 10;
		if(sscanf(arg, "%31s %d", victim, &duration) >= 1){
			if(is_twitch){
				ctx->send_msg(chan, ".timeout %s %d", victim, duration);
			} else {
				char buf[256];
				snprintf(buf, sizeof(buf), "KICK %s %s", chan, victim);
				ctx->send_raw(buf);
			}
		}
	} else if(cmd == AUTOMOD_UNBAN){
		if(is_twitch){
			ctx->send_msg(chan, ".unban %s", arg);
		} else {
			char buf[256];
			char* who = strndupa(arg, strchrnul(arg, ' ') - arg);
			snprintf(buf, sizeof(buf), "MODE %s -b %s!*@*", chan, who);
			ctx->send_raw(buf);
		}
	}
}
