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

static char* text_trim(const char* text){
	const char* p = text;
	int len = 0;

	do {
		p = strchrnul(p+1, '.');
		if(!len || p - text < 150){
			len = p - text;
		}
	} while(*p && p - text < 150);

	return strndup(text, len);
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
	curl_easy_cleanup(curl);
	free(url);

	sb_push(data, 0);

	static const char* paths[][2] = {
		{ "Type", NULL },
		{ "AbstractText", NULL },
		{ "AbstractURL", NULL },
		{ "Results", NULL },
		{ "RelatedTopics", NULL },
		{ "FirstURL", NULL },
	};

	yajl_val root     = yajl_tree_parse(data, NULL, 0);
	yajl_val type     = yajl_tree_get(root, paths[0], yajl_t_string);
	yajl_val abstract = yajl_tree_get(root, paths[1], yajl_t_string);
	yajl_val abs_url  = yajl_tree_get(root, paths[2], yajl_t_string);
	yajl_val results  = yajl_tree_get(root, paths[3], yajl_t_array);
	yajl_val related  = yajl_tree_get(root, paths[4], yajl_t_array);

	if(!root || !type || !abstract || !abs_url || !results || !related){
		ctx->send_msg(chan, "Sorry, something went wrong getting information...");
		return;
	}

	switch(*type->u.string){

		case 0: {
			ctx->send_msg(chan, "Sorry, I don't have any information about '%s.'", arg);
		} break;

		case 'D': {
			char choices[512] = {};
			const int limit = INSO_MIN(5, related->u.array.len);

			for(int i = 0; i < limit; ++i){
				yajl_val link = yajl_tree_get(related->u.array.values[i], paths[5], yajl_t_string);
				if(!link || strncmp(link->u.string, "https://duckduckgo.com/", 23) != 0) continue;

				if(*choices){
					inso_strcat(choices, sizeof(choices), ", ");
				}

				size_t len = strlen(choices);
				inso_strcat(choices, sizeof(choices), link->u.string+23);

				for(char* c = choices + len; *c; ++c){
					if(*c == '_') *c = ' ';
				}
			}

			ctx->send_msg(chan, "'%s' could refer to: %s.", arg, choices);
		} break;

		default: {
			char* desc = text_trim(abstract->u.string);
			const char* link = abs_url->u.string;

			if(results->u.array.len > 0 && YAJL_IS_OBJECT(results->u.array.values[0])){
				yajl_val result_link = yajl_tree_get(results->u.array.values[0], paths[5], yajl_t_string);
				if(result_link){
					link = result_link->u.string;
				}
			}

			ctx->send_msg(chan, "%s. %s", desc, link);
			free(desc);
		} break;
	}

	sb_free(data);
	yajl_tree_free(root);

}

static bool info_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

