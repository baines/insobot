#include "module.h"
#include <curl/curl.h>
#include <yajl/yajl_tree.h>
#include "utils.h"

static void info_cmd  (const char*, const char*, const char*, int);
static bool info_init (const IRCCoreCtx*);

enum { INFO_GET };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "info",
	.desc     = "Gets information about stuff from the internet",
	.flags    = IRC_MOD_DEFAULT,
	.on_cmd   = &info_cmd,
	.on_init  = &info_init,
	.commands = DEFINE_CMDS(
		[INFO_GET] = CMD("info")
	)
};

static const IRCCoreCtx* ctx;

enum { P_TYPE, P_ABSTEXT, P_ABSURL, P_RESULTS, P_RELATED, P_FIRSTURL, P_ANSWER, P_REDIRECT };

static const char* paths[][2] = {
	[P_TYPE]     = { "Type"         , NULL },
	[P_ABSTEXT]  = { "AbstractText" , NULL },
	[P_ABSURL]   = { "AbstractURL"  , NULL },
	[P_RESULTS]  = { "Results"      , NULL },
	[P_RELATED]  = { "RelatedTopics", NULL },
	[P_FIRSTURL] = { "FirstURL"     , NULL },
	[P_ANSWER]   = { "Answer"       , NULL },
	[P_REDIRECT] = { "Redirect"     , NULL },
};

static char* info_trim(const char* text, size_t maxlen){
	const char* p = text-1;
	int len = 0;

	do {
		p = strchrnul(p+1, '.');
		if(!len || p - text < maxlen){
			len = p - text;
		}
	} while(*p && p - text < maxlen);

	return strndup(text, len);
}

static size_t info_header_cb(char* buffer, size_t size, size_t nelem, void* arg){
	char* loc;
	if(buffer && sscanf(buffer, "Location: %m[^\r\n]", &loc) == 1){
		*(char**)arg = loc;
	}
	return size * nelem;
}

static const char* info_top_result(yajl_val results){
	if(results->u.array.len > 0 && YAJL_IS_OBJECT(results->u.array.values[0])){
		yajl_val result_link = yajl_tree_get(results->u.array.values[0], paths[P_FIRSTURL], yajl_t_string);
		if(result_link){
			return result_link->u.string;
		}
	}
	return NULL;
}

static void info_cmd(const char* chan, const char* nick, const char* arg, int cmd){
	if(cmd != INFO_GET || !inso_is_wlist(ctx, nick)) return;

	if(!*arg++){
		ctx->send_msg(chan, "What would you like info about, %s?", nick);
		return;
	}

	char* data = NULL;
	char* url;
	CURL* curl = curl_easy_init();

	{
		char* query = curl_easy_escape(curl, arg, 0);
		asprintf_check(&url, "https://api.duckduckgo.com/?q=%s&format=json&no_html=1&skip_disambig=1&t=insobot", query);
		curl_free(query);
	}

	inso_curl_reset(curl, url, &data);
	curl_easy_perform(curl);
	free(url);

	sb_push(data, 0);

	yajl_val root     = yajl_tree_parse(data, NULL, 0);

	yajl_val type     = yajl_tree_get(root, paths[P_TYPE]   , yajl_t_string);
	yajl_val abstract = yajl_tree_get(root, paths[P_ABSTEXT], yajl_t_string);
	yajl_val abs_url  = yajl_tree_get(root, paths[P_ABSURL] , yajl_t_string);
	yajl_val results  = yajl_tree_get(root, paths[P_RESULTS], yajl_t_array);
	yajl_val related  = yajl_tree_get(root, paths[P_RELATED], yajl_t_array);

	sb_free(data);

	if(!root || !type || !abstract || !abs_url || !results || !related){
		ctx->send_msg(chan, "Sorry, something went wrong getting information...");
		goto exit;
	}

	switch(*type->u.string){

		case 0: {
			ctx->send_msg(chan, "Sorry, I don't have any information about '%s.'", arg);
		} break;

		case 'D': {
			char choices[512] = {};
			const int limit = INSO_MIN(5, related->u.array.len);

			for(int i = 0; i < limit; ++i){
				yajl_val link = yajl_tree_get(related->u.array.values[i], paths[P_FIRSTURL], yajl_t_string);
				if(!link || strncmp(link->u.string, "https://duckduckgo.com/", 23) != 0) continue;

				if(*choices){
					inso_strcat(choices, sizeof(choices), ", ");
				}

				char* choice;
				if((choice = curl_easy_unescape(curl, link->u.string+23, 0, NULL))){
					size_t len = strlen(choices);
					inso_strcat(choices, sizeof(choices), choice);
					for(char* c = choices + len; *c; ++c){
						if(*c == '_') *c = ' ';
					}
					curl_free(choice);
				}
			}

			ctx->send_msg(chan, "'%s' could refer to: %s.", arg, choices);
		} break;

		case 'E': {
			yajl_val ans   = yajl_tree_get(root, paths[P_ANSWER]  , yajl_t_string);
			yajl_val redir = yajl_tree_get(root, paths[P_REDIRECT], yajl_t_string);

			if(ans && *ans->u.string){
				char* str = info_trim(ans->u.string, 200);
				ctx->send_msg(chan, "%s", str);
				free(str);
			} else if(redir && *redir->u.string){
				char* location = NULL;
				inso_curl_reset(curl, redir->u.string, &data);

				curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
				curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &info_header_cb);
				curl_easy_setopt(curl, CURLOPT_HEADERDATA, &location);

				curl_easy_perform(curl);
				curl_easy_cleanup(curl);

				sb_free(data);

				if(location){
					ctx->send_msg(chan, "%s: %s", nick, location);
					free(location);
				} else {
					ctx->send_msg(chan, "%s: %s", nick, redir->u.string);
				}
			} else {
				const char* link = info_top_result(results);
				if(!link)	link = abs_url->u.string;

				if(link){
					ctx->send_msg(chan, "%s: %s", nick, link);
				} else {
					ctx->send_msg(chan, "Sorry, I don't have any information about '%s.'", arg);
				}
			}

		} break;

		default: {
			char* desc = info_trim(abstract->u.string, 175);
			const char* link = info_top_result(results);
			if(!link)   link = abs_url->u.string;

			ctx->send_msg(chan, "%s. %s", desc, link);
			free(desc);
		} break;
	}

exit:
	curl_easy_cleanup(curl);
	yajl_tree_free(root);

}

static bool info_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

