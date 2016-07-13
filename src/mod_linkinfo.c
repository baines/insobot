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
static void linkinfo_quit (void);

const IRCModuleCtx irc_mod_ctx = {
	.name     = "linkinfo",
	.desc     = "Shows information about some links posted in the chat.",
	.flags    = IRC_MOD_DEFAULT,
	.on_msg   = &linkinfo_msg,
	.on_init  = &linkinfo_init,
	.on_quit  = &linkinfo_quit
};

static const IRCCoreCtx* ctx;

static regex_t yt_url_regex;
static regex_t yt_title_regex;
static regex_t yt_length_regex;

static regex_t msdn_url_regex;
static regex_t generic_title_regex;

static regex_t twitter_url_regex;
static const char* twitter_token;

static regex_t steam_url_regex;

static regex_t vimeo_url_regex;

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
		&yt_length_regex,
		"length_seconds=([0-9]+)",
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

	ret = ret & (regcomp(
		&vimeo_url_regex,
		"https?://vimeo.com/([0-9]+)",
		REG_EXTENDED | REG_ICASE
	) == 0);

	twitter_token = getenv("INSOBOT_TWITTER_TOKEN");
	if(!twitter_token || !*twitter_token){
		fputs("mod_linkinfo: no twitter token, expanding tweets won't work.\n", stderr);
	}

	return ret;
}

