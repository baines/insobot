#include "module.h"
#include <regex.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include "stb_sb.h"
#include "utils.h"

static void automod_msg     (const char*, const char*, const char*);
static void automod_cmd     (const char*, const char*, const char*, int);
static bool automod_init    (const IRCCoreCtx*);
static void automod_join    (const char*, const char*);
static void automod_connect (const char*);
//static void automod_quit    (void);

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
//	.on_quit    = &automod_quit
	.commands   = DEFINE_CMDS(
		[AUTOMOD_TIMEOUT] = "!b \\b !to !ko \\ko",
		[AUTOMOD_UNBAN]   = "!ub \\ub"
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
	regcomp(&url_regex, "www\\.|https?:\\/\\/|\\.com|\\.[a-zA-Z]\\/", REG_ICASE | REG_EXTENDED);
	return true;
}

static void automod_connect(const char* serv){
	is_twitch = strcasestr(serv, "twitch.tv") || getenv("IRC_IS_TWITCH");
}

static Suspect* get_suspect(const char* chan, const char* name){

	int index = -1;
	for(int i = 0; i < sb_count(channels); ++i){
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

	for(int i = 0; i < sb_count(suspects[index]); ++i){
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
		for(int i = 0; i < sb_count(channels); ++i){
			if(strcmp(channels[i], chan)) return;
		}

		sb_push(channels, strdup(chan));
		sb_push(suspects, NULL);
	} else {
		Suspect* s = get_suspect(chan, name);
		if(!s->join) s->join = time(0);
	}
}

static int am_score_caps(const Suspect* s, const char* msg, size_t len){
	size_t num_caps = 0;

	for(const char* p = msg; *p; ++p){
		if(isupper(*p)) num_caps++;
	}

	return (num_caps / (float)len) > 0.80 ? 45 : 0;
}

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
			bad_char_score += 10;
		}

		// hexagrams
		if(codepoint >= 0x4DC0 && codepoint < 0x4E00){
			bad_char_score += 5;
		}
		
		// private use
		if(codepoint >= 0xE000 && codepoint < 0xF900){
			bad_char_score += 2;
		}

		if(ispunct(codepoint)){
			punct++;
		}

		ptr += ret;
	}

	// invalid sequences?
	if(ret < 0){
		bad_char_score += 60;
	}

	// symbol spam
	if(len >= 10 && (punct / (float)len) > 0.4f){
		bad_char_score += 60;
	}

	// same char spam
	if(max_same >= 8){
		bad_char_score += (60 + (max_same - 8) * 5);
	}

	return bad_char_score;
}

static void get_karma_cb(intptr_t result, intptr_t arg){
	if(result) *(int*)arg = result;
}

static void get_user_cb(intptr_t result, intptr_t arg){
	if(result) *(time_t*)arg = result;
}

static int am_score_links(const Suspect* s, const char* msg, size_t len){
	int url_len = 0;
	regmatch_t match;

	if(regexec(&url_regex, msg, 1, &match, 0) == 0){
		url_len = match.rm_eo - match.rm_so;
	}

	time_t now = time(0);

	if((now - init_time) < 120) return 0;

	// look for short urls as first message
	if(url_len > 0 && url_len < 25 && !s->last_msg && (!s->join || (now - s->join) < 60)){

		// new account?
		if(is_twitch){
			time_t user_created_date = 0;
			MOD_MSG(ctx, "twitch_get_user_date", s->name, &get_user_cb, &user_created_date);

			if((now - user_created_date) < (7*24*60*60)){
				return 100;
			}
		}

		// has been ++'d before?
		int karma = 0;
		MOD_MSG(ctx, "karma_get", s->name, &get_karma_cb, &karma);
		return karma > 0 ? 0 : 100;

	} else {
		return 0;
	}
}

static int am_score_flood(const Suspect* s, const char* msg, size_t len){
	time_t now = time(0);

	if((now - s->last_msg) < 5){
		return 35;
	} else {
		return 0;
	}
}

