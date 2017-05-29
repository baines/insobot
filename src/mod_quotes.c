#include <time.h>
#include <string.h>
#include <ctype.h>
#include "module.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include "inso_gist.h"

static bool quotes_init     (const IRCCoreCtx*);
static void quotes_modified (void);
static void quotes_cmd      (const char*, const char*, const char*, int);
static void quotes_quit     (void);
static void quotes_ipc      (int, const uint8_t*, size_t);

enum { GET_QUOTE, ADD_QUOTE, DEL_QUOTE, FIX_QUOTE, FIX_TIME, LIST_QUOTES, SEARCH_QUOTES, GET_RANDOM };

const IRCModuleCtx irc_mod_ctx = {
	.name        = "quotes",
	.desc        = "Saves per-channel quotes",
	.on_init     = &quotes_init,
	.on_modified = &quotes_modified,
	.on_cmd      = &quotes_cmd,
	.on_quit     = &quotes_quit,
	.on_ipc      = &quotes_ipc,
	.commands    = DEFINE_CMDS (
		[GET_QUOTE]     = CMD1("q"   ) CMD1("quote"   ),
		[ADD_QUOTE]     = CMD1("qadd") CMD1("q+"      ),
		[DEL_QUOTE]     = CMD1("qdel") CMD1("q-"      ) CMD1("qrm"),
		[FIX_QUOTE]     = CMD1("qfix") CMD1("qmv"     ),
		[FIX_TIME]      = CMD1("qft" ) CMD1("qfixtime"),
		[LIST_QUOTES]   = CMD1("ql"  ) CMD1("qlist"   ),
		[SEARCH_QUOTES] = CMD1("qs"  ) CMD1("qsearch" ) CMD1("qfind"  ) CMD1("qgrep"),
		[GET_RANDOM]    = CMD1("qr"  ) CMD1("qrand"   ) CMD1("qrandom")
	),
	.cmd_help = DEFINE_CMDS (
		[GET_QUOTE]     = "[#chan] [ID] | Shows the quote with given ID, or a random one.",
		[ADD_QUOTE]     = "[#chan] <text> | Adds a new quote.",
		[DEL_QUOTE]     = "[#chan] <ID> | Removes the quote with given ID.",
		[FIX_QUOTE]     = "[#chan] <ID> <text> | Modifies the text of the quote with the given ID.",
		[FIX_TIME]      = "[#chan] <ID> <YYYY-MM-DD> | Modifies the associated timestamp of the quote with the given ID.",
		[LIST_QUOTES]   = "| Shows the URL to the full list of quotes",
		[SEARCH_QUOTES] = "[#chan] <query> | Searches the quotes for those that match <query>",
		[GET_RANDOM]    = "[#chan] | Displays a random quote"
	)
};

static const IRCCoreCtx* ctx;

static inso_gist* gist;
static char* gist_pub_url;

typedef struct Quote_ {
	uint32_t id;
	time_t timestamp;
	char* text;
} Quote;

static char** channels;
static Quote** chan_quotes;

static char* gen_escaped_csv(Quote* quotes){
	char* csv = NULL;

	char id_buff[16];
	char tm_buff[256];

	for(Quote* q = quotes; q < sb_end(quotes); ++q){

		size_t id_len = snprintf(id_buff, sizeof(id_buff), "%d", q->id);
		size_t tm_len = strftime(tm_buff, sizeof(tm_buff), "%c", gmtime(&q->timestamp));

		memcpy(sb_add(csv, id_len), id_buff, id_len);
		sb_push(csv, ',');

		sb_push(csv, '"');
		memcpy(sb_add(csv, tm_len), tm_buff, tm_len);
		sb_push(csv, '"');
		sb_push(csv, ',');

		sb_push(csv, '"');
		
		for(const char* t = q->text; *t; ++t){
			if(*t == '"'){
				sb_push(csv, '"');
			}
			sb_push(csv, *t);
		}
		
		sb_push(csv, '"');
		sb_push(csv, '\n');
	}
	sb_push(csv, 0);

	return csv;
}

static void load_csv(const char* content, Quote** qlist){
	char* buffer = strdup(content);

	char* line_state = NULL;
	char* line = strtok_r(buffer, "\r\n", &line_state);

	for(; line; line = strtok_r(NULL, "\r\n", &line_state)){
		Quote q = {};
		char* time_start = NULL;

		q.id = strtol(line, &time_start, 10);

		if(!time_start || *time_start != ',' || time_start[1] != '"'){
			printf("csv parse err 0: %s\n", time_start);
			continue;
		}

		time_start += 2;

		struct tm timestamp = {};
		char* text_start = strptime(time_start, "%c", &timestamp);
		if(!text_start || *text_start != '"' || text_start[1] != ',' || text_start[2]!= '"'){
			printf("csv parse err 1: %s\n", text_start);
			continue;
		}

		text_start+=3;

		q.timestamp = timegm(&timestamp);

		size_t len = strlen(text_start);
		if(text_start[len - 1] != '"'){
			printf("csv parse err 2: %s", text_start);
			continue;
		}
		text_start[len - 1] = 0;

		char* text_buff = alloca(len - 1);
		char* o = text_buff;

		for(char* i = text_start; *i; ++i){
			if(*i == '"'){
				if(*(i+1) == '"'){
					++i;
				} else {
					puts("This csv file is pretty broken...");
				}
			}
			*o++ = *i;
		}

		q.text = strndup(text_buff, o - text_buff);

		sb_push(*qlist, q);
	}

	free(buffer);
}

