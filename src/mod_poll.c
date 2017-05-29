#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "inso_json.h"
#include "stb_sb.h"

static bool poll_init (const IRCCoreCtx*);
static void poll_cmd  (const char*, const char*, const char*, int);
static void poll_nick (const char*, const char*);
static bool poll_save (FILE*);
static bool poll_load (void);
static void poll_quit (void);

enum { POLL_OPEN, POLL_CLOSE, POLL_LIST, POLL_LIST_OPEN, POLL_LIST_CLOSED, POLL_VOTE };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "poll",
	.desc     = "Create polls that can be voted on.",
	.on_init  = &poll_init,
	.on_cmd   = &poll_cmd,
	.on_nick  = &poll_nick,
	.on_save  = &poll_save,
	.on_quit  = &poll_quit,
	.commands = DEFINE_CMDS (
		[POLL_OPEN]        = CMD("poll+"),
		[POLL_CLOSE]       = CMD("poll-"),
		[POLL_LIST]        = CMD("poll"),
		[POLL_LIST_OPEN]   = CMD("pall") CMD("popen"),
		[POLL_LIST_CLOSED] = CMD("phist"),
		[POLL_VOTE]        = CMD("vote")
	),
	.cmd_help = DEFINE_CMDS(
		[POLL_OPEN]        = "<question> '?' <opt1> '|' <opt2> '|' ... <optN> | Creates a new poll for <question> with options <opt1..N>",
		[POLL_CLOSE]       = "<ID> | Closes the poll numbered <ID> (which is displayed when created or listed",
		[POLL_LIST]        = "<ID> | Shows information about the poll identified by <ID>",
		[POLL_LIST_OPEN]   = "| List currently open polls",
		[POLL_LIST_CLOSED] = "| List closed polls",
		[POLL_VOTE]        = "[ID] <N> | Vote for option <N> on the most recently opened poll, or, if given, the poll identified by [ID]."
	),
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
	time_t   modified;
	bool     open;
} Poll;

static Poll* poll_list;
static int poll_next_id;

static bool poll_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return poll_load();
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
		if(strcmp(p->chan, chan) == 0 && p->open && (!candidate || p->creation > candidate->creation)){
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
			char* opt = strndupa(p, len);

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

			// handle escaping
			for(c = opt; *c; ++c){
				if(c[0] == '\\' && c[1] == '|'){
					memmove(c, c + 1, len - (c-opt));
					--len;
				}
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
	poll.modified = poll.creation;
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
	bool show_closed = false;

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

				ctx->send_msg(chan, "New poll (#%d): [%s] Use !vote N to vote for: %s", poll->id, poll->question, opt_buf);
				ctx->save_me();
			} else {
				ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "poll+ <question>? opt0 | opt1 | ... | optn", name);
			}
		} break;

		case POLL_CLOSE: {
			if(!wlist) break;
			Poll* poll;
			int id;

			if(sscanf(arg, " %d", &id) == 1 || sscanf(arg, " #%d", &id) == 1){
				poll = poll_get(id);
			} else {
				poll = poll_get_mru(chan);
			}

			if(poll && poll->open){
				poll->open = false;
				poll->modified = time(0);

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
				ctx->save_me();
			} else {
				ctx->send_msg(chan, "%s: Poll not found.", name);
			}
		} break;

		case POLL_VOTE: {
			Poll* poll;
			int id, vote;

			if(sscanf(arg, " %d %d", &id, &vote) == 2 || sscanf(arg, " #%d %d", &id, &vote) == 2){
				poll = poll_get(id);
			} else if(sscanf(arg, " %d", &vote) == 1){
				poll = poll_get_mru(chan);
			} else {
				ctx->send_msg(name, "Could not parse your vote command.");
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
					ctx->save_me();

					ctx->send_msg(name, "Your vote for [%s] was counted successfully.", poll->options[vote].text);
				} else {
					ctx->send_msg(name, "You've already voted on poll #%d.", poll->id);
				}
			} else if(!poll){
				ctx->send_msg(name, "Vote command failed: Poll not found.");
			} else if(!poll->open){
				ctx->send_msg(name, "Vote command failed: That poll is closed.");
			} else {
				ctx->send_msg(name, "Vote command failed: Vote number out of range.");
			}
		} break;

		case POLL_LIST: {
			if(!wlist) break;
			Poll* poll;
			int id;

			if(sscanf(arg, " %d", &id) == 1 || sscanf(arg, " #%d", &id) == 1){
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

				char time_buf[32];
				time_diff_string(poll->modified, time(0), time_buf, sizeof(time_buf));

				const char* o[] = { "closed", "opened" };
				ctx->send_msg(chan, "Poll #%d (%s %s): [%s] %s", poll->id, o[poll->open], time_buf, poll->question, opt_buf);
			} else {
				ctx->send_msg(chan, "%s: Poll not found.", name);
			}
		} break;

		case POLL_LIST_CLOSED: {
			show_closed = true;
		} // fall-through;

		case POLL_LIST_OPEN: {
			if(!wlist) break;

			char buf[256] = {};
			char* p = buf;
			size_t sz = sizeof(buf);

			sb_each(poll, poll_list){
				if(show_closed == poll->open) continue;
				snprintf_chain(&p, &sz, "[#%d: %s] ", poll->id, poll->question);
			}

			if(*buf){
				static const char* o[] = { "Open", "Closed" };
				ctx->send_msg(chan, "%s: %s polls: %s", name, o[show_closed], buf);
			} else {
				static const char* o[] = { "open", "closed" };
				ctx->send_msg(chan, "%s: No %s polls found.", name, o[show_closed]);
			}
		} break;
	}
}

