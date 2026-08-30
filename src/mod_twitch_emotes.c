#include "module.h" // open me to view the available APIs
#include "stb_sb.h"
#include <string.h>
#include <ctype.h>
#include <curl/curl.h>
#include "inso_utils.h"
#include "inso_json.h"

static bool twitch_emotes_init    (const IRCCoreCtx*);
static void twitch_emotes_msg     (const char* chan, const char* name, const char* msg);
static void twitch_emotes_join    (const char* chan, const char* name);
static void twitch_emotes_mod_msg (const char* sender, const IRCModMsg* msg);
static void twitch_emotes_tick    (time_t now);

const IRCModuleCtx irc_mod_ctx = {
	.name       = "twitch_emotes",
	.desc       = "look up available twitch emotes for channels",
	.flags      = IRC_MOD_GLOBAL,
	.on_init    = &twitch_emotes_init,
	.on_join    = &twitch_emotes_join,
	.on_msg     = &twitch_emotes_msg,
	.on_tick    = &twitch_emotes_tick,
	.on_mod_msg = &twitch_emotes_mod_msg,
};

static const IRCCoreCtx* ctx;

enum EmoteCategory {
	EM_APPROVE,
	EM_DISAPPROVE,
	EM_LAUGH,
	EM_CRY,
	EM_INTERESTING,
	EM_CUTE,
	EM_SCARED,
	EM_BLANK,
	EM_TRIP,

	EM_COUNT,
};

struct TwitchEmoteList {
	char* chan;
	bool loaded;
	sb(char)* data;
	sb(char*) emotes[EM_COUNT];
};

static sb(struct TwitchEmoteList) emote_list;
static CURLM* curl_multi;

static bool twitch_emotes_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	curl_multi = curl_multi_init();

	return true;
}

static void begin_fetching_emotes(struct TwitchEmoteList* el) {
	char* url;
	asprintf_check(&url, "https://emotes.crippled.dev/v1/channel/%s/all", el->chan + 1);

	printf("BEGIN FETCH EMOTES %s\n", el->chan);

	CURL* curl = inso_curl_init(url, el->data);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30);
	curl_easy_setopt(curl, CURLOPT_PRIVATE, el->chan);

	curl_multi_add_handle(curl_multi, curl);

	free(url);
}

static struct EmoteCandidate {
	const char** exact;
	const char** partial;
	const char* fallback;
} candidates[] = {
	[EM_APPROVE] = {
		.partial = (const char*[]) {
			"yes", "approve", "okay", "ok", "based", "cool", "nod", NULL
		},
		.fallback = ":)"
	},
	[EM_DISAPPROVE] = {
		.partial = (const char*[]) {
			"smh", "nope", "noidontthinkso", "nuhuh", "nono", NULL
		},
		.exact = (const char*[]) {
			"no", NULL
		},
		.fallback = "PunOko"
	},
	[EM_LAUGH] = {
		.partial = (const char*[]) {
			"xd", "lul", "lol", "laugh", "hehe", "haha", "kek", NULL
		},
		.fallback = "TehePelo"
	},
	[EM_CRY] = {
		.partial = (const char*[]) {
			"sad", "cry", "blubb", "wah", NULL
		},
		.fallback = "☹️ "
	},
	[EM_INTERESTING] = {
		.partial = (const char*[]) {
			"noted", "isee", "naruhodo", "woah", "whoa", NULL
		},
		.fallback = "🤔"
	},
	[EM_CUTE] = {
		.partial = (const char*[]) {
			"cute", "ayaya", "cute", ":3", "nya", "hap", "sip", NULL
		},
		.fallback = "AYAYA"
	},
	[EM_SCARED] = {
		.partial = (const char*[]) {
			"scare", "worry", "concern", "sweat", "monka", NULL
		},
		.fallback = "😨"
	},
	[EM_BLANK] = {
		.partial = (const char*[]) {
			"reaction", "uhhuh", "blank", "sit", "huh", NULL
		},
		.fallback = "😶",
	},
	[EM_TRIP] = {
		.partial = (const char*[]) {
			"trip", "schizo", "voices", "wha", "woozy", NULL
		},
		.fallback = "🥴"
	}
};