static void quotes_free(void){
	for(size_t i = 0; i < sb_count(channels); ++i){
		free(channels[i]);
		for(size_t j = 0; j < sb_count(chan_quotes[i]); ++j){
			free(chan_quotes[i][j].text);
		}
		sb_free(chan_quotes[i]);
	}
	sb_free(channels);
	sb_free(chan_quotes);
}

static void quotes_quit(void){
	quotes_free();
	inso_gist_close(gist);
	free(gist_pub_url);
}

static void quotes_upload(int modified_index){
	inso_gist_file* file = NULL;
	inso_gist_file_add(&file, " Quote List", "Here are the quotes stored by insobot, in csv format, one file per channel. Times are UTC.");

	if(modified_index == -1){ // full upload
		for(size_t i = 0; i < sb_count(channels); ++i){
			if(sb_count(chan_quotes[i]) == 0){
				inso_gist_file_add(&file, channels[i], NULL);
			} else {
				char* csv = gen_escaped_csv(chan_quotes[i]);
				inso_gist_file_add(&file, channels[i], csv);
				sb_free(csv);
			}
		}
	} else { // single file upload
		if(sb_count(chan_quotes[modified_index]) == 0){
			inso_gist_file_add(&file, channels[modified_index], NULL);
		} else {
			char* csv = gen_escaped_csv(chan_quotes[modified_index]);
			inso_gist_file_add(&file, channels[modified_index], csv);
			sb_free(csv);
		}
	}

	inso_gist_save(gist, "IRC quotes", file);
	inso_gist_file_free(file);
}

static bool quotes_reload(void){
	inso_gist_file* files = NULL;
	int ret = inso_gist_load(gist, &files);

	if(ret == INSO_GIST_304){
		puts("mod_quotes: not modified.");
		return true;
	}

	if(ret != INSO_GIST_OK){
		puts("mod_quotes: gist error.");
		return false;
	}

	puts("mod_quotes: doing full reload");

	quotes_free();

	for(inso_gist_file* f = files; f; f = f->next){
		if(!f->name || f->name[0] != '#') continue;

		sb_push(channels, strdup(f->name));
		sb_push(chan_quotes, 0);
		load_csv(f->content, &sb_last(chan_quotes));
	}

	inso_gist_file_free(files);

	return true;
}

static bool quotes_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	char* gist_id = getenv("INSOBOT_GIST_ID");
	if(!gist_id || !*gist_id){
		fputs("mod_quotes: No INSOBOT_GIST_ID env, can't continue.\n", stderr);
		return false;
	}

	char* gist_user = getenv("INSOBOT_GIST_USER");
	if(!gist_user || !*gist_user){
		fputs("mod_quotes: No INSOBOT_GIST_USER env, can't continue.\n", stderr);
		return false;
	}
	
	char* gist_token = getenv("INSOBOT_GIST_TOKEN");
	if(!gist_token || !*gist_token){
		fputs("mod_quotes: No INSOBOT_GIST_TOKEN env, can't continue.\n", stderr);
		return false;
	}

	asprintf_check(&gist_pub_url, "https://gist.github.com/%s", gist_id);

	gist = inso_gist_open(gist_id, gist_user, gist_token);

	return quotes_reload();
}

static void quotes_modified(void){
	printf("RELOAD: %d\n", quotes_reload());
}

static Quote* quote_get(const char* chan, unsigned int id){
	int index = -1;
	for(size_t i = 0; i < sb_count(channels); ++i){
		if(strcmp(chan, channels[i]) == 0){
			index = i;
			break;
		}
	}

	if(index < 0) return NULL;

	for(Quote* q = chan_quotes[index]; q < sb_end(chan_quotes[index]); ++q){
		if(q->id == id){
			return q;
		}
	}

	return NULL;
}

