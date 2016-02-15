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
	.flags    = IRC_MOD_DEFAULT,
	.on_msg   = &linkinfo_msg,
	.on_init  = &linkinfo_init,
};

static const IRCCoreCtx* ctx;

static regex_t yt_url_regex;
static regex_t yt_title_regex;

static regex_t msdn_url_regex;
static regex_t generic_title_regex;

static bool linkinfo_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	bool ret = true;

	ret = ret & (regcomp(
		&yt_url_regex,
		"youtu(\\.be\\/|be(-nocookie)?\\.com/(embed\\/|v\\/|watch\\?v=))([0-9A-Za-z_\\-]+)",
		REG_EXTENDED | REG_ICASE
	) == 0);

	ret = ret & (regcomp(
		&yt_title_regex,
		"&title=([^&]*)",
		REG_EXTENDED | REG_ICASE
	) == 0);

	ret = ret & (regcomp(
		&msdn_url_regex,
		"msdn\\.microsoft\\.com/[^/]+/library/.*\\.aspx",
		REG_EXTENDED | REG_ICASE
	) == 0);

	ret = ret & (regcomp(
		&generic_title_regex,
		"<title>([^<]+)</title>",
		REG_EXTENDED | REG_ICASE
	) == 0);

	return ret;
}

static size_t curl_callback(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = (char**)data;
	const size_t total = sz * nmemb;

	memcpy(sb_add(*out, total), ptr, total);

	return total;
}

static void do_youtube_info(const char* chan, const char* msg, regmatch_t* matches){
	regmatch_t* match = matches + 4;

	if(match->rm_so == -1 || match->rm_eo == -1) return;

	size_t matchsz = match->rm_eo - match->rm_so;

	const char url_prefix[] = "https://www.youtube.com/get_video_info?video_id=";
	const char url_suffix[] = "&el=vevo&el=embedded";
	const size_t psz = sizeof(url_prefix) - 1;
	const size_t ssz = sizeof(url_suffix) - 1;

	char* url = alloca(psz + ssz + matchsz + 1);

	memcpy(url, url_prefix, psz);
	memcpy(url + psz, msg + match->rm_so, matchsz);
	memcpy(url + psz + matchsz, url_suffix, ssz);

	url[psz + matchsz + ssz] = 0;

	char* data = NULL;

	fprintf(stderr, "linkinfo: Fetching [%s]\n", url);

	CURL* curl = curl_easy_init();

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
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

	curl_easy_cleanup(curl);

	sb_free(data);
}

void do_generic_info(const char* chan, const char* msg, regmatch_t* matches, const char* tag){

	const char url_prefix[] = "https://";
	int prefix_sz = sizeof(url_prefix) - 1;
	int match_sz  = matches[0].rm_eo - matches[0].rm_so;

	char* url = alloca(match_sz + sizeof(url_prefix));
	memcpy(url, url_prefix, prefix_sz);
	memcpy(url + prefix_sz, msg + matches[0].rm_so, match_sz);
	url[prefix_sz + match_sz] = 0;

	char* data = NULL;

	fprintf(stderr, "linkinfo: Fetching [%s]\n", url);

	CURL* curl = curl_easy_init();

	/* if you get curl SSL Connect errors here for some reason, it seems like a bug in 
	 * the gnutls version of curl, using the openssl version fixed it for me
	 */

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5);

	CURLcode curl_ret = curl_easy_perform(curl);

	sb_push(data, 0);

	regmatch_t title[2];
	int title_len = 0;

	if(
		curl_ret == 0 &&
		regexec(&generic_title_regex, data, 2, title, 0) == 0 &&
		(title_len = (title[1].rm_eo - title[1].rm_so)) > 0
	){
		ctx->send_msg(chan, "↑ %s: [%.*s]", tag, title_len, data + title[1].rm_so);
	} else {
		fprintf(stderr, "linkinfo: Couldn't extract title\n[%s]\n[%s]", curl_easy_strerror(curl_ret) ,data);
	}

	curl_easy_cleanup(curl);

	sb_free(data);
}

static void linkinfo_msg(const char* chan, const char* name, const char* msg){

	regmatch_t matches[5] = {};

	if(regexec(&yt_url_regex, msg, 5, matches, 0) == 0){
		do_youtube_info(chan, msg, matches);
	}

	if(regexec(&msdn_url_regex, msg, 1, matches, 0) == 0){
		do_generic_info(chan, msg, matches, "MSDN");
	}
}