static void maybe_add_emote(struct TwitchEmoteList* el, const char* emote) {
	for(int i = 0; i < EM_COUNT; ++i) {
		struct EmoteCandidate* em = candidates + i;

		if(em->partial) {
			for(const char** p = em->partial; *p; ++p) {
				if(strcasestr(emote, *p) != NULL) {
					printf("ADD EMOTE %s TO %d\n", emote, i);
					sb_push(el->emotes[i], strdup(emote));
					return;
				}
			}
		}

		if(em->exact) {
			for(const char** p = em->exact; *p; ++p) {
				if(strcasecmp(emote, *p) == 0) {
					printf("ADD EMOTE %s TO %d\n", emote, i);
					sb_push(el->emotes[i], strdup(emote));
					return;
				}
			}
		}
	}
}

static void end_fetching_emotes(struct TwitchEmoteList* el) {
	printf("END FETCH EMOTES %s\n", el->chan);

	struct uj_node* root = uj_parse(*el->data, sb_count(*el->data), NULL);
	if(!root || root->type != UJ_ARR)  {
		printf("error loading emotes for %s: failed to parse json response.\n", el->chan);
		goto fail;
	}

	for(size_t i = 0; i < root->len; ++i) {
		struct uj_node* provider = UJ_GET(root->arr + i, ("provider"), UJ_INT);
		if(!provider || provider->num == 0) {
			continue;
		}

		struct uj_node* emote = UJ_GET(root->arr + i, ("code"), UJ_STR);
		if(!emote) {
			continue;
		}

		maybe_add_emote(el, emote->str);
	}

	el->loaded = true;

fail:
	sb_free(*el->data);
}

static void process_channel(const char* chan) {
	sb_each(el, emote_list) {
		if(strcmp(el->chan, chan) == 0) {
			return;
		}
	}

	struct TwitchEmoteList el = {
		.chan = strdup(chan),
		.loaded = false,
		.data = calloc(1, sizeof(void*))
	};

	sb_push(emote_list, el);

	begin_fetching_emotes(&sb_last(emote_list));
}

static void twitch_emotes_msg(const char* chan, const char* name, const char* msg) {
	process_channel(chan);
}

static void twitch_emotes_join(const char* chan, const char* name) {
	process_channel(chan);
}

static void twitch_emotes_tick(time_t now) {

	int n;
	curl_multi_perform(curl_multi, &n);

	do {
		CURLMsg* msg = curl_multi_info_read(curl_multi, &n);

		if(msg && msg->msg == CURLMSG_DONE) {

			const char* chan;
			curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &chan);

			sb_each(el, emote_list) {
				if(strcmp(el->chan, chan) == 0) {
					end_fetching_emotes(el);
					break;
				}
			}

			curl_multi_remove_handle(curl_multi, msg->easy_handle);
			curl_easy_cleanup(msg->easy_handle);
		}
	} while(n > 0);
}

static const char* get_random_emote(const char* chan) {
	int category = rand() % EM_COUNT;

	sb_each(el, emote_list) {
		if(!el->loaded) continue;
		if(strcmp(chan, el->chan) != 0) continue;

		int count = sb_count(el->emotes[category]);
		if(count == 0) {
			break;
		}

		int r = rand() % count;
		return el->emotes[category][r];
	}

	return candidates[category].fallback;
}

static void twitch_emotes_mod_msg (const char* sender, const IRCModMsg* msg) {
	if(strcmp(msg->cmd, "emote_random") == 0) {
		const char* chan = (const char*)msg->arg;
		const char* emote = get_random_emote(chan);
		msg->callback((intptr_t)emote, msg->cb_arg);
	}
}