static const char* quotes_get_chan(const char* default_chan, const char** arg, Quote*** qlist, bool* same){

	const char* chan = getenv("INSOBOT_QUOTES_DEFAULT_CHAN");
	if(!chan) chan = default_chan;
	if(same) *same = true;

	// if the arg starts with a #, parse the channel out of it
	if(**arg == '#'){
		const char* end = strchrnul(*arg, ' ');

		char* new_chan = strndupa(*arg, end - *arg);
		for(char* c = new_chan; *c; ++c){
			*c = tolower(*c);
		}

		*arg = *end ? end + 1 : end;

		if(same){
			*same = strcasecmp(chan, new_chan) == 0;
		}

		chan = new_chan;
	}

	bool found = false;
	for(size_t i = 0; i < sb_count(channels); ++i){
		if(strcmp(channels[i], chan) == 0){
			chan = channels[i];
			if(qlist) *qlist = chan_quotes + i;
			found = true;
		}
	}

	if(!found){
		sb_push(channels, strdup(chan));
		sb_push(chan_quotes, 0);
		chan = sb_last(channels);
		if(qlist) *qlist = &sb_last(chan_quotes);
	}

	return chan;
}

static char quote_date_buf[64];
static const char* quote_strtime(Quote* q){
	struct tm* date_tm = gmtime(&q->timestamp);
	strftime(quote_date_buf, sizeof(quote_date_buf), "%F", date_tm);
	return quote_date_buf;
}

static void quotes_notify(const char* chan, const char* name, Quote* q){

	bool known_channel = false;
	for(const char** c = ctx->get_channels(); *c; ++c){
		if(strcasecmp(*c, chan) == 0){
			known_channel = true;
			break;
		}
	}

	if(known_channel && q){
		ctx->send_msg(chan, "%s added quote %d: \"%s\".", name, q->id, q->text);
	}
}

