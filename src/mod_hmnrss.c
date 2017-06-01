#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "inso_xml.h"
#include "stb_sb.h"
#include <curl/curl.h>
#include <regex.h>

// TODO: add commands, and more than just HMN's rss feed

//#define DEBUG_MODE

static bool hmnrss_init (const IRCCoreCtx*);
static void hmnrss_quit (void);
static void hmnrss_tick (time_t);

const IRCModuleCtx irc_mod_ctx = {
	.name    = "hmnrss",
	.desc    = "Notifies about new blog posts on HMN",
	.flags   = IRC_MOD_GLOBAL,
	.on_init = &hmnrss_init,
	.on_quit = &hmnrss_quit,
	.on_tick = &hmnrss_tick,
};

static const IRCCoreCtx* ctx;
static CURL* curl;
static char* etag;
static time_t latest_post;
static time_t last_check;
static regex_t url_regex;

typedef struct {
	char* name;
	time_t last_post;
} HMNMember;

static HMNMember hmn_members[32];
static size_t    hmn_member_idx;

#ifdef DEBUG_MODE
	#define RSS_URL "http://127.0.0.1:8000/rss.xml"
#else
	#define RSS_URL "https://handmade.network/atom"
#endif

static size_t etag_cb(char* buffer, size_t size, size_t nelem, void* arg){
	char* new_etag;

	if(buffer && sscanf(buffer, "ETag: %m[^\r\n]", &new_etag) == 1){
		free(etag);
		printf("hmnrss: ETag: %s\n", new_etag);
		etag = new_etag;
	}

	return size * nelem;
}

static bool hmnrss_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	curl = curl_easy_init();
#ifdef DEBUG_MODE
	last_check = time(0) - 50;
#else
	last_check = latest_post = time(0);
#endif
	regcomp(&url_regex, "https://([^\\.]*)\\.?handmade\\.network/.*/[0-9]+", REG_ICASE | REG_EXTENDED);

	return true;
}

static void hmnrss_quit(void){
	curl_easy_cleanup(curl);
	free(etag);
	regfree(&url_regex);

	for(size_t i = 0; i < ARRAY_SIZE(hmn_members); ++i){
		free(hmn_members[i].name);
	}
}

static bool hmnrss_check_spam(const char* msg, time_t post_time, const char* member_url){
	const char* member_name = strrchr(member_url, '/') + 1;

	HMNMember* member = NULL;
	for(size_t i = 0; i < ARRAY_SIZE(hmn_members); ++i){
		if(hmn_members[i].name && strcmp(hmn_members[i].name, member_name) == 0){
			member = hmn_members + i;
			break;
		}
	}

	if(member){
		time_t prev_post = member->last_post;
		member->last_post = post_time;

		if(labs(post_time - prev_post) < 90){
			printf("hmnrss: [%s] posting too quickly, spam? [%zu : %zu]\n", member_name, (size_t)post_time, (size_t)prev_post);
			return true;
		}
	} else {
		HMNMember* m = hmn_members + hmn_member_idx;
		free(m->name);
		m->name = strdup(member_name);
		m->last_post = post_time;
		hmn_member_idx = (hmn_member_idx + 1) % ARRAY_SIZE(hmn_members);
	}

	static const char* susp_words[] = {
		"buy", "drug", "pharmacy", "insurance", "degree transcript", "nike",
	};

	for(size_t i = 0; i < ARRAY_SIZE(susp_words); ++i){
		if(strcasestr(msg, susp_words[i])){
			printf("hmnrss: [%s] spam word found: %s.\n", member_name, susp_words[i]);
			return true;
		}
	}

	wchar_t wc;
	int ret;
	size_t wclen = 0;
	size_t unusual = 0;

	const char* p = msg;
	const char* end = msg + strlen(msg);

	while((ret = mbtowc(&wc, p, end - p)) > 0){
		if(wc > 255){
			++unusual;
		}
		++wclen;
		p += ret;
	}

	if(ret == -1 || (unusual / (float)wclen) > 0.2){
		printf("hmnrss: [%s] too many non-ascii chars.\n", member_name);
		return true;
	}

	return false;
}

static void hmnrss_tick(time_t now){
	if(now - last_check < 60) return;
	last_check = now;

	char* data = NULL;
	inso_curl_reset(curl, RSS_URL, &data);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &etag_cb);
	struct curl_slist* headers = NULL;

	if(etag){
		char buf[1024];
		snprintf(buf, sizeof(buf), "If-None-Match: %s", etag);
		headers = curl_slist_append(NULL, buf);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	}

	time_t new_latest_post = latest_post;
	long ret = inso_curl_perform(curl, &data);

#ifdef DEBUG_MODE
	printf("hmnrss: doing check...\n");
#endif

	if(ret == 200){
		uintptr_t tokens[0x1000];
		if(ixt_tokenize(data, tokens, 0x1000, IXTF_SKIP_BLANK | IXTF_TRIM) != IXTR_OK) return;

		char *message = NULL, *url = NULL, *member_url = NULL;
		time_t published = 0;

		int message_count = 0;

		for(uintptr_t* t = tokens; *t; ++t){

			// #1: get message
			if(ixt_match(t, IXT_TAG_OPEN, "title", IXT_CONTENT, NULL) && (
					strncmp((char*)t[3], "Blog Post:", 10) == 0 ||
					strncmp((char*)t[3], "Forum Thread:", 13) == 0)){
				message = (char*)t[3];
			}

			// #2: get url
			if(message && ixt_match(t, IXT_ATTR_KEY, "href", IXT_ATTR_VAL, NULL) && t[3]){
				url = (char*)t[3];
			}

			// #3: get published date
			if(url && ixt_match(t, IXT_TAG_OPEN, "published", IXT_CONTENT, NULL)){
				struct tm pub_tm = {};
				char* c = strptime((char*)t[3], "%Y-%m-%dT%H:%M:%S", &pub_tm);
				time_t pub = mktime(&pub_tm);

				if(c && *c == '.'){
					if(pub <= latest_post){
						break;
					} else if(pub > latest_post){
						published = pub;
						if(pub > new_latest_post){
							new_latest_post = pub;
						}
					}
				}
			}

			// #4: get member url
			if(published && ixt_match(t, IXT_TAG_OPEN, "uri", IXT_CONTENT, NULL)
				&& t[3]
				&& strncmp((char*)t[3], "https://handmade.network/m/", 27) == 0){
				member_url = (char*)t[3];
			}

			// #5: if we got everything, check it and send it.
			if(message && url && published && member_url){
				regmatch_t m[2];

				if(regexec(&url_regex, url, 2, m, 0) == 0 && m[0].rm_so >= 0 && m[1].rm_so >= 0){
					int   url_chan_sz = m[1].rm_eo - m[1].rm_so;
					char* url_chan    = alloca(url_chan_sz + 2);

					sprintf(url_chan, "#%.*s", url_chan_sz, url + m[1].rm_so);

					const char* chan = inso_in_chan(ctx, url_chan) ? url_chan : "#random";
					const char* link = url + m[0].rm_so;

					if(message_count++ < 3 && !hmnrss_check_spam(message, published, member_url)){
						ctx->send_msg(chan, "New HMN %s | %.*s", message, m[0].rm_eo - m[0].rm_so, link);
					}
					message = url = member_url = NULL;
					published = 0;
				}
			}

		}
	} else if(ret != 304){
		printf("mod_hmnrss: http %ld\n", ret);
	}

	latest_post = new_latest_post;

	if(headers){
		curl_slist_free_all(headers);
	}

	sb_free(data);
}
