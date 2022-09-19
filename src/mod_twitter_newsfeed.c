#include "module.h"
#include <curl/curl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <argz.h>
#include "stb_sb.h"
#include "inso_utils.h"
#include "inso_json.h"

static bool twitter_newsfeed_init (const IRCCoreCtx*);
static void twitter_newsfeed_quit (void);
static void twitter_newsfeed_tick (time_t);
static void twitter_newsfeed_cmd  (const char* chan, const char* name, const char* arg, int cmd);
static bool twitter_newsfeed_save (FILE* file);

enum { TWITTER_NEWSFEED_CMD };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "twitter_newsfeed",
	.desc     = "get news from twitter",
	.on_init  = &twitter_newsfeed_init,
	.on_quit  = &twitter_newsfeed_quit,
	.on_tick  = &twitter_newsfeed_tick,
	.on_cmd   = &twitter_newsfeed_cmd,
	.on_save  = &twitter_newsfeed_save,
	.commands = DEFINE_CMDS (
		[TWITTER_NEWSFEED_CMD] = "!tnf"
	),
};

static const IRCCoreCtx* ctx;
static CURLM* curl_multi;
static sb(char) msgbuf;
struct uj_parser uj;
static char* auth_header;

static char* channels;
static size_t channels_len;

struct curl_req {
	CURL* handle;
	struct curl_slist* headers;
};

static struct curl_req* curl_req;

static void str_replace(sb(char)* msg, const char* from, const char* to){
	size_t from_len = strlen(from);
	size_t to_len = strlen(to);
	size_t msg_len = sb_count(*msg);

	char* p;
	size_t off = 0;

	while((p = strstr(*msg + off, from))){

		off = p - *msg;

		if(to_len > from_len){
			memset(sb_add(*msg, to_len - from_len), 0, to_len - from_len);
		} else {
			stb__sbn(*msg) -= (from_len - to_len);
		}

		p = *msg + off;

		const char* end_p = *msg + msg_len;
		memmove(p + to_len, p + from_len, end_p - (p + from_len));
		memcpy(p, to, to_len);

		off += to_len;

		msg_len += (to_len - from_len);
	}
}

struct rep {
	size_t start, end;
	const char* replacement;
	const char* original;
};

static int sort_replacements(struct rep* a, struct rep* b) {
	return b->end - a->end;
}

struct tweet {
	const char* username;
	const char* display_name;
	sb(char) text;
};

static bool expand_one_tweet(struct tweet* output, struct uj_node* root, struct uj_node* urls, struct uj_node* media, struct uj_node* users) {
	if(!root) {
		return false;
	}

	struct uj_node* rawtext = UJ_GET(root, ("text"), UJ_STR);
	if(!rawtext) {
		return false;
	}

	size_t rawtextlen = strlen(rawtext->str);
	memcpy(sb_add(output->text, rawtextlen+1), rawtext->str, rawtextlen+1);
	sb(struct rep) replacements = NULL;

	int urlcount = 0;
	if(urls) {
		urlcount = urls->len;
	}

	for(int i = 0; i < urlcount; ++i) {
		struct uj_node* entry = urls->arr + i;
		struct uj_node* start = uj_get(entry, "start", UJ_INT);
		struct uj_node* end = uj_get(entry, "end", UJ_INT);
		struct uj_node* orig = uj_get(entry, "url", UJ_STR);

		if(!start || !end || !orig) {
			continue;
		}

		struct uj_node* unwound = uj_get(entry, "unwound_url", UJ_STR);
		if(unwound) {
			struct rep r = {
				start->num, end->num, unwound->str, orig->str
			};
			sb_push(replacements, r);
			continue;
		}

		struct uj_node* media_key = uj_get(entry, "media_key", UJ_STR);
		if(!media_key || !media) {
			continue;
		}

		for(size_t j = 0; j < media->len; ++j) {
			struct uj_node* m = media->arr + j;
			struct uj_node* key = uj_get(m, "media_key", UJ_STR);
			if(!key || strcmp(key->str, media_key->str) != 0) {
				continue;
			}

			struct uj_node* url = uj_get(m, "url", UJ_STR);

			if(url) {
				struct rep r = {
					start->num, end->num, url->str, orig->str
				};
				sb_push(replacements, r);
				break;
			}

			struct uj_node* variants = uj_get(m, "variants", UJ_ARR);
			if(!variants) {
				continue;
			}

			int best_bitrate = -1;
			const char* best_url = NULL;

			for(size_t k = 0; k < variants->len; ++k) {
				struct uj_node* v = variants->arr + k;
				struct uj_node* bitrate = uj_get(v, "bit_rate", UJ_INT);
				struct uj_node* vurl = uj_get(v, "url", UJ_STR);

				if(!bitrate || !vurl) {
					continue;
				}

				if(bitrate->num > best_bitrate) {
					best_bitrate = bitrate->num;
					best_url = vurl->str;
				}
			}

			if(best_url) {
				struct rep r = {
					start->num, end->num, best_url, orig->str
				};
				sb_push(replacements, r);
				break;
			}
		}
	}

	size_t rep_count = sb_count(replacements);
	qsort(replacements, rep_count, sizeof(struct rep), (int(*)())&sort_replacements);

	for(size_t i = 0; i < rep_count; ++i) {
		struct rep* rep = replacements + i;

		sb(char) reptext = NULL;
		size_t replen = strlen(rep->replacement);
		memcpy(sb_add(reptext, replen), rep->replacement, replen);

		while(i < rep_count && replacements[i+1].start == rep->start && replacements[i+1].end == rep->end) {
			++i;
			struct rep* rep2 = replacements + i;
			sb_push(reptext, ' ');
			size_t len = strlen(rep2->replacement);
			memcpy(sb_add(reptext, len), rep2->replacement, len);
		}

		sb_push(reptext, 0);

		str_replace(&output->text, rep->original, reptext);
		sb_free(reptext);
	}

	sb_free(replacements);

	struct uj_node* author = UJ_GET(root, ("author_id"), UJ_STR);

	if(author && users) {
		for(size_t i = 0; i < users->len; ++i) {
			struct uj_node* entry = users->arr + i;
			struct uj_node* id = uj_get(entry, "id", UJ_STR);
			if(!id) {
				continue;
			}

			if(strcmp(id->str, author->str) == 0) {
				struct uj_node* display_name = uj_get(entry, "name", UJ_STR);
				struct uj_node* username = uj_get(entry, "username", UJ_STR);

				if(username) {
					output->username = username->str;
				}

				if(display_name) {
					output->display_name = display_name->str;
				}
			}
		}
	}

	for(size_t i = 0; i < sb_count(output->text); ++i) {
		char* p = output->text + i;
		if(*p == '\r' || *p == '\n') {
			*p = ' ';
		}
	}

	return true;
}

