#include "module.h"
#include <sys/types.h>
#include <regex.h>
#include <curl/curl.h>
#include <yajl/yajl_tree.h>
#include <string.h>
#include <assert.h>
#include "stb_sb.h"
#include "utils.h"

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

static regex_t twitter_url_regex;
static const char* twitter_token;

static regex_t steam_url_regex;

static bool linkinfo_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	bool ret = true;

	ret = ret & (regcomp(
		&yt_url_regex,
		"(y2u\\.be\\/|youtu\\.be\\/|youtube(-nocookie)?\\.com/(embed\\/|v\\/|watch\\?v=))([0-9A-Za-z_\\-]+)",
		REG_EXTENDED | REG_ICASE
	) == 0);

	ret = ret & (regcomp(
		&yt_title_regex,
		"title=([^&]*)",
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

	ret = ret & (regcomp(
		&twitter_url_regex,
		"twitter.com/[^/]+/status/([0-9]+)",
		REG_EXTENDED | REG_ICASE
	) == 0);

	ret = ret & (regcomp(
		&steam_url_regex,
		"store.steampowered.com/app/([0-9]+)",
		REG_EXTENDED | REG_ICASE
	) == 0);

	twitter_token = getenv("INSOBOT_TWITTER_TOKEN");
	if(!twitter_token || !*twitter_token){
		fputs("mod_linkinfo: no twitter token, expanding tweets won't work.\n", stderr);
	}

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

	sb_push(data, 0);

	if(
		result == 0 &&
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
		fprintf(stderr, "data_len = %zu, so:%d eo:%d\n", strlen(data), title[1].rm_so, title[1].rm_eo);
		ctx->send_msg(chan, "Error getting YT data. Blame insofaras.");
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
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8);

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
		fprintf(stderr, "linkinfo: Couldn't extract title\n[%s]\n[%s]", curl_easy_strerror(curl_ret), data);
		ctx->send_msg(chan, "Error getting %s data. Blame insofaras.", tag);
	}

	curl_easy_cleanup(curl);

	sb_free(data);
}

typedef struct {
	char *from, *to;
	size_t from_len, to_len;
} Replacement;

//XXX: this isn't very good. it only replaces a couple of escape sequences that turn up in the twitter api for whatever reason.
static void dodgy_html_unescape(char* msg, size_t len){

	#define RTAG(x, y) { .from = (x), .to = (y), .from_len = sizeof(x) - 1, .to_len = sizeof(y) - 1 }
	Replacement tags[] = {
		RTAG("&amp;", "&"),
		RTAG("&gt;", ">"),
		RTAG("&lt;", "<")
	};
	#undef RTAG

	for(char* p = msg; *p; ++p){
		for(int i = 0; i < sizeof(tags) / sizeof(*tags); ++i){
			if(strncmp(p, tags[i].from, tags[i].from_len) == 0){
				const int sz = tags[i].from_len - tags[i].to_len;
				assert(sz >= 0);

				memmove(p, p + sz, len - (p - msg));
				memcpy(p, tags[i].to, tags[i].to_len);
			}
		}
	}

}

static int twitter_add_url_replacements(Replacement** out, yajl_val urls, const char** exp_url_path){
	int size = 0;
	const char* tco_url_path[] = { "url", NULL };

	for(int i = 0; i < urls->u.array.len; ++i){
		yajl_val tco_url = yajl_tree_get(urls->u.array.values[i], tco_url_path, yajl_t_string);
		yajl_val exp_url = yajl_tree_get(urls->u.array.values[i], exp_url_path, yajl_t_string);

		if(!tco_url || !exp_url) continue;

		Replacement ur = {
			.from     = tco_url->u.string,
			.from_len = strlen(tco_url->u.string),
			.to       = exp_url->u.string,
			.to_len   = strlen(exp_url->u.string)
		};

		int size_diff = ur.to_len - ur.from_len;
		if(size_diff > 0) size += size_diff;

		sb_push(*out, ur);
	}

	return size;
}