// only global twitch emotes for now
static const char* emotes[] = {
  "4Head",
  "AMPEnergy",
  "AMPEnergyCherry",
  "ANELE",
  "ArgieB8",
  "ArsonNoSexy",
  "AsianGlow",
  "AthenaPMS",
  "BabyRage",
  "BatChest",
  "BCouch",
  "BCWarrior",
  "BibleThump",
  "BiersDerp",
  "BigBrother",
  "BionicBunion",
  "BlargNaut",
  "bleedPurple",
  "BloodTrail",
  "BORT",
  "BrainSlug",
  "BrokeBack",
  "BudBlast",
  "BuddhaBar",
  "BudStar",
  "ChefFrank",
  "cmonBruh",
  "CoolCat",
  "CorgiDerp",
  "CougarHunt",
  "DAESuppy",
  "DalLOVE",
  "DansGame",
  "DatSheffy",
  "DBstyle",
  "deExcite",
  "deIlluminati",
  "DendiFace",
  "DogFace",
  "DOOMGuy",
  "duDudu",
  "EagleEye",
  "EleGiggle",
  "FailFish",
  "FPSMarksman",
  "FrankerZ",
  "FreakinStinkin",
  "FUNgineer",
  "FunRun",
  "FutureMan",
  "FuzzyOtterOO",
  "GingerPower",
  "GrammarKing",
  "HassaanChop",
  "HassanChop",
  "HeyGuys",
  "HotPokket",
  "HumbleLife",
  "ItsBoshyTime",
  "Jebaited",
  "JKanStyle",
  "JonCarnage",
  "KAPOW",
  "Kappa",
  "KappaClaus",
  "KappaPride",
  "KappaRoss",
  "KappaWealth",
  "Keepo",
  "KevinTurtle",
  "Kippa",
  "Kreygasm",
  "Mau5",
  "mcaT",
  "MikeHogu",
  "MingLee",
  "MrDestructoid",
  "MVGame",
  "NinjaTroll",
  "NomNom",
  "NoNoSpot",
  "NotATK",
  "NotLikeThis",
  "OhMyDog",
  "OMGScoots",
  "OneHand",
  "OpieOP",
  "OptimizePrime",
  "OSfrog",
  "OSkomodo",
  "OSsloth",
  "panicBasket",
  "PanicVis",
  "PartyTime",
  "PazPazowitz",
  "PeoplesChamp",
  "PermaSmug",
  "PeteZaroll",
  "PeteZarollTie",
  "PicoMause",
  "PipeHype",
  "PJSalt",
  "PMSTwin",
  "PogChamp",
  "Poooound",
  "PraiseIt",
  "PRChase",
  "PunchTrees",
  "PuppeyFace",
  "RaccAttack",
  "RalpherZ",
  "RedCoat",
  "ResidentSleeper",
  "riPepperonis",
  "RitzMitz",
  "RuleFive",
  "SeemsGood",
  "ShadyLulu",
  "ShazBotstix",
  "ShibeZ",
  "SmoocherZ",
  "SMOrc",
  "SMSkull",
  "SoBayed",
  "SoonerLater",
  "SriHead",
  "SSSsss",
  "StinkyCheese",
  "StoneLightning",
  "StrawBeary",
  "SuperVinlin",
  "SwiftRage",
  "TBCheesePull",
  "TBTacoLeft",
  "TBTacoRight",
  "TF2John",
  "TheRinger",
  "TheTarFu",
  "TheThing",
  "ThunBeast",
  "TinyFace",
  "TooSpicy",
  "TriHard",
  "TTours",
  "twitchRaid",
  "TwitchRPG",
  "UleetBackup",
  "UncleNox",
  "UnSane",
  "VaultBoy",
  "VoHiYo",
  "Volcania",
  "WholeWheat",
  "WinWaker",
  "WTRuck",
  "WutFace",
  "YouWHY",
  NULL
};

static int am_score_emotes(const Suspect* s, const char* msg, size_t len){
	int emote_count = 0;

	for(const char** e = emotes; *e; ++e){
		const char* p = msg;
		while((p = strstr(p, *e))){
			++emote_count;
			p += strlen(*e);
		}

		if(emote_count >= 5){
			break;
		}
	}

	return emote_count >= 5 ? 100 : emote_count * 5;
}

static void automod_discipline(Suspect* s, const char* chan, const char* reason){

	s->num_offences += (s->score / 100);
	s->score = INSO_MIN(s->score / 2, 50);

#if 0
	ctx->send_msg(chan, "[automod-test] Would've timed out %s.\n", s->name);
#else
	if(is_twitch){
		int timeout = 10 + (s->num_offences * s->num_offences * 60);
		ctx->send_msg(chan, ".timeout %s %d %s", s->name, timeout, reason);
		ctx->send_msg(chan, "Timed out %s (%s)", s->name, reason);
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

	int (*score_fns[])(const Suspect*, const char*, size_t) = {
		am_score_caps,
		am_score_ascii_art,
		am_score_flood,
		am_score_emotes,
		am_score_links
	};

	const char* rules[] = { "caps", "ascii", "flood", "emotes", "links" };

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
			ctx->send_msg(chan, ".timeout %s %d", victim, duration);
		}
	} else if(cmd == AUTOMOD_UNBAN){
		ctx->send_msg(chan, ".unban %s", arg);
	}
}
