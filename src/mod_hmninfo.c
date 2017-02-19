#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "inso_xml.h"
#include "stb_sb.h"
#include <curl/curl.h>
#include <regex.h>

// TODO: perhaps this should be merged with mod_linkinfo or mod_hmnrss (with altered name)

static bool hmninfo_init (const IRCCoreCtx*);
static void hmninfo_quit (void);
static void hmninfo_msg  (const char*, const char*, const char*);
static void hmninfo_cmd  (const char*, const char*, const char*, int);

enum { HMNINFO_UPDATE };

const IRCModuleCtx irc_mod_ctx = {
	.name    = "hmninfo",
	.desc    = "Shows info about projects on HMN when referenced like ~project",
	.flags   = IRC_MOD_GLOBAL,
	.on_init = &hmninfo_init,
	.on_quit = &hmninfo_quit,
	.on_msg  = &hmninfo_msg,
	.on_cmd  = &hmninfo_cmd,
	.commands = DEFINE_CMDS(
		[HMNINFO_UPDATE] = CMD1("hmnpupdate")
	)
};

static const IRCCoreCtx* ctx;

typedef struct {
	char* info;
	char* name;
	char* slug;
	int   slug_len;
} HMNProject;

static HMNProject* projects;
static regex_t hmn_proj_regex;

static void hmninfo_update(void){
	char* data = NULL;
	CURL* curl = inso_curl_init("https://handmade.network/sitemap", &data);

	if(inso_curl_perform(curl, &data) == 200){

		uintptr_t* tokens = calloc(0x2000, sizeof(*tokens));
		ixt_tokenize(data, tokens, 0x2000, IXTF_SKIP_BLANK | IXTF_TRIM);

		enum {
			S_H3_FIND,
			S_H3_CONTENT,
			S_LI_FIND,
			S_LI_HREF,
			S_LI_CONTENT,
		} state = S_H3_FIND;

		regmatch_t m[2];
		char* url  = NULL;
		char* name = NULL;

		for(uintptr_t* t = tokens; *t; ++t){
			switch(state){
				case S_H3_FIND: {
					if(ixt_match(t, IXT_TAG_OPEN, "h3", NULL)){
						state = S_H3_CONTENT;
					}
				} break;

				case S_H3_CONTENT: {
					if(t[0] == IXT_CONTENT){
						if(strcmp((char*)t[1], "Projects") == 0){
							state = S_LI_FIND;
						} else {
							state = S_H3_FIND;
						}
					}
				} break;

				case S_LI_FIND: {
					if(ixt_match(t, IXT_TAG_OPEN, "li", NULL)){
						state = S_LI_HREF;
					}
				} break;

				case S_LI_HREF: {
					if(ixt_match(t, IXT_ATTR_KEY, "href", IXT_ATTR_VAL, NULL) && t[4] == IXT_CONTENT){
						url  = (char*)t[3];
						name = (char*)t[5];
						state = S_LI_CONTENT;
					}
				} break;

				case S_LI_CONTENT: {
					if(*t == IXT_CONTENT && ixt_match(t+2, IXT_TAG_OPEN, "div", NULL)){
						char* desc = (char*)t[1];

						if(regexec(&hmn_proj_regex, url, 2, m, 0) == 0 && m[1].rm_so != -1){
							HMNProject proj = { .name = strdup(name) };
							asprintf_check(&proj.slug, "~%.*s%n", m[1].rm_eo - m[1].rm_so, url + m[1].rm_so, &proj.slug_len);
							asprintf_check(&proj.info, "%s %s", url, desc);
							sb_push(projects, proj);

							printf("hmninfo: got project %s = %s\n", proj.name, proj.slug);
						}
						state = S_LI_FIND;
					}
				} break;
			}
		}
		free(tokens);
	}
	curl_easy_cleanup(curl);
	sb_free(data);
}

static bool hmninfo_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	regcomp(&hmn_proj_regex, "https://([^\\.]+)\\.handmade\\.network/", REG_EXTENDED | REG_ICASE);
	hmninfo_update();
	return true;
}

static void hmninfo_quit(void){
	for(HMNProject* p = projects; p < sb_end(projects); ++p){
		free(p->name);
		free(p->info);
		free(p->slug);
	}
	sb_free(projects);
	regfree(&hmn_proj_regex);
}

static void hmninfo_msg(const char* chan, const char* name, const char* msg){
	for(HMNProject* p = projects; p < sb_end(projects); ++p){
		const char* s = strcasestr(msg, p->slug);
		if(s && (s[p->slug_len] == ' ' || s[p->slug_len] == 0)){
			ctx->send_msg(chan, "↑ %s: %s", p->name, p->info);
			break;
		}
	}
}

static void hmninfo_cmd(const char* chan, const char* name, const char* arg, int cmd){
	for(HMNProject* p = projects; p < sb_end(projects); ++p){
		free(p->name);
		free(p->info);
		free(p->slug);
	}
	sb_free(projects);
	hmninfo_update();
	ctx->send_msg(chan, "%s: Project list updated.", name);
	// TODO: print out changes if any?
}
