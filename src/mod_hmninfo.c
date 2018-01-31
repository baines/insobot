#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "inso_xml.h"
#include "stb_sb.h"
#include <curl/curl.h>
#include <regex.h>
#include <ctype.h>

static bool hmninfo_init (const IRCCoreCtx*);
static void hmninfo_quit (void);
static void hmninfo_msg  (const char*, const char*, const char*);
static void hmninfo_cmd  (const char*, const char*, const char*, int);

enum { HMN_UPDATE, HMN_ANNO_SEARCH };

const IRCModuleCtx irc_mod_ctx = {
	.name    = "hmninfo",
	.desc    = "Shows info about projects on HMN when referenced like ~project",
	.flags   = IRC_MOD_GLOBAL,
	.on_init = &hmninfo_init,
	.on_quit = &hmninfo_quit,
	.on_msg  = &hmninfo_msg,
	.on_cmd  = &hmninfo_cmd,
	.commands = DEFINE_CMDS(
		[HMN_UPDATE]      = CMD("hmnupdate") CMD("hmnup") CMD("hmnpupdate"),
		[HMN_ANNO_SEARCH] = CMD("as") CMD("ag") CMD("sa") CMD("agrep")
	),
	.cmd_help = DEFINE_CMDS(
		[HMN_UPDATE]      = "| Updates the list of known handmade.network projects",
		[HMN_ANNO_SEARCH] = "[~project] <regex> | Searches through video annotations"
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
static bool hmn_msg_suppress;
static CURL* curl;

struct anno_meta {
	int32_t  str_off;
	uint16_t ep_off;
	uint16_t time;
};

static struct ep_guide {
	const char*  channels;
	const char*  project_id;
	const char*  subproject_id;
	const char*  ep_prefix;
	const size_t ep_name_skip;

	sb(char)             ep_names;
	sb(char)             an_text;
	sb(struct anno_meta) an_meta;
} ep_guides[] = {
	{ "handmade_hero hero", "hero" , "code"      , "episode/code/day"      , 4 },
	{ "handmade_hero hero", "hero" , "misc"      , "episode/misc/"         , 0 },
	{ "handmade_hero hero", "hero" , "intro-to-c", "episode/intro-to-c/day", 10 },
	{ "handmade_hero hero", "hero" , "chat"      , "episode/chat/"         , 0 },
	{ "handmade_hero hero", "hero" , "ray"       , "episode/ray/"          , 0 },
	{ "miotatsu"          , "riscy", "riscy"     , "episode/riscy/"        , 0 },
	{ "miotatsu"          , "riscy", "book"      , "episode/book/"         , 0 },
};

static void hmninfo_update_projects(void){
	char* data = NULL;

	inso_curl_reset(curl, "https://handmade.network/sitemap", &data);
	if(inso_curl_perform(curl, &data) != 200){
		goto out;
	}

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

out:
	sb_free(data);
}

static size_t anno_unescape(char* str){
	for(char* c = str; *c; ++c){
		if(*c == '"' || *c == '\\'){
			memmove(c, c+1, strlen(c)+1);
			if(!*c) break;
		}
	}
	return strlen(str);
}

static void hmninfo_update_guides(void){
	sb(char) data = NULL;
	inso_curl_reset(curl, NULL, &data);
	char urlbuf[256];

	array_each(g, ep_guides){
		sb_free(data);
		snprintf(urlbuf, sizeof(urlbuf), "https://%s.handmade.network/%s.index", g->project_id, g->subproject_id);
		curl_easy_setopt(curl, CURLOPT_URL, urlbuf);

		long ret;
		if((ret = inso_curl_perform(curl, &data)) != 200){
			fprintf(stderr, "hmninfo: problem getting index [%s, %s]: %ld\n", g->project_id, g->subproject_id, ret);
			continue;
		}

		sb_free(g->ep_names);
		sb_free(g->an_text);
		sb_free(g->an_meta);

		char epname[64];
		enum { S_TOPLEVEL, S_MARKERS } state = S_TOPLEVEL;
		uint16_t ep_index = 0;
		char* _r;

		for(char* line = strtok_r(data, "\r\n", &_r); line; line = strtok_r(NULL, "\r\n", &_r)){

			if(state == S_MARKERS){
				int off = 0, timestamp;
				if(sscanf(line, "\"%d\": \"%n", &timestamp, &off) == 1 && off){
					struct anno_meta m = {
						.str_off = sb_count(g->an_text),
						.ep_off = ep_index,
						.time = timestamp,
					};

					size_t sz = anno_unescape(line + off);
					memcpy(sb_add(g->an_text, sz), line + off, sz);
					sb_push(g->an_text, '\n');
					sb_push(g->an_meta, m);
				} else {
					state = S_TOPLEVEL;
				}
			}

			if(state == S_TOPLEVEL){
				if(sscanf(line, "name: \"%63[^\"]", epname) == 1){
					ep_index = sb_count(g->ep_names);

					size_t sz = strlen(epname) + 1;
					assert(sz > g->ep_name_skip);
					memcpy(sb_add(g->ep_names, sz), epname + g->ep_name_skip, sz - g->ep_name_skip);

				} else if(strcmp(line, "markers:") == 0){
					state = S_MARKERS;
				}
			}
		}

		sb_push(g->an_text, 0);
	}

	sb_free(data);
}

static int anno_bs(const void* _a, const void* _b){
	int32_t off = *(int32_t*)_a;
	const struct anno_meta* meta = _b;
	return off - meta->str_off;
}

static void anno_search(struct ep_guide* guide, const char* chan, const char* name, const char* regex){
	regex_t rx;

	if(regcomp(&rx, regex, REG_ICASE | REG_EXTENDED | REG_NEWLINE) != 0){
		ctx->send_msg(chan, "@%s: 0 matches (bad regex)", name);
		return;
	}

	time_t start = time(0);

	int nmatches = 0;
	int prev_ep = -1;

	sb(int) ep_list = NULL;

	struct anno_meta* last_match = NULL;
	regmatch_t match = {};
	const char* str = guide->an_text;

	while(regexec(&rx, str, 1, &match, 0) == 0){
		const char* p = str + match.rm_so;
		while(p > guide->an_text && p[-1] != '\n') --p;

		time_t now = time(0);
		if(now - start > 5){
			ctx->send_msg(chan, "@%s: Sorry i'm either too slow for that regex or broken D:", name);
			goto out;
		}

		int32_t off = p - guide->an_text;
		struct anno_meta* m = bsearch(&off, guide->an_meta, sb_count(guide->an_meta), sizeof(*m), anno_bs);
		assert(m);

#if 0
		int sz = strcspn(p, "\n");
		printf("[https://%s.handmade.network/%s%s#%d] [%.*s]\n",
		       guide->project_id,
		       guide->ep_prefix,
		       guide->ep_names + m->ep_off,
		       m->time,
		       sz, p);
#endif
		nmatches++;
		last_match = m;

		str += INSO_MAX(1, match.rm_eo);
		str += strcspn(str, "\n");

		if(prev_ep != m->ep_off){
			sb_push(ep_list, m->ep_off);
			prev_ep = m->ep_off;
		}
	}

	char* regex_urlenc = curl_easy_escape(curl, regex, 0);

	if(nmatches == 0){
		ctx->send_msg(chan,
		              "@%s: No dice, but maybe I missed something?: https://%s.handmade.network/episode/%s#%s",
		              name, guide->project_id, guide->subproject_id, regex_urlenc);
	} else if(nmatches == 1){
		assert(last_match);
		const char* str = guide->an_text + last_match->str_off;
		int sz = strchrnul(str, '\n') - str;
		ctx->send_msg(chan,
		              "@%s: 1 hit: https://%s.handmade.network/%s%s#%d [%.*s]",
		              name, guide->project_id, guide->ep_prefix, guide->ep_names + last_match->ep_off, last_match->time, sz, str);
	} else {
		char ep_buf[32];
		char* p = ep_buf;
		const char* more_str = "";

		sb_each(ep, ep_list){
			const char* name = guide->ep_names + *ep;
			size_t len = strlen(name);

			if((p + len + 2) >= ep_buf + sizeof(ep_buf)){
				more_str = " and more...";
				break;
			} else {
				if(p != ep_buf){
					*p++ = ',';
					*p++ = ' ';
				}
				p = stpcpy(p, name);
			}
		}

		const char* ep_plural = sb_count(ep_list) == 1 ? "" : "s";
		ctx->send_msg(chan,
		              "@%s: ~%d hits: https://%s.handmade.network/episode/%s#%s [ep%s: %s%s]",
		              name, nmatches, guide->project_id, guide->subproject_id, regex_urlenc, ep_plural, ep_buf, more_str);
	}

	curl_free(regex_urlenc);

out:
	sb_free(ep_list);
	regfree(&rx);
}

static void hmninfo_update(void){
	hmninfo_update_projects();
	hmninfo_update_guides();
}

static bool hmninfo_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	curl = curl_easy_init();
	regcomp(&hmn_proj_regex, "https://([^\\.]+)\\.handmade\\.network/", REG_EXTENDED | REG_ICASE);
	hmninfo_update();
	return true;
}

static void hmninfo_quit(void){
	sb_each(p, projects){
		free(p->name);
		free(p->info);
		free(p->slug);
	}
	sb_free(projects);

	array_each(g, ep_guides){
		sb_free(g->ep_names);
		sb_free(g->an_text);
		sb_free(g->an_meta);
	}

	regfree(&hmn_proj_regex);
	curl_easy_cleanup(curl);
}

static void hmninfo_msg(const char* chan, const char* name, const char* msg){

	if(hmn_msg_suppress){
		hmn_msg_suppress = false;
		return;
	}

	sb_each(p, projects){
		const char* s = strcasestr(msg, p->slug);
		if(s && (s[p->slug_len] == ' ' || s[p->slug_len] == 0 || ispunct(s[p->slug_len]))){
			ctx->send_msg(chan, "↑ %s: %s", p->name, p->info);
			break;
		}
	}
}

static bool is_guide_chan(struct ep_guide* g, const char* chan){
	size_t chan_len = strlen(chan);
	const char* list = g->channels;

	for(;;){
		size_t len = strcspn(list, " ");

		if(len == chan_len && strncmp(list, chan, len) == 0){
			return true;
		}

		if(!list[len]){
			break;
		}

		list += len + 1;
	}

	return false;
}

static void hmninfo_cmd(const char* chan, const char* name, const char* arg, int cmd){

	if(cmd == HMN_UPDATE){
		sb_each(p, projects){
			free(p->name);
			free(p->info);
			free(p->slug);
		}
		sb_free(projects);

		hmninfo_update();

		// TODO: print out changes if any?
		ctx->send_msg(chan, "%s: Project list updated.", name);

	} else if(cmd == HMN_ANNO_SEARCH){

		const char* disp = inso_dispname(ctx, name);
		const char* project = NULL;
		char project_buf[128];
		bool fallback = true;
		int off = 0;

		if(sscanf(arg, " ~%127s %n", project_buf, &off) == 1 && off){
			fallback = false;
			project = project_buf;
			arg += (off-1);
			hmn_msg_suppress = true;
		}

		if(!*arg++){
			ctx->send_msg("@%s: Usage: %s", disp, irc_mod_ctx.cmd_help[cmd]);
			return;
		}

		struct ep_guide* guide = NULL;

		// first try subprojects, for ~code, ~book etc.
		array_each(g, ep_guides){
			if((project && strcmp(project, g->subproject_id) == 0) || (!project && is_guide_chan(g, chan + 1))){
				guide = g;
				break;
			}
		}

		// otherwise, try main projects like ~hero, and pick the first subproject if found.
		if(!guide && project){
			array_each(g, ep_guides){
				if(strcmp(project, g->project_id) == 0){
					guide = g;
					break;
				}
			}
		}

		if(!guide){
			if(fallback){
				guide = ep_guides;
			} else {
				ctx->send_msg(chan, "@%s: Unknown project.", disp);
				return;
			}
		}

		anno_search(guide, chan, disp, arg);
	}
}