static struct uj_node* get_tweet_obj(struct uj_node* tweets, const char* id) {
	if(!tweets || tweets->type != UJ_ARR) {
		return NULL;
	}

	for(size_t i = 0; i < tweets->len; ++i) {
		struct uj_node* entry = tweets->arr + i;
		struct uj_node* tid = uj_get(entry, "id", UJ_STR);

		if(!tid || strcmp(tid->str, id) != 0) {
			continue;
		}

		return entry;
	}

	return NULL;
}

enum tweet_type {
	TW_NONE,
	TW_MAIN,
	TW_RETWEET,
	TW_QUOTED,
	TW_REPLY,
};

static void send_msg(const char* fmt, ...) {
	char buf[2048];
	va_list va;

	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);

	char* chan = NULL;
	while((chan = argz_next(channels, channels_len, chan))) {
		ctx->send_msg(chan, "%s", buf);
	}

	va_end(va);
}

static void print_msgs(void) {
	enum uj_status status = uj_parse_chunk(&uj, msgbuf, sb_count(msgbuf));
	struct uj_node* root = NULL;

	if(status == UJ_OK && (root = uj_parse_chunk_end(&uj))) {
		struct uj_node* urls = UJ_GET(root, ("data", "entities", "urls"), UJ_ARR);
		struct uj_node* media = UJ_GET(root, ("includes", "media"), UJ_ARR);
		struct uj_node* users = UJ_GET(root, ("includes", "users"), UJ_ARR);
		struct uj_node* data = uj_get(root, "data", UJ_OBJ);

		struct tweet tweet = {};
		if(!expand_one_tweet(&tweet, data, urls, media, users)) {
			goto out;
		}

		enum tweet_type other_tweet_type = TW_NONE;
		struct tweet other_tweet = {};

		struct uj_node* tweets = UJ_GET(root, ("includes", "tweets"), UJ_ARR);
		struct uj_node* ref_tweets = UJ_GET(root, ("data", "referenced_tweets"), UJ_ARR);

		if(ref_tweets && tweets && ref_tweets->len > 0) {
			struct uj_node* obj = ref_tweets->arr; // XXX: first array entry only?
			struct uj_node* type = uj_get(obj, "type", UJ_STR);
			struct uj_node* id = uj_get(obj, "id", UJ_STR);

			if(type && id) {
				struct uj_node* ref_tweet_obj = get_tweet_obj(tweets, id->str);
				struct uj_node* ref_urls = UJ_GET(ref_tweet_obj, ("entities", "urls"), UJ_ARR);
				expand_one_tweet(&other_tweet, ref_tweet_obj, ref_urls, media, users);

				if(strcmp(type->str, "retweeted") == 0) {
					other_tweet_type = TW_RETWEET;
				} else if(strcmp(type->str, "quoted") == 0) {
					other_tweet_type = TW_QUOTED;
				} else if(strcmp(type->str, "replied_to") == 0) {
					other_tweet_type = TW_REPLY;
				}
			}
		}

		if(tweet.username == NULL) {
			tweet.username = "???";
		}

		if(other_tweet.username == NULL) {
			other_tweet.username = "???";
		}

		if(other_tweet_type == TW_REPLY) {
			send_msg("[\00312%s\017]: [%s]", other_tweet.username, other_tweet.text);
			send_msg(" ↳ [\00309%s\017]: [%s]", tweet.username, tweet.text);
		} else if(other_tweet_type == TW_QUOTED) {
			send_msg("[\00308%s\017]: [%s]", other_tweet.username, other_tweet.text);
			send_msg(" ↳ [\00309%s\017]: [%s]", tweet.username, tweet.text);
		} else if(other_tweet_type == TW_RETWEET) {
			send_msg("[\00309%s\017 RT @\00311%s\017]: [%s]", tweet.username, other_tweet.username, other_tweet.text);
		} else {
			send_msg("[\00309%s\017]: [%s]\n", tweet.username, tweet.text);
		}

		sb_free(tweet.text);
		sb_free(other_tweet.text);

		uj_node_free(root, 1);
	}

	out:
		stb__sbn(msgbuf) = 0;
}