static void do_twitter_info(const char* chan, const char* msg, regmatch_t* matches){

	if(!twitter_token){
		fputs("Can't fetch tweet, no twitter_token\n", stderr);
		return;
	}

	char* tweet_id = strndupa(msg + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
	char* auth_token = NULL;
	char* data = NULL;
	char* url = NULL;

	asprintf(&auth_token, "Authorization: Bearer %s", twitter_token);
	asprintf(&url, "https://api.twitter.com/1.1/statuses/show/%s.json", tweet_id);

	CURL* curl = curl_easy_init();

	struct curl_slist* headers = curl_slist_append(NULL, auth_token);

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

	curl_easy_perform(curl);

	sb_push(data, 0);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	free(auth_token);
	free(url);

//	printf("TWITTER DEBUG: [%s]\n", data);

	const char* text_path[] = { "text", NULL };
	const char* user_path[] = { "user", "name", NULL };
	const char* urls_path[] = { "entities", "urls", NULL };
	const char* media_path[] = { "entities", "media", NULL };

	yajl_val root = yajl_tree_parse(data, NULL, 0);

	if(!root){
		fprintf(stderr, "mod_linkinfo: Can't parse twitter json!\n");
		goto out;
	}

	yajl_val text = yajl_tree_get(root, text_path, yajl_t_string);
	yajl_val user = yajl_tree_get(root, user_path, yajl_t_string);
	yajl_val urls = yajl_tree_get(root, urls_path, yajl_t_array);
	yajl_val media = yajl_tree_get(root, media_path, yajl_t_array);

	if(!text || !user){
		fprintf(stderr, "mod_linkinfo: text or user null!\n");
		goto out;
	}

	Replacement* url_replacements = NULL;

	const char* exp_url_path[] = { "expanded_url", NULL };
	const char* media_url_path[] = { "media_url_https", NULL };

	size_t text_mem_size = strlen(text->u.string) + 1;

	//XXX: The expanding will break here if a media url comes before a url, can this happen?

	if(urls)  text_mem_size += twitter_add_url_replacements(&url_replacements, urls, exp_url_path);
	if(media) text_mem_size += twitter_add_url_replacements(&url_replacements, media, media_url_path);

	char* fixed_text = alloca(text_mem_size);

	const char* read_ptr = text->u.string;
	char* write_ptr = fixed_text;

	for(int i = 0; i < sb_count(url_replacements); ++i){
		const char* p = strstr(read_ptr, url_replacements[i].from);
		assert(p);

		size_t sz = p - read_ptr;
		memcpy(write_ptr, read_ptr, sz);
		memcpy(write_ptr + sz, url_replacements[i].to, url_replacements[i].to_len);

		read_ptr = p + url_replacements[i].from_len;
		write_ptr += (sz + url_replacements[i].to_len);
	}

	strcpy(write_ptr, read_ptr);

	for(unsigned char* c = fixed_text; *c; ++c) if(*c < ' ') *c = ' ';

	dodgy_html_unescape(fixed_text, strlen(fixed_text));

	sb_free(url_replacements);

	ctx->send_msg(chan, "↑ Tweet by %s: [%s]", user->u.string, fixed_text);

out:
	if(root){
		yajl_tree_free(root);
	}

	sb_free(data);
}

static void do_steam_info(const char* chan, const char* msg, regmatch_t* matches){
	const char* appid = strndupa(msg + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);

	char* url;
	asprintf(&url, "http://store.steampowered.com/api/appdetails?appids=%s&cc=US", appid);

	char* data = NULL;
	yajl_val root = NULL;

	CURL* curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8);

	CURLcode ret = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	free(url);

	if(ret != 0){
		fprintf(stderr, "mod_linkinfo: steam curl err %s\n", curl_easy_strerror(ret));
		goto out;
	}

	sb_push(data, 0);

	root = yajl_tree_parse(data, NULL, 0);
	if(!root){
		fprintf(stderr, "mod_linkinfo: steam root null!\n");
		goto out;
	}

	const char* title_path[] = { appid, "data", "name", NULL };
	const char* price_path[] = { appid, "data", "price_overview", "final", NULL };
	const char* plats_path[] = { appid, "data", "platforms", NULL };

	yajl_val title = yajl_tree_get(root, title_path, yajl_t_string);
	yajl_val price = yajl_tree_get(root, price_path, yajl_t_number);
	yajl_val plats = yajl_tree_get(root, plats_path, yajl_t_object);

	if(!title || !price || !plats){
		fprintf(stderr, "mod_linkinfo: steam title/price/plats null!\n");
		goto out;
	}

	char plat_str[16] = {};
	for(int i = 0; i < plats->u.object.len; ++i){
		if(!plats->u.object.values[i] || plats->u.object.values[i]->type != yajl_t_true){
			continue;
		}

		const char* tag = NULL;
		if(*plat_str) inso_strcat(plat_str, sizeof(plat_str), "/");

		if(strcmp(plats->u.object.keys[i], "linux"  ) == 0) tag = "LNX";
		if(strcmp(plats->u.object.keys[i], "mac"    ) == 0) tag = "MAC";
		if(strcmp(plats->u.object.keys[i], "windows") == 0) tag = "WIN";

		if(tag)	inso_strcat(plat_str, sizeof(plat_str), tag);
	}

	int price_hi = price->u.number.i / 100, price_lo = price->u.number.i % 100;

	ctx->send_msg(chan, "↑ Steam: [%s] [%s] [$%d.%2d]", title->u.string, plat_str, price_hi, price_lo);

out:
	if(data) sb_free(data);
	if(root) yajl_tree_free(root);
}

static void linkinfo_msg(const char* chan, const char* name, const char* msg){

	regmatch_t matches[5] = {};

	if(regexec(&yt_url_regex, msg, 5, matches, 0) == 0){
		do_youtube_info(chan, msg, matches);
	}

	if(regexec(&msdn_url_regex, msg, 1, matches, 0) == 0){
		do_generic_info(chan, msg, matches, "MSDN");
	}

	if(regexec(&twitter_url_regex, msg, 2, matches, 0) == 0){
		do_twitter_info(chan, msg, matches);
	}

	if(regexec(&steam_url_regex, msg, 2, matches, 0) == 0){
		do_steam_info(chan, msg, matches);
	}
}