static void quotes_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool is_wlist = inso_is_wlist(ctx, name);
	bool has_cmd_perms = strcasecmp(chan+1, name) == 0 || is_wlist;

	bool empty_arg = !*arg;
	if(!empty_arg) ++arg;

	inso_gist_lock(gist);
	quotes_reload();

	bool same_chan;
	Quote** quotes;

	const char* quote_chan = quotes_get_chan(chan, &arg, &quotes, &same_chan);

	name = inso_dispname(ctx, name);

	switch(cmd){
		case GET_QUOTE: {
			if(!empty_arg){
				char* end;
				int id = strtol(arg, &end, 0);
				if(end == arg || id < 0){
					ctx->send_msg(chan, "%s: Quotes start at id 0.", name);
					break;
				}

				Quote* q = quote_get(quote_chan, id);
				if(q){
					ctx->send_msg(chan, "Quote %d: \"%s\" ―%s %s", id, q->text, quote_chan+1, quote_strtime(q));
				} else {
					ctx->send_msg(chan, "%s: Can't find that quote.", name);
				}
				break;
			}
		} // fall-through

		case GET_RANDOM: {
			if(!sb_count(*quotes)){
				ctx->send_msg(chan, "%s: No quotes found.", name);
				break;
			}

			Quote* q = *quotes + (rand() % sb_count(*quotes));
			ctx->send_msg(chan, "Quote %d: \"%s\" ―%s %s", q->id, q->text, quote_chan+1, quote_strtime(q));
		} break;

		case ADD_QUOTE: {
			if(!has_cmd_perms) break;

			if(empty_arg){
				ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "qadd <text>", name);
				break;
			}

			if(!same_chan && !is_wlist){
				break;
			}

			int id = 0;
			if(sb_count(*quotes) > 0){
				id = sb_last(*quotes).id + 1;
			}
			Quote q = { .id = id, .text = strdup(arg), .timestamp = time(0) };
			sb_push(*quotes, q);
			ctx->send_msg(chan, "%s: Added as quote %d.", name, id);

			// if adding to another channel, send a message to that channel.
			if(!same_chan){
				quotes_notify(quote_chan, name, &q);
			}

			quotes_upload(quotes - chan_quotes);

			// notify other instances so they can also send messages to the affected channel
			char ipc_buf[256];
			int ipc_len = snprintf(ipc_buf, sizeof(ipc_buf), "ADD %d %s %s", id, quote_chan, name);
			ctx->send_ipc(0, ipc_buf, ipc_len + 1);
		} break;

		case DEL_QUOTE: {
			if(!has_cmd_perms) break;

			if(empty_arg){
				ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "qdel <id>", name);
				break;
			}

			if(!same_chan && !is_wlist){
				break;
			}

			char* end;
			int id = strtol(arg, &end, 0);
			if(!end || end == arg || *end || id < 0){
				ctx->send_msg(chan, "%s: That id doesn't look valid.", name);
				break;
			}

			Quote* q = quote_get(quote_chan, id);
			if(q){
				free(q->text);
				int off = q - *quotes;
				sb_erase(*quotes, off);
				ctx->send_msg(chan, "%s: Deleted quote %d\n", name, id);
				quotes_upload(quotes - chan_quotes);
			} else {
				ctx->send_msg(chan, "%s: Can't find that quote.", name);
			}
		} break;

		case FIX_QUOTE: {
			if(!has_cmd_perms) break;

			if(empty_arg){
				ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "qfix <id> <new_text>", name);
				break;
			}

			if(!same_chan && !is_wlist){
				break;
			}

			char* arg2;
			int id = strtol(arg, &arg2, 0);
			if(!arg2 || arg2 == arg || id < 0){
				ctx->send_msg(chan, "%s: That id doesn't look valid.", name);
				break;
			}
			if(arg2[0] != ' ' || !arg2[1]){
				ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "qfix <id> <new_text>", name);
				break;
			}

			Quote* q = quote_get(quote_chan, id);
			if(q){
				free(q->text);
				q->text = strdup(arg2 + 1);
				ctx->send_msg(chan, "%s: Updated quote %d.", name, id);
				quotes_upload(quotes - chan_quotes);
			} else {
				ctx->send_msg(chan, "%s: Can't find that quote.", name);
			}
		} break;

		case FIX_TIME: {
			if(!has_cmd_perms) break;

			if(empty_arg){
				ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "qft <id> <YYYY-MM-DD hh:mm:ss>", name);
				break;
			}

			if(!same_chan && !is_wlist){
				break;
			}

			char* arg2;
			int id = strtol(arg, &arg2, 0);
			if(!arg2 || arg2 == arg || id < 0){
				ctx->send_msg(chan, "%s: That id doesn't look valid.", name);
				break;
			}
			if(arg2[0] != ' ' || !arg2[1]){
				ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "qft <id> <YYYY-MM-DD hh:mm:ss>", name);
				break;
			}

			Quote* q = quote_get(quote_chan, id);
			if(!q){
				ctx->send_msg(chan, "%s: Can't find that quote.", name);
				break;
			}

			struct tm timestamp = {};
			char* ret = strptime(arg2 + 1, "%F %T", &timestamp);
			if(ret){
				q->timestamp = timegm(&timestamp);
				ctx->send_msg(chan, "%s: Updated quote %d's timestamp successfully.", name, id);
				quotes_upload(quotes - chan_quotes);
			} else {
				ctx->send_msg(chan, "%s: Sorry, I don't understand that timestamp. Use YYYY-MM-DD hh:mm:ss", name);
			}

		} break;

		case LIST_QUOTES: {
			ctx->send_msg(chan, "%s: You can find a list of quotes at %s", name, gist_pub_url);
		} break;

		case SEARCH_QUOTES: {
			if(empty_arg){
				ctx->send_msg(chan, "%s: Give me something to search for!", name);
				break;
			}

			bool sensible_search = false;
			for(const char* a = arg; *a; ++a){
				if(isalnum(*a)){
					sensible_search = true;
					break;
				}
			}
			if(!sensible_search){
				ctx->send_msg(chan, "%s: Give me something sensible to search for!", name);
				break;
			}

			if(sb_count(*quotes) == 0){
				ctx->send_msg(chan, "%s: There aren't any quotes to search.", name);
				break;
			}

			int found_count = 0;
			Quote* last_found_q = NULL;
			bool more_flag = false;

			char msg_buf[128];
			char* buf_ptr = msg_buf;
			ssize_t buf_len = sizeof(msg_buf);

			for(Quote* q = *quotes; q < sb_end(*quotes); ++q){
				if(strcasestr(q->text, arg) != NULL){
					++found_count;
					last_found_q = q;

					int ret = snprintf(buf_ptr, buf_len, "%d, ", q->id);
					if(ret < buf_len){
						buf_ptr += ret;
						buf_len -= ret;
					} else {
						*buf_ptr = 0;
						more_flag = true;
						break;
					}
				}
			}

			if(found_count == 1){
				Quote* q = last_found_q;
				ctx->send_msg(chan, "Quote %d: \"%s\" ―%s %s", q->id, q->text, quote_chan+1, quote_strtime(q));
			} else if(found_count > 1){
				if(more_flag){
					ctx->send_msg(chan, "%s: Matching quotes: %s and more.", name, msg_buf);
				} else {
					buf_ptr[-2] = '.';
					buf_ptr[-1] = 0;
					ctx->send_msg(chan, "%s: Matching quotes: %s", name, msg_buf);
				}
			} else {
				ctx->send_msg(chan, "%s: No matches for '%s'.", name, arg);
			}

		} break;
	}

	inso_gist_unlock(gist);
}

static void quotes_ipc(int sender, const uint8_t* data, size_t data_len){
	if(!memchr(data, 0, data_len)) return;

	int id;
	char* chan = NULL;
	int name_offset = 0;

	if(sscanf(data, "ADD %d %ms %n", &id, &chan, &name_offset) == 2){
		quotes_reload();
		quotes_notify(chan, data + name_offset, quote_get(chan, id));
	}

	free(chan);
}