static size_t curl_callback(char* ptr, size_t sz, size_t nmemb, void* data) {
	size_t total = sz * nmemb;
	memcpy(sb_add(msgbuf, total), ptr, total);
	printf("tnf got data: %zu\n", total);
	print_msgs();
	return total;
}

static struct curl_req* curl_req_new(void) {
	struct curl_req* req = calloc(1, sizeof(*req));
	req->handle = curl_easy_init();

	const char* url = "https://api.twitter.com/2/tweets/search/stream"
		"?expansions=attachments.media_keys,author_id,referenced_tweets.id"
		"&media.fields=url,variants"
		"&tweet.fields=attachments,author_id,text,entities,created_at,referenced_tweets";

	req->headers = curl_slist_append(req->headers, auth_header);

	curl_easy_setopt(req->handle, CURLOPT_HTTPHEADER, req->headers);
	curl_easy_setopt(req->handle, CURLOPT_URL, url);
	curl_easy_setopt(req->handle, CURLOPT_WRITEFUNCTION, &curl_callback);
	curl_easy_setopt(req->handle, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(req->handle, CURLOPT_USERAGENT, "insobot");

	return req;
}

static void curl_req_free(struct curl_req* req) {
	if(!req) {
		return;
	}

	curl_multi_remove_handle(curl_multi, req);
	curl_easy_cleanup(req);

	curl_slist_free_all(req->headers);
	free(req);
}

static bool twitter_newsfeed_save(FILE* file) {
	char* entry = NULL;
	while((entry = argz_next(channels, channels_len, entry))) {
		fprintf(file, "%s\n", entry);
	}
	return true;
}

static bool twitter_newsfeed_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	curl_multi = curl_multi_init();
	if(!curl_multi) {
		return false;
	}

	const char* twitter_token = getenv("INSOBOT_TWITTER_TOKEN");
	if(!twitter_token) {
		return NULL;
	}

	FILE* f = fopen(ctx->get_datafile(), "r");
	if(!f) {
		return false;
	}

	char line[1024] = {};
	while(fscanf(f, "%1023s\n", line) == 1) {
		argz_add(&channels, &channels_len, line);
	}

	fclose(f);

	asprintf_check(&auth_header, "Authorization: Bearer %s", twitter_token);

	return true;
}

static void twitter_newsfeed_quit(void) {
	curl_req_free(curl_req);
	free(auth_header);
	sb_free(msgbuf);
	free(channels);
	curl_multi_cleanup(curl_multi);
}

static void twitter_newsfeed_tick(time_t now) {

	if(!curl_req) {
		curl_req = curl_req_new();
		curl_multi_add_handle(curl_multi, curl_req->handle);
		printf("TNF connection (re)started\n");
	}

	curl_multi_wait(curl_multi, NULL, 0, 0, NULL);

	int msgq = 0;
	curl_multi_perform(curl_multi, &msgq);

	do {
		CURLMsg* msg = curl_multi_info_read(curl_multi, &msgq);

		if(msg && msg->msg == CURLMSG_DONE) {
			printf("TNF connection ended\n");
			curl_req_free(curl_req);
			curl_req = NULL;
		}

	} while(msgq > 0);
}

static char* chan_find(const char* input) {
	char* entry = NULL;
	while((entry = argz_next(channels, channels_len, entry))) {
		if(strcmp(entry, input) == 0) {
			return entry;
		}
	}
	return NULL;
}

static void twitter_newsfeed_cmd(const char* chan, const char* name, const char* arg, int cmd){
	switch(cmd){
		case TWITTER_NEWSFEED_CMD: {
			if(strcmp(arg, " on") == 0) {
				char* entry = chan_find(chan);
				if(entry) {
					ctx->send_msg(chan, "It's already enabled here.");
				} else {
					argz_add(&channels, &channels_len, chan);
					ctx->send_msg(chan, "Twitter newsfeed enabled.");
				}
			} else if(strcmp(arg, " off") == 0) {
				char* entry = chan_find(chan);
				if(entry) {
					argz_delete(&channels, &channels_len, entry);
					ctx->send_msg(chan, "Twitter newsfeed disabled.");
				} else {
					ctx->send_msg(chan, "It's already disabled here.");
				}
			}

			ctx->save_me();

		} break;
	}
}