static void linkinfo_quit(void){
	regfree(&yt_url_regex);
	regfree(&yt_title_regex);
	regfree(&yt_length_regex);
	regfree(&msdn_url_regex);
	regfree(&generic_title_regex);
	regfree(&twitter_url_regex);
	regfree(&steam_url_regex);
	regfree(&vimeo_url_regex);
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

	CURL* curl = inso_curl_init(url, &data);
	CURLcode result = curl_easy_perform(curl);
	sb_push(data, 0);

	regmatch_t title[2], length[2];

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

		enum { SEC_IN_HOUR = (60*60), SEC_IN_MIN = 60 };

		char length_str[32] = {};
		char* ls_ptr = length_str;
		size_t ls_sz = sizeof(length_str);

		if(strstr(data, "ps=live")){
			strcpy(length_str, "LIVE");
		} else if(regexec(&yt_length_regex, data, 2, length, 0) == 0){
			int secs = strtoul(data + length[1].rm_so, NULL, 10);
			if(secs > SEC_IN_HOUR){
				snprintf_chain(&ls_ptr, &ls_sz, "%d:", secs / SEC_IN_HOUR);
				secs %= SEC_IN_HOUR;
			}
			snprintf_chain(&ls_ptr, &ls_sz, "%02d:", secs / SEC_IN_MIN);
			snprintf_chain(&ls_ptr, &ls_sz, "%02d", secs % SEC_IN_MIN);
		} else {
			strcpy(length_str, "00:00");
		}

		ctx->send_msg(chan, "↑ YT Video: [%.*s] [%s]", outlen, str, length_str);

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


	/* if you get curl SSL Connect errors here for some reason, it seems like a bug in 
	 * the gnutls version of curl, using the openssl version fixed it for me
	 */

	CURL* curl = inso_curl_init(url, &data);
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

static const char* url_path[]        = { "url", NULL };
static const char* exp_url_path[]    = { "expanded_url", NULL };
static const char* media_url_path[]  = { "media_url_https", NULL };
static const char* video_info_path[] = { "video_info", "variants", NULL };
static const char* bitrate_path[]    = { "bitrate", NULL };

static int twitter_parse_entities(Replacement** out, yajl_val entity, bool is_media){
	int size = 0;
	bool is_video = false;

	for(int i = 0; i < entity->u.array.len; ++i){
		yajl_val exp_url;

		if(is_media && (exp_url = yajl_tree_get(entity->u.array.values[i], video_info_path, yajl_t_array))){
			int best_bitrate = 0;
			int best_index = 0;

			for(int j = 0; j < exp_url->u.array.len; ++j){
				yajl_val bitrate = yajl_tree_get(exp_url->u.array.values[j], bitrate_path, yajl_t_number);
				if(bitrate && bitrate->u.number.i >= best_bitrate){
					best_bitrate = bitrate->u.number.i;
					best_index = j;
				}
			}

			is_video = true;
			exp_url = yajl_tree_get(exp_url->u.array.values[best_index], url_path, yajl_t_string);

		} else {
			const char** exp_path = is_media ? media_url_path : exp_url_path;
			exp_url = yajl_tree_get(entity->u.array.values[i], exp_path, yajl_t_string);
		}

		yajl_val tco_url = yajl_tree_get(entity->u.array.values[i], url_path, yajl_t_string);

		if(!tco_url || !exp_url) continue;

		//XXX: better image url hack
		if(is_media && !is_video){
			char* better_url;
			asprintf_check(&better_url, "%s:orig", exp_url->u.string);
			free(exp_url->u.string);
			exp_url->u.string = better_url;
		}

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

	asprintf_check(&auth_token, "Authorization: Bearer %s", twitter_token);
	asprintf_check(&url, "https://api.twitter.com/1.1/statuses/show/%s.json", tweet_id);

	CURL* curl = inso_curl_init(url, &data);

	struct curl_slist* headers = curl_slist_append(NULL, auth_token);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	curl_easy_perform(curl);

	sb_push(data, 0);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	free(auth_token);
	free(url);

//	printf("TWITTER DEBUG: [%s]\n", data);

	const char* date_path[] = { "created_at", NULL };
	const char* text_path[] = { "text", NULL };
	const char* user_path[] = { "user", "name", NULL };
	const char* urls_path[] = { "entities", "urls", NULL };
	const char* media_path[] = { "extended_entities", "media", NULL };

	yajl_val root = yajl_tree_parse(data, NULL, 0);

	if(!root){
		fprintf(stderr, "mod_linkinfo: Can't parse twitter json!\n");
		goto out;
	}

	yajl_val date = yajl_tree_get(root, date_path, yajl_t_string);
	yajl_val text = yajl_tree_get(root, text_path, yajl_t_string);
	yajl_val user = yajl_tree_get(root, user_path, yajl_t_string);
	yajl_val urls = yajl_tree_get(root, urls_path, yajl_t_array);
	yajl_val media = yajl_tree_get(root, media_path, yajl_t_array);

	if(!date || !text || !user){
		fprintf(stderr, "mod_linkinfo: date/text/user null!\n");
		goto out;
	}

	struct tm tweet_tm = {};
	strptime(date->u.string, "%a %b %d %T %z %Y", &tweet_tm);
	time_t time_diff = time(0) - timegm(&tweet_tm);
	char time_buf[32] = {};

	// TODO: make a general version of this duration-to-string stuff and put it in utils.h
	// since i end up doing this differently everywhere...

	struct {
		const char* unit;
		int limit;
		int divisor;
	} time_info[] = { { "s", 60, 1}, { "m", (60*60), 60 }, { "h", (60*60*24), (60*60) }, { "d", (60*60*24*365), (60*60*24) } };

	for(int i = 0; i < ARRAY_SIZE(time_info); ++i){
		if(time_diff < time_info[i].limit){
			snprintf(time_buf, sizeof(time_buf), "%d%s ago", (int)time_diff / time_info[i].divisor, time_info[i].unit);
			break;
		}
	}

	if(!*time_buf){
		strftime(time_buf, sizeof(time_buf), "%F", &tweet_tm);
	}

	Replacement* url_replacements = NULL;

	size_t text_mem_size = strlen(text->u.string) + 1;

	//XXX: The expanding will break here if a media url comes before a url, can this happen?
	if(urls)  text_mem_size += twitter_parse_entities(&url_replacements, urls, false);
	if(media) text_mem_size += twitter_parse_entities(&url_replacements, media, true);

	char* fixed_text = alloca(text_mem_size);

	const char* read_ptr = text->u.string;
	char* write_ptr = fixed_text;

	for(int i = 0; i < sb_count(url_replacements); ++i){
		const char* p = strstr(read_ptr, url_replacements[i].from);
		if(!p) continue;

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

	ctx->send_msg(chan, "↑ Tweet by %s: [%s] [%s]", user->u.string, fixed_text, time_buf);

out:
	if(root){
		yajl_tree_free(root);
	}

	sb_free(data);
}

static void do_steam_info(const char* chan, const char* msg, regmatch_t* matches){
	const char* appid = strndupa(msg + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);

	char* url;
	asprintf_check(&url, "http://store.steampowered.com/api/appdetails?appids=%s&cc=US", appid);

	char* data = NULL;
	yajl_val root = NULL;

	CURL* curl = inso_curl_init(url, &data);
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

	const char* title_path[]  = { appid, "data", "name", NULL };
	const char* price_path[]  = { appid, "data", "price_overview", "final", NULL };
	const char* plats_path[]  = { appid, "data", "platforms", NULL };
	const char* csoon_path[]  = { appid, "data", "release_date", "coming_soon", NULL };
	const char* isfree_path[] = { appid, "data", "is_free", NULL };

	yajl_val title  = yajl_tree_get(root, title_path, yajl_t_string);
	yajl_val price  = yajl_tree_get(root, price_path, yajl_t_number);
	yajl_val plats  = yajl_tree_get(root, plats_path, yajl_t_object);
	yajl_val csoon  = yajl_tree_get(root, csoon_path, yajl_t_any);
	yajl_val isfree = yajl_tree_get(root, isfree_path, yajl_t_any);

	if(!title || !plats){
		fprintf(stderr, "mod_linkinfo: steam title/plats null!\n");
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

	if(price){
		int price_hi = price->u.number.i / 100, price_lo = price->u.number.i % 100;
		ctx->send_msg(chan, "↑ Steam: [%s] [%s] [$%d.%02d]", title->u.string, plat_str, price_hi, price_lo);
	} else {
		const char* status = YAJL_IS_TRUE(isfree) ? "[Free]" : YAJL_IS_TRUE(csoon) ? "[Coming Soon]" : "";
		ctx->send_msg(chan, "↑ Steam: [%s] [%s] %s", title->u.string, plat_str, status);
	}

out:
	if(data) sb_free(data);
	if(root) yajl_tree_free(root);
}

static void do_vimeo_info(const char* chan, const char* msg, regmatch_t* matches){

	char* data = NULL;
	yajl_val root = NULL;

	char* id = strndupa(msg + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
	char* url;
	asprintf_check(&url, "https://vimeo.com/api/oembed.json?url=https%%3A%%2F%%2Fvimeo.com%%2F%s", id); 

	CURL* curl = inso_curl_init(url, &data);
	int ret = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	free(url);

	sb_push(data, 0);

	if(ret != 0){
		fprintf(stderr, "mod_linkinfo: vimeo curl err %s\n", curl_easy_strerror(ret));
		ctx->send_msg(chan, "Error getting Vimeo data. Blame insofaras.");
		goto out;
	}

	root = yajl_tree_parse(data, NULL, 0);
	if(!root){
		fprintf(stderr, "mod_linkinfo: vimeo err getting root json\n");
		ctx->send_msg(chan, "Error getting Vimeo data. Blame insofaras.");
		goto out;
	}

	const char* title_path[] = { "title", NULL };
	const char* duration_path[] = { "duration", NULL };

	yajl_val title = yajl_tree_get(root, title_path, yajl_t_string);
	yajl_val duration = yajl_tree_get(root, duration_path, yajl_t_number);

	if(!title || !duration){
		fprintf(stderr, "mod_linkinfo: vimeo err title/duration null\n");
		ctx->send_msg(chan, "Error getting Vimeo data. Blame insofaras.");
		goto out;
	}

	enum { SEC_IN_HOUR = (60*60), SEC_IN_MIN = 60 };

	char length_str[32] = {};
	char* ls_ptr = length_str;
	size_t ls_sz = sizeof(length_str);

	int secs = duration->u.number.i;

	if(secs > SEC_IN_HOUR){
		snprintf_chain(&ls_ptr, &ls_sz, "%d:", secs / SEC_IN_HOUR);
		secs %= SEC_IN_HOUR;
	}
	snprintf_chain(&ls_ptr, &ls_sz, "%02d:", secs / SEC_IN_MIN);
	snprintf_chain(&ls_ptr, &ls_sz, "%02d", secs % SEC_IN_MIN);

	ctx->send_msg(chan, "↑ Vimeo: [%s] [%s]", title->u.string, length_str);

out:
	if(root) yajl_tree_free(root);
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

	if(regexec(&twitter_url_regex, msg, 2, matches, 0) == 0){
		do_twitter_info(chan, msg, matches);
	}

	if(regexec(&steam_url_regex, msg, 2, matches, 0) == 0){
		do_steam_info(chan, msg, matches);
	}

	if(regexec(&vimeo_url_regex, msg, 2, matches, 0) == 0){
		do_vimeo_info(chan, msg, matches);
	}
}
