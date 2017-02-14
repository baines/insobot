#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "inso_xml.h"
#include <curl/curl.h>
#include "stb_sb.h"

// TODO: add commands, and more than just HMN's rss feed

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
static char* data;

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
	curl = inso_curl_init("https://handmade.network/atom", &data);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &etag_cb);
	last_check = latest_post = time(0);
	return true;
}

static void hmnrss_quit(void){
	curl_easy_cleanup(curl);
	free(etag);
}

static void hmnrss_tick(time_t now){
	if(now - last_check < 60) return;
	last_check = now;

	struct curl_slist* headers = NULL;

	puts("mod_hmnrss: doing check.");

	if(etag){
		char buf[1024];
		snprintf(buf, sizeof(buf), "If-None-Match: %s", etag);
		headers = curl_slist_append(NULL, buf);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	}

	time_t new_latest_post = latest_post;

	long ret = inso_curl_perform(curl, &data);

	if(ret == 200){
		intptr_t tokens[0x2000];
		if(ixt_tokenize(data, tokens, 0x2000) != IXTR_OK) return;

		char *message = NULL, *url = NULL;

		for(intptr_t* t = tokens; *t; ++t){

			if (t[0] == IXT_TAG_OPEN &&
				strcmp((char*)t[1], "published") == 0 &&
				t[2] == IXT_CONTENT
			){
				struct tm pub_tm = {};
				char* c = strptime((char*)t[3], "%Y-%m-%dT%H:%M:%S", &pub_tm);
				time_t pub = mktime(&pub_tm);

				if(c && *c == '.'){
					if(pub <= latest_post){
						break;
					} else if(pub > latest_post){
						if(message && url){
							// FIXME: don't hard-code channel
							ctx->send_msg("#random", "New HMN %s | %s", message, url);
							message = url = NULL;
						}

						if(pub > new_latest_post){
							new_latest_post = pub;
						}
					}
				}
			}

			if (message &&
				t[0] == IXT_ATTR_KEY &&
				strcmp((char*)t[1], "href") == 0 &&
				t[2] == IXT_ATTR_VAL &&	t[3]
			){
				url = (char*)t[3];
				*strchrnul(url, '-') = 0;
			}

			if (t[0] == IXT_TAG_OPEN &&
				strcmp((char*)t[1], "title") == 0 &&
				t[2] == IXT_CONTENT &&
				strncmp((char*)t[3], "Blog Post:", 10) == 0
			){
				message = (char*)t[3];
			}
		}
	} else {
		printf("mod_hmnrss: http %ld\n", ret);
	}

	latest_post = new_latest_post;

	if(headers){
		curl_slist_free_all(headers);
	}

	sb_free(data);
}
