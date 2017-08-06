#include <time.h>
#include <string.h>
#include <ctype.h>
#include "module.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include <curl/curl.h>

static bool quotes_init     (const IRCCoreCtx*);
static void quotes_cmd      (const char*, const char*, const char*, int);
static void quotes_quit     (void);
static void quotes_ipc      (int, const uint8_t*, size_t);

enum { GET_QUOTE, ADD_QUOTE, DEL_QUOTE, FIX_QUOTE, FIX_TIME, LIST_QUOTES, SEARCH_QUOTES, GET_RANDOM };

const IRCModuleCtx irc_mod_ctx = {
	.name        = "quotes",
	.desc        = "Saves per-channel quotes",
	.on_init     = &quotes_init,
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

// XXX: change me to a CGIQuotes instance
#define QUOTES_URL "http://127.0.0.1/quotes"

static const IRCCoreCtx* ctx;

typedef struct Chan {
	char* name;
	time_t last_mod;
} QChan;

typedef struct Quote {
	uint32_t id;
	time_t timestamp;
	char* text;
} Quote;

static QChan*  channels;
static Quote** chan_quotes;
static CURL* curl;
static const char* quotes_auth;

static bool quote_parse(const char* line, Quote* out){
	size_t epoch;

	if(sscanf(line, "%u,%zu,%m[^\n]", &out->id, &epoch, &out->text) == 3){
		out->timestamp = epoch;
		return true;
	}

	return false;
}

static Quote* quote_add(const char* line, Quote** qlist){
	Quote q = {};
	if(quote_parse(line, &q)){
		sb_push(*qlist, q);
		return &sb_last(*qlist);
	}
	return NULL;
}

static bool quotes_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	quotes_auth = getenv("INSOBOT_QUOTES_AUTH");
	if(!quotes_auth){
		puts("mod_quotes: INSOBOT_QUOTES_AUTH not set, exiting");
		return false;
	}

	time_t now = time(0);
	char* data = NULL;
	curl = inso_curl_init(QUOTES_URL "/.raw", &data);

	long err;
	if((err = inso_curl_perform(curl, &data)) != 200){
		printf("mod_quotes: initial curl error: %ld, exiting\n", err);
	}

	int idx = -1;
	char* state;

	for(char* line = strtok_r(data, "\n", &state); line; line = strtok_r(NULL, "\n", &state)){
		if(*line == '#'){
			QChan chan = {
				.name = strdup(line),
				.last_mod = now,
			};
			sb_push(channels, chan);
			sb_push(chan_quotes, 0);
			idx = sb_count(channels) - 1;
		} else {
			if(idx == -1){
				printf("mod_quotes: initial parse error\n");
				return false;
			}

			quote_add(line, chan_quotes + idx);
		}
	}

	return true;
}
static void quotes_free(void){
	sb_each(c, channels){
		free(c->name);

		sb_each(q, chan_quotes[c - channels]){
			free(q->text);
		}
		sb_free(chan_quotes[c - channels]);
	}
	sb_free(channels);
	sb_free(chan_quotes);
}

static void quotes_quit(void){
	quotes_free();
}

static QChan* quotes_get_chan(const char* default_chan, const char** arg, bool* same){

	const char* chan = getenv("INSOBOT_QUOTES_DEFAULT_CHAN");
	if(!chan) chan = default_chan;
	if(same) *same = true;

#ifdef HACKS
    if(default_chan && strcmp(default_chan, "#hero") == 0){
        chan = "#handmade_hero";
    }
#endif

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

	// can only happen if the default_chan passed in is NULL
	if(!chan) return NULL;

	QChan* result = NULL;
	sb_each(c, channels){
		if(strcmp(c->name, chan) == 0){
			result = c;
			break;
		}
	}

	if(!result){
		QChan qc = { .name = strdup(chan) };
		sb_push(channels, qc);
		sb_push(chan_quotes, 0);
		result = &sb_last(channels);
	}

	return result;
}

static Quote* quote_www_get(QChan* chan, uint32_t id){

	char url[512];
	snprintf(url, sizeof(url), QUOTES_URL "/%s/%d.raw", chan->name+1, id);

	char* data = NULL;
	inso_curl_reset(curl, url, &data);
	curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
	curl_easy_setopt(curl, CURLOPT_TIMEVALUE    , chan->last_mod);

	Quote* quote = NULL;
	sb_each(q, chan_quotes[chan - channels]){
		if(q->id == id){
			quote = q;
			break;
		}
	}

	long ret = inso_curl_perform(curl, &data);
	if(ret == 200){
		chan->last_mod = time(0);

		if(quote){
			quote_parse(data, quote);
		} else {
			quote = quote_add(data, chan_quotes + (chan - channels));
		}
	} else if(ret == 404 && quote){ //delete quote if exists
		free(quote->text);
		Quote** base = chan_quotes + (chan - channels);
		sb_erase(*base, quote - *base);
		quote = NULL;
	}

	return quote;
}