static void poll_nick(const char* prev, const char* cur){
	sb_each(poll, poll_list){
		if(!poll->open) continue;

		sb_each(v, poll->voters){
			if(strcmp(*v, cur) == 0){
				return;
			}
		}

		sb_each(v, poll->voters){
			if(strcmp(*v, prev) == 0){
				sb_push(poll->voters, strdup(cur));
				break;
			}
		}
	}
}

static bool poll_load(void){
	bool success = false;

	FILE* f = fopen(ctx->get_datafile(), "r");

	fseek(f, 0, SEEK_END);
	size_t buf_sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	// succeed on empty config file
	if(buf_sz <= 1){
		fclose(f);
		return true;
	}

	char* buf = malloc(buf_sz + 1);
	fread(buf, buf_sz, 1, f);
	fclose(f);

	buf[buf_sz] = '\0';

	yajl_val root = yajl_tree_parse(buf, NULL, 0);
	if(!YAJL_IS_ARRAY(root)){
		goto end;
	}

	for(size_t i = 0; i < root->u.array.len; ++i){
		yajl_val v = root->u.array.values[i];
		if(!YAJL_IS_OBJECT(v)){
			goto end;
		}

		yajl_val id, chan, question, options, voters, creation, modified, open;

		if(!yajl_multi_get(
			v,
			"id",       yajl_t_number, &id,
			"chan",     yajl_t_string, &chan,
			"question", yajl_t_string, &question,
			"options" , yajl_t_array,  &options,
			"voters",   yajl_t_array,  &voters,
			"creation", yajl_t_number, &creation,
			"modified", yajl_t_number, &modified,
			"open",     yajl_t_any,    &open,
			NULL)){
			goto end;
		}

		if (!YAJL_IS_INTEGER(id) ||
			!YAJL_IS_INTEGER(creation) ||
			!YAJL_IS_INTEGER(modified) ||
			!(YAJL_IS_TRUE(open) || YAJL_IS_FALSE(open))){
			goto end;
		}

		Poll poll = {
			.id       = id->u.number.i,
			.chan     = strdup(chan->u.string),
			.question = strdup(question->u.string),
			.creation = creation->u.number.i,
			.modified = modified->u.number.i,
			.open     = YAJL_IS_TRUE(open),
		};

		PollOpt opt;
		for(size_t j = 0; j < options->u.array.len; ++j){
			yajl_val v = options->u.array.values[j];

			if((j % 2) == 0){
				if(!YAJL_IS_STRING(v)) goto end;
				opt.text = strdup(v->u.string);
			} else {
				if(!YAJL_IS_INTEGER(v)) goto end;
				opt.votes = v->u.number.i;
				sb_push(poll.options, opt);
			}
		}

		for(size_t j = 0; j < voters->u.array.len; ++j){
			yajl_val v = voters->u.array.values[j];
			if(!YAJL_IS_STRING(v)) goto end;
			sb_push(poll.voters, strdup(v->u.string));
		}

		sb_push(poll_list, poll);
		poll_next_id = INSO_MAX(poll_next_id, poll.id + 1);
	}

	success = true;
end:
	yajl_tree_free(root);
	free(buf);

	return success;
}

static bool poll_save(FILE* file){
	yajl_gen json = yajl_gen_alloc(NULL);
	yajl_gen_config(json, yajl_gen_beautify, 1);

	yajl_gen_array_open(json);
	for(Poll* p = poll_list; p < sb_end(poll_list); ++p){
		yajl_gen_map_open(json);

		yajl_gen_string(json, "id", 2);
		yajl_gen_integer(json, p->id);

		yajl_gen_string(json, "chan", 4);
		yajl_gen_string(json, p->chan, strlen(p->chan));

		yajl_gen_string(json, "question", 8);
		yajl_gen_string(json, p->question, strlen(p->question));

		yajl_gen_string(json, "options", 7);
		yajl_gen_array_open(json);
		for(size_t i = 0; i < sb_count(p->options); ++i){
			yajl_gen_string(json, p->options[i].text, strlen(p->options[i].text));
			yajl_gen_integer(json, p->options[i].votes);
		}
		yajl_gen_array_close(json);

		yajl_gen_string(json, "voters", 6);
		yajl_gen_array_open(json);
		for(size_t i = 0; i < sb_count(p->voters); ++i){
			yajl_gen_string(json, p->voters[i], strlen(p->voters[i]));
		}
		yajl_gen_array_close(json);

		yajl_gen_string(json, "creation", 8);
		yajl_gen_integer(json, p->creation);

		yajl_gen_string(json, "modified", 8);
		yajl_gen_integer(json, p->modified);

		yajl_gen_string(json, "open", 4);
		yajl_gen_bool(json, p->open);

		yajl_gen_map_close(json);
	}

	yajl_gen_array_close(json);

	size_t len;
	const uint8_t* buf;
	yajl_gen_get_buf(json, &buf, &len);
	fwrite(buf, len, 1, file);

	yajl_gen_free(json);

	return true;
}

static void poll_quit(void){
	sb_each(p, poll_list){
		free(p->chan);
		free(p->question);

		sb_each(o, p->options){
			free(o->text);
		}
		sb_free(p->options);

		sb_each(v, p->voters){
			free(*v);
		}
		sb_free(p->voters);
	}
}
