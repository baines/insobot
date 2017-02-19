#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "inso_xml.h"
#include "stb_sb.h"
#include <curl/curl.h>
#include <regex.h>

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
static regex_t url_regex;

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

	regcomp(&url_regex, "https://([^\\.]*)\\.?handmade\\.network/.*/[0-9]+", REG_ICASE | REG_EXTENDED);

	return true;
}

static void hmnrss_quit(void){
	curl_easy_cleanup(curl);
	free(etag);
	regfree(&url_regex);
}

static void hmnrss_tick(time_t now){
	if(now - last_check < 60) return;
	last_check = now;

	struct curl_slist* headers = NULL;

	if(etag){
		char buf[1024];
		snprintf(buf, sizeof(buf), "If-None-Match: %s", etag);
		headers = curl_slist_append(NULL, buf);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	}

	time_t new_latest_post = latest_post;

	long ret = inso_curl_perform(curl, &data);

	if(ret == 200){
		uintptr_t tokens[0x1000];
		if(ixt_tokenize(data, tokens, 0x1000, IXTF_SKIP_BLANK | IXTF_TRIM) != IXTR_OK) return;

		char *message = NULL, *url = NULL;

		for(uintptr_t* t = tokens; *t; ++t){
			if(ixt_match(t, IXT_TAG_OPEN, "published", IXT_CONTENT, NULL)){
				struct tm pub_tm = {};
				char* c = strptime((char*)t[3], "%Y-%m-%dT%H:%M:%S", &pub_tm);
				time_t pub = mktime(&pub_tm);

				if(c && *c == '.'){
					if(pub <= latest_post){
						break;
					} else if(pub > latest_post){
						regmatch_t m[2];

						if(message && url && regexec(&url_regex, url, 2, m, 0) == 0 && m[0].rm_so >= 0 && m[1].rm_so >= 0){
							int   url_chan_sz = m[1].rm_eo - m[1].rm_so;
							char* url_chan    = alloca(url_chan_sz + 2);

							sprintf(url_chan, "#%.*s", url_chan_sz, url + m[1].rm_so);

							const char* chan = inso_in_chan(ctx, url_chan) ? url_chan : "#random";
							const char* link = url + m[0].rm_so;

							ctx->send_msg(chan, "New HMN %s | %.*s", message, m[0].rm_eo - m[0].rm_so, link);
							message = url = NULL;
						}

						if(pub > new_latest_post){
							new_latest_post = pub;
						}
					}
				}
			}

			if(message && ixt_match(t, IXT_ATTR_KEY, "href", IXT_ATTR_VAL, NULL) && t[3]){
				url = (char*)t[3];
			}

			if(ixt_match(t, IXT_TAG_OPEN, "title", IXT_CONTENT, NULL) && (
					strncmp((char*)t[3], "Blog Post:", 10) == 0 ||
					strncmp((char*)t[3], "Forum Thread:", 13) == 0)){
				message = (char*)t[3];
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
