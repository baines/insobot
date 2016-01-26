#include "module.h"
#include <sys/types.h>
#include <regex.h>
#include <curl/curl.h>
#include <string.h>
#include "stb_sb.h"

static void linkinfo_msg  (const char*, const char*, const char*);
static bool linkinfo_init (const IRCCoreCtx*);

const IRCModuleCtx irc_mod_ctx = {
	.name     = "linkinfo",
	.desc     = "Shows information about some links posted in the chat.",
	.on_msg   = &linkinfo_msg,
	.on_init  = &linkinfo_init,
};

static const IRCCoreCtx* ctx;

static CURL* curl;

static regex_t yt_url_regex;
static regex_t yt_title_regex;

static bool linkinfo_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	bool ret = true;

	ret = ret & regcomp(
		&yt_url_regex,
		"youtu(\\.be\\/|be(-nocookie)?\\.com/(embed\\/|v\\/|watch\\?v=))([0-9A-Za-z_\\-]+)",
		REG_EXTENDED | REG_ICASE
	);

	ret = ret & regcomp(
		&yt_title_regex,
		"&title=([^&]*)",
		REG_EXTENDED | REG_ICASE
	);

	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");

	return ret;
}

static size_t curl_callback(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = (char**)data;
	const size_t total = sz * nmemb;

	memcpy(sb_add(*out, total), ptr, total);

	return total;
}

static void linkinfo_msg(const char* chan, const char* name, const char* msg){

	regmatch_t matches[5] = {};

	if(regexec(&yt_url_regex, msg, 5, matches, 0) == 0){
		regmatch_t* match = matches + 4;

		if(match->rm_so == -1 || match->rm_eo == -1) return;

		size_t matchsz = match->rm_eo - match->rm_so;

		const char url_prefix[] = "https://www.youtube.com/get_video_info?video_id=";
		char* url = alloca(sizeof(url_prefix) + matchsz);
		
		memcpy(url, url_prefix, sizeof(url_prefix) - 1);
		memcpy(url + sizeof(url_prefix) - 1, msg + match->rm_so, matchsz);
		
		url[sizeof(url_prefix) + matchsz - 1] = 0;

		char* data = NULL;

		fprintf(stderr, "linkinfo: Fetching [%s]\n", url);

		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
		
		regmatch_t title[2];

		CURLcode result = curl_easy_perform(curl);

		if(
			result == 0 &&
			data &&
			regexec(&yt_title_regex, data, 2, title, 0) == 0 &&
			title[1].rm_so != -1 &&
			title[1].rm_eo != -1
		){
			int len = title[1].rm_eo - title[1].rm_so, outlen = 0;

			for(char* p = data + title[1].rm_so; p < data + title[1].rm_eo; ++p){
				if(*p == '+') *p = ' ';
			}

			char* str = curl_easy_unescape(curl, data + title[1].rm_so, len, &outlen);

			for(int i = 0; i < outlen; ++i){
				if(!str[i]) str[i] = ' ';
			}

			ctx->send_msg(chan, "↑ YT Video: [%.*s]", outlen, str);

			curl_free(str);
		} else {
			fprintf(stderr, "linkinfo: curl returned %d: %s\n", result, curl_easy_strerror(result));
		}

		sb_free(data);
	}
}