static Quote* quote_www_post(QChan* chan, const char* text){
	size_t len = strlen(text);
	if(len == 0) return NULL;

	// remove redundant quotation marks.
	if(len >= 2 && *text == '"' && text[len-1] == '"' && !memchr(text+1, '"', len-2)){
		++text;
		len -= 2;
	}

	char* data = NULL;
	char* url  = NULL;
	asprintf_check(&url, QUOTES_URL "/%s", chan->name+1);
	inso_curl_reset(curl, url, &data);
	curl_easy_setopt(curl, CURLOPT_USERPWD, quotes_auth);
	free(url);

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS   , text);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, len);

	Quote* quote = NULL;
	if(inso_curl_perform(curl, &data) == 200){
		Quote q = {
			.text = strndup(text, len),
		};

		size_t epoch;
		if(sscanf(data, "%d,%zu", &q.id, &epoch) == 2){
			q.timestamp = epoch;
			sb_push(chan_quotes[chan - channels], q);
			quote =  &sb_last(chan_quotes[chan - channels]);
		} else {
			free(q.text);
		}
	}

	sb_free(data);
	return quote;
}

static Quote* quote_www_modify(QChan* chan, uint32_t id, const char* new_txt, ssize_t new_time){
	char* data = NULL;
	char* url  = NULL;
	asprintf_check(&url, QUOTES_URL "/%s/%u", chan->name+1, id);
	inso_curl_reset(curl, url, &data);
	curl_easy_setopt(curl, CURLOPT_USERPWD, quotes_auth);
	free(url);

	char send_buf[1024];
	char* s = send_buf;

	if(new_time >= 0){
		s += snprintf(send_buf, sizeof(send_buf), "%u", id);
	}

	*s++ = ':';
	*s++ = '\0';

	if(new_txt){
		inso_strcat(send_buf, sizeof(send_buf), new_txt);
	}

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, send_buf);

	Quote* quote = NULL;
	if(inso_curl_perform(curl, &data) == 200){
		sb_each(q, chan_quotes[chan - channels]){
			if(q->id == id){
				quote = q;
				break;
			}
		}

		if(quote){
			if(new_txt){
				free(quote->text);
				quote->text = strdup(new_txt);
			}
			if(new_time >= 0){
				quote->timestamp = new_time;
			}
		} else {
			quote = quote_www_get(chan, id);
		}
	}

	sb_free(data);
	return quote;
}

static bool quote_www_delete(QChan* chan, uint32_t id){
	char* data = NULL;
	char* url  = NULL;
	asprintf_check(&url, QUOTES_URL "/%s/%u", chan->name+1, id);
	inso_curl_reset(curl, url, &data);
	curl_easy_setopt(curl, CURLOPT_USERPWD, quotes_auth);
	free(url);

	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

	bool ret = false;
	if(inso_curl_perform(curl, &data) == 200){
		ret = true;

		Quote** base = chan_quotes + (chan - channels);
		sb_each(q, *base){
			if(q->id == id){
				sb_erase(*base, q - *base);
				break;
			}
		}
	}

	sb_free(data);
	return ret;
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

	bool same_chan;
	QChan* quote_chan = quotes_get_chan(chan, &arg, &same_chan);
	Quote** quotes = chan_quotes + (quote_chan - channels);

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

				Quote* q = quote_www_get(quote_chan, id);
				if(q){
					ctx->send_msg(chan, "Quote %d: \"%s\" ―%s %s", id, q->text, quote_chan->name+1, quote_strtime(q));
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
			ctx->send_msg(chan, "Quote %d: \"%s\" ―%s %s", q->id, q->text, quote_chan->name+1, quote_strtime(q));
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

			Quote* q = quote_www_post(quote_chan, arg);
			if(!q){
				ctx->send_msg(chan, "%s: I'm not adding an empty quote...", name);
				break;
			}

			ctx->send_msg(chan, "%s: Added as quote %d.", name, q->id);

			// if adding to another channel, send a message to that channel.
			if(!same_chan){
				quotes_notify(quote_chan->name, name, q);
			}

			// notify other instances so they can also send messages to the affected channel
			char ipc_buf[256];
			int ipc_len = snprintf(ipc_buf, sizeof(ipc_buf), "ADD %d %s %s", q->id, quote_chan->name, name);
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

			if(quote_www_delete(quote_chan, id)){
				ctx->send_msg(chan, "%s: Deleted quote %d", name, id);
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

			Quote* q = quote_www_modify(quote_chan, id, arg2+1, -1);
			if(q){
				ctx->send_msg(chan, "%s: Updated quote %d.", name, id);
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

			struct tm timestamp = {};
			char* ret = strptime(arg2 + 1, "%F %T", &timestamp);
			if(!ret){
				ctx->send_msg(chan, "%s: Sorry, I don't understand that timestamp. Use YYYY-MM-DD hh:mm:ss", name);
				break;
			}
			ssize_t t = timegm(&timestamp);

			Quote* q = quote_www_modify(quote_chan, id, NULL, t);
			if(q){
				ctx->send_msg(chan, "%s: Updated quote %d's timestamp successfully.", name, id);
			} else {
				ctx->send_msg(chan, "%s: Can't find that quote.", name);
			}
		} break;

		case LIST_QUOTES: {
			ctx->send_msg(chan, "%s: You can find a list of quotes at " QUOTES_URL, name);
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
				ctx->send_msg(chan, "Quote %d: \"%s\" ―%s %s", q->id, q->text, quote_chan->name+1, quote_strtime(q));
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
}

static void quotes_ipc(int sender, const uint8_t* data, size_t data_len){
	if(!memchr(data, 0, data_len)) return;

	int id;
	char* chan = NULL;
	int name_offset = 0;

	if(sscanf(data, "ADD %d %ms %n", &id, &chan, &name_offset) == 2){
		const char* c = chan;
		QChan* qc = quotes_get_chan(NULL, &c, NULL);
		if(qc){
			quotes_notify(chan, data + name_offset, quote_www_get(qc, id));
		}
	}

	free(chan);
}
