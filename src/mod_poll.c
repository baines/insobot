#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "stb_sb.h"
#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>

static bool poll_init (const IRCCoreCtx*);
static void poll_cmd  (const char*, const char*, const char*, int);
static bool poll_save (FILE*);
static void poll_quit (void);

enum { POLL_OPEN, POLL_CLOSE, POLL_LIST, POLL_VOTE };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "poll",
	.desc     = "Create polls that can be voted on.",
	.on_init  = &poll_init,
	.on_cmd   = &poll_cmd,
	.on_save  = &poll_save,
	.on_quit  = &poll_quit,
	.commands = DEFINE_CMDS (
		[POLL_OPEN]  = CMD("poll+"),
		[POLL_CLOSE] = CMD("poll-"),
		[POLL_LIST]  = CMD("poll"),
		[POLL_VOTE]  = CMD("vote")
	)
};

static const IRCCoreCtx* ctx;

typedef struct {
	char* text;
	int   votes;
} PollOpt;

typedef struct {
	int      id;
	char*    chan;
	char*    question;
	PollOpt* options;
	char**   voters;
	time_t   creation;
	bool     open;
} Poll;

static Poll* poll_list;
static int poll_next_id;

static bool poll_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static Poll* poll_get(int id){
	for(Poll* p = poll_list; p < sb_end(poll_list); ++p){
		if(p->id == id) return p;
	}
	return NULL;
}

static Poll* poll_get_mru(const char* chan){
	Poll* candidate = NULL;

	for(Poll* p = poll_list; p < sb_end(poll_list); ++p){
		if(strcmp(p->chan, chan) == 0 && (!candidate || p->creation > candidate->creation)){
			candidate = p;
		}
	}

	return candidate;
}

static Poll* poll_create(const char* chan, const char* arg){
	const char* p;
	Poll poll = {};

	if((p = strchr(arg, '?'))){
		poll.question = strndupa(arg, (p+1) - arg);
	} else {
		return NULL;
	}

	if(p[1] != ' ') return NULL;
	p += 2;

	const char* q = p;
	do {
		if(!q[0] || (q[0] == '|' && q[-1] != '\\')){
			ptrdiff_t len = q - p;
			char*  opt = strndupa(p, len);

			// trim leading spaces
			char* c = opt;
			while(c < opt + len && *c == ' '){
				++c;
			}
			len -= (c - opt);
			opt = c;

			// trim trailing spaces
			c = opt + len - 1;
			while(c >= opt && *c == ' '){
				*c-- = '\0';
			}

			if(*opt){
				PollOpt po = { .text = strdup(opt) };
				sb_push(poll.options, po);
			}

			p = q + 1;
		}
	} while(*q++);

	if(sb_count(poll.options) <= 1){
		sb_free(poll.options);
		return NULL;
	}

	poll.id = poll_next_id++;
	poll.chan = strdup(chan);
	poll.question = strdup(poll.question);
	poll.creation = time(0);
	poll.open = true;

	sb_push(poll_list, poll);

	return &sb_last(poll_list);
}

static int poll_opt_sort(const void* _a, const void* _b){
	const PollOpt *a = _a, *b = _b;
	return b->votes - a->votes;
}

static void poll_cmd(const char* chan, const char* name, const char* arg, int cmd){
	bool wlist = inso_is_wlist(ctx, name);

	switch(cmd){

		case POLL_OPEN: {
			if(!wlist || !*arg) break;

			Poll* poll = poll_create(chan, arg+1);
			if(poll){
				char opt_buf[256] = {};
				char* p = opt_buf;
				size_t sz = sizeof(opt_buf);

				for(size_t i = 0; i < sb_count(poll->options); ++i){
					snprintf_chain(&p, &sz, "[%d: %s] ", i, poll->options[i].text);
				}

				ctx->send_msg(chan, "New poll: [%s] Use !vote N to vote for: %s", poll->question, opt_buf);
			} else {
				ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "poll+ <question>? opt0 | opt1 | ... | optn", name);
			}
		} break;

		case POLL_CLOSE: {
			if(!wlist) break;
			Poll* poll;
			int id;

			if(sscanf(arg, " #%d", &id) == 1){
				poll = poll_get(id);
			} else {
				poll = poll_get_mru(chan);
			}

			if(poll && poll->open){
				poll->open = false;

				PollOpt* ranking = alloca(sb_count(poll->options) * sizeof(*ranking));
				memcpy(ranking, poll->options, sb_count(poll->options) * sizeof(*ranking));

				qsort(ranking, sb_count(poll->options), sizeof(PollOpt), &poll_opt_sort);

				char opt_buf[256] = {};
				char* p = opt_buf;
				size_t sz = sizeof(opt_buf);

				for(size_t i = 0; i < sb_count(poll->options); ++i){
					snprintf_chain(&p, &sz, "[#%d = %s (%d)] ", i+1, ranking[i].text, ranking[i].votes);
				}
				ctx->send_msg(chan, "Poll [%s] closed. Results: %s", poll->question, opt_buf);
			} else {
				ctx->send_msg(chan, "%s: Poll not found.", name);
			}
		} break;

		case POLL_VOTE: {
			Poll* poll;
			int id, vote, dummy;

			if(sscanf(arg, " #%d %d", &id, &vote) == 2){
				poll = poll_get(id);
			} else if(sscanf(arg, " %d %d", &vote, &dummy) == 1){
				poll = poll_get_mru(chan);
			} else {
				break;
			}

			if(poll && poll->open && vote >= 0 && vote < sb_count(poll->options)){
				bool can_vote = true;

				for(char** voter = poll->voters; voter < sb_end(poll->voters); ++voter){
					if(strcmp(name, *voter) == 0){
						can_vote = false;
						break;
					}
				}

				if(can_vote){
					poll->options[vote].votes++;
					sb_push(poll->voters, strdup(name));
				}
			}
		} break;

		case POLL_LIST: {
			if(!wlist) break;
			Poll* poll;
			int id;

			if(sscanf(arg, " #%d", &id) == 1){
				poll = poll_get(id);
			} else {
				poll = poll_get_mru(chan);
			}

			if(poll){
				char opt_buf[256] = {};
				char* p = opt_buf;
				size_t sz = sizeof(opt_buf);

				for(size_t i = 0; i < sb_count(poll->options); ++i){
					snprintf_chain(&p, &sz, "[%d: %s (%d)] ", i, poll->options[i].text, poll->options[i].votes);
				}

				const char* o[] = { "closed", "open" };
				ctx->send_msg(chan, "Poll #%d (%s): [%s] %s", poll->id, o[poll->open], poll->question, opt_buf);
			} else {
				ctx->send_msg(chan, "%s: Poll not found.", name);
			}
		} break;
	}
}

static bool poll_save(FILE* file){
	return false;
}

static void poll_quit(void){
	for(Poll* p = poll_list; p < sb_end(poll_list); ++p){
		free(p->chan);
		free(p->question);

		for(PollOpt* o = p->options; o < sb_end(p->options); ++o){
			free(o->text);
		}
		sb_free(p->options);

		for(char** c = p->voters; c < sb_end(p->voters); ++c){
			free(*c);
		}
		sb_free(p->voters);
	}
}
