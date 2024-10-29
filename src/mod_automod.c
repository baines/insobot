#include "module.h"

#include <regex.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <time.h>
#include <argz.h>
#include "stb_sb.h"
#include "inso_utils.h"

#include <curl/curl.h>
#include <iconv.h>

//#define TRIGGER_HAPPY

static void automod_msg     (const char*, const char*, const char*);
static void automod_cmd     (const char*, const char*, const char*, int);
static bool automod_init    (const IRCCoreCtx*);
static void automod_join    (const char*, const char*);
static void automod_connect (const char*);
static void automod_modified(void);
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
	.on_modified = &automod_modified,
	.on_join    = &automod_join,
	.on_quit    = &automod_quit,
	.commands   = DEFINE_CMDS(
		[AUTOMOD_TIMEOUT] = CMD("b") CMD("ko"),
		[AUTOMOD_UNBAN]   = CMD("ub")
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

static char* bad_words_argz;
static size_t bad_words_len;

static iconv_t ic;

static void load_bad_words(void) {
	free(bad_words_argz);
	bad_words_argz = NULL;
	bad_words_len = 0;

	FILE* f = fopen(ctx->get_datafile(), "rb");

	char line[256] = {};
	while((fgets(line, 255, f))) {
		size_t sz = strlen(line);
		if(line[sz-1] == '\n') {
			line[sz-1] = '\0';
		}
		argz_add(&bad_words_argz, &bad_words_len, line);
	}

	fclose(f);
}

static bool automod_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	init_time = time(0);
	is_twitch = true;

	load_bad_words();

	ic = iconv_open("ASCII//TRANSLIT//IGNORE", "UTF-8");

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


static void automod_modified(void) {
	load_bad_words();
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

#if 0
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
#endif

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
#endif

static int am_score_viewbot(const Suspect* s, const char* msg, size_t len) {
    static char output[4096];
    static char output2[4096];

    memset(output, 0, sizeof(output));

    char* in = (char*)msg;
    char* out = output;

    size_t inlen = strlen(msg);
    size_t outlen = sizeof(output)-1;

    size_t res;
    do {
        res = iconv(ic, &in, &inlen, &out, &outlen);
        if(res == (size_t)-1) {
            perror("iconv");
            break;
        }

    } while(inlen);

	char* w = output2;
	char* r = output;

	while(r < out) {
		char c = *r++;
		if(c == '?') {
			continue;
		}
		*w++ = c;
	}

	*w = '\0';

	if(strcasestr(output2, "best viewers") || strcasestr(output2, "cheap viewers")) {
		return 9000;
	}

	return 0;
}

static int am_score_words(const Suspect* s, const char* msg, size_t len) {

	char* entry = NULL;
	while((entry = argz_next(bad_words_argz, bad_words_len, entry))) {
		printf("try [%s] [%s]\n", entry, msg);

		const char* p = strcasestr(msg, entry);
		if(!p) {
			continue;
		}

		size_t esize = strlen(entry);

		printf("bad word [%s] = [%c] [%d]\n", entry, p[esize], p == msg);

		if((p == msg || !isalpha(p[-1])) && !isalpha(p[esize])) {
			return 1000;
		}
	}

	return 0;
}

static void twitch_timeout(const char* chan, const char* who, int duration, const char* reason);

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

		twitch_timeout(chan, NULL, timeout, reason);
		//ctx->send_msg(chan, ".timeout %s %d %s", s->name, timeout, reason);
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
		am_score_links,
		am_score_viewbot,
	};
#else
	const char* rules[] = { "spambot?", "bad word", "spambot" };

	int (*score_fns[])(const Suspect*, const char*, size_t) = {
		//am_score_ascii_art,
		am_score_links,
		am_score_words,
		am_score_viewbot,
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

static const char* get_tag(const char* key) {
	size_t i = 0;
	const char *k, *v;
	while(ctx->get_tag(i++, &k, &v)) {
		if(strcmp(k, key) == 0) {
			return v;
		}
	}

	return NULL;
}

static void twitch_timeout(const char* chan, const char* who, int duration, const char* reason) {
	struct curl_slist* list = NULL;

	const char* client_id = getenv("INSOBOT_TWITCH_CLIENT_ID");
	if(client_id){
		char buf[256];
		snprintf(buf, sizeof(buf), "Client-ID: %s", client_id);
		list = curl_slist_append(list, buf);
	}

	const char* oauth = getenv("INSOBOT_TWITCH_TOKEN");
	if(oauth) {
		char buf[256];
		snprintf(buf, sizeof(buf), "Authorization: Bearer %s", oauth);
		list = curl_slist_append(list, buf);
	}

	char* chan_id;
	char* mod_id;

	MOD_MSG(ctx, "twitch_get_user_id", chan+1, &get_user_cb, &chan_id);
	MOD_MSG(ctx, "twitch_get_user_id", "insobot", &get_user_cb, &mod_id);

	if(!chan_id || !mod_id) {
		return;
	}

	char url[512];
	snprintf(url, sizeof(url), "https://api.twitch.tv/helix/moderation/bans?broadcaster_id=%s&moderator_id=%s", chan_id, mod_id);

	CURL* curl = inso_curl_init(url, NULL);

	const char* user_id = NULL;

	if(who == NULL) {
		user_id = get_tag("user-id");
	} else {
		MOD_MSG(ctx, "twitch_get_user_id", who, &get_user_cb, &user_id);
	}

	if(!user_id) {
		return;
	}

	char json[512];
	snprintf(json, sizeof(json), "{ \"data\": { \"user_id\": \"%s\", \"duration\": %d } }", user_id, duration);

	list = curl_slist_append(list, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(json));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	curl_easy_perform(curl);

	curl_slist_free_all(list);
	curl_easy_cleanup(curl);
}

static void automod_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(!inso_is_admin(ctx, name)) return;

	if(cmd == AUTOMOD_TIMEOUT){

		// no manual moderation for this channel
		if(strcmp(chan, "#molly_rocket") == 0) return;

		char victim[32] = {};
		int duration = 10;
		if(sscanf(arg, "%31s %d", victim, &duration) >= 1){
			if(is_twitch){
				twitch_timeout(chan, victim, duration, "");
				//ctx->send_msg(chan, ".timeout %s %d", victim, duration);
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
