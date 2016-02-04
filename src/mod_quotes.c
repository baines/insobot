#include "module.h"
#include <curl/curl.h>
#include "stb_sb.h"
#include <time.h>
#include <string.h>
#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>

static bool quotes_init (const IRCCoreCtx*);
static void quotes_join (const char*, const char*);
static void quotes_msg  (const char*, const char*, const char*);
static void quotes_save (FILE*);

const IRCModuleCtx irc_mod_ctx = {
	.name     = "quotes",
	.desc     = "Saves per-channel quotes",
	.on_init  = quotes_init,
	.on_msg   = &quotes_msg,
	.on_join  = &quotes_join,
	.on_save  = &quotes_save,
};

static const IRCCoreCtx* ctx;

static char* gist_auth;
static char* gist_api_url;
static char* gist_pub_url;

static char** channels;

typedef struct Quote_ {
	uint32_t id;
	time_t timestamp;
	char* text;
} Quote;

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

		q.timestamp = mktime(&timestamp);

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
		*o = 0;

		q.text = strndup(text_buff, o - text_buff);

		sb_push(*qlist, q);
	}

	free(buffer);

}

static size_t curl_callback(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = (char**)data;
	const size_t total = sz * nmemb;

	memcpy(sb_add(*out, total), ptr, total);

	return total;
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

	char url_buf[1024];
	snprintf(url_buf, sizeof(url_buf), "%s:%s", gist_user, gist_token);
	gist_auth = strdup(url_buf);

	CURL* curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_USERPWD, gist_auth);

	snprintf(url_buf, sizeof(url_buf), "https://api.github.com/gists/%s", gist_id);
	gist_api_url = strdup(url_buf);

	snprintf(url_buf, sizeof(url_buf), "https://gist.github.com/%s", gist_id);
	gist_pub_url = strdup(url_buf);

	char* data = NULL;

	curl_easy_setopt(curl, CURLOPT_URL, gist_api_url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

	CURLcode ret = curl_easy_perform(curl);
	
	curl_easy_cleanup(curl);

	if(ret != 0){
		printf("CURL returned %d, %s\n", ret, curl_easy_strerror(ret));
		return false;
	}

	sb_push(data, 0);

	const char* files_path[]   = { "files",   NULL };
	const char* content_path[] = { "content", NULL };

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!YAJL_IS_OBJECT(root)){
		fprintf(stderr, "mod_quotes: error getting root object\n");
		return false;
	}

	yajl_val files = yajl_tree_get(root, files_path, yajl_t_object);
	if(!files){
		fprintf(stderr, "mod_quotes: error getting files object\n");
		return false;
	}

	for(size_t i = 0; i < files->u.object.len; ++i){
		const char* filename = files->u.object.keys[i];

		if(!filename || filename[0] != '#') continue;

		yajl_val file = files->u.object.values[i];
		if(!YAJL_IS_OBJECT(file)){
			fprintf(stderr, "mod_quotes: error getting file object\n");
			break;
		}

		yajl_val content = yajl_tree_get(file, content_path, yajl_t_string);
		if(!content){
			fprintf(stderr, "mod_quotes: error getting content string\n");
			break;
		}

		sb_push(channels, strdup(filename));
		sb_push(chan_quotes, 0);

		load_csv(content->u.string, &sb_last(chan_quotes));
	}
	
	for(size_t i = 0; i < sb_count(channels); ++i){
		Quote* q = chan_quotes[i];

		printf("%s\n", channels[i]);

		for(size_t j = 0; j < sb_count(q); ++j){
			printf("\t%d, %lu, %s\n", q[j].id, q[j].timestamp, q[j].text);
		}
	}

	yajl_tree_free(root);

	return true;
}

static Quote** get_quotes(const char* chan){
	for(int i = 0; i < sb_count(channels); ++i){
		if(strcmp(chan, channels[i]) == 0){
			return chan_quotes + i;
		}
	}
	return NULL;
}

static Quote* get_quote(const char* chan, int id){
	int index = -1;
	for(int i = 0; i < sb_count(channels); ++i){
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

static void whitelist_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
}

static void quotes_msg(const char* chan, const char* name, const char* msg){

	enum { GET_QUOTE, ADD_QUOTE, DEL_QUOTE, FIX_QUOTE, FIX_TIME, LIST_QUOTES, SEARCH_QUOTES	};

	const char* arg = msg;
	int i = ctx->check_cmds(
		&arg,
		"\\q",
		"\\qadd",
		"\\qdel,\\qrm",
		"\\qfix,\\qmv",
		"\\qft",
		"\\qlist",
		"\\qs",
		NULL
	);
	if(i < 0) return;

	bool has_cmd_perms = strcasecmp(chan+1, name) == 0;
	
	if(!has_cmd_perms){
		ctx->send_mod_msg(&(IRCModMsg){
			.cmd      = "check_whitelist",
			.arg      = (intptr_t)name,
			.callback = &whitelist_cb,
			.cb_arg   = (intptr_t)&has_cmd_perms
		});
	}

	if(!has_cmd_perms) return;

	Quote** quotes = get_quotes(chan);
	if(!quotes){
		fprintf(stderr, "mod_quotes: BUG, got message from channel we don't know about?\n");
		return;
	}

	switch(i){
		case GET_QUOTE: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Usage: \\q <id>", name);
				break;
			}
			char* end;
			int id = strtol(arg, &end, 0);
			if(end == arg || id < 0){
				ctx->send_msg(chan, "%s: Quotes start at id 0.", name);
				break;
			}
			Quote* q = get_quote(chan, id);
			if(q){
				struct tm* date_tm = gmtime(&q->timestamp);
				char date[256];
				strftime(date, sizeof(date), "%F", date_tm);
				ctx->send_msg(chan, "Quote %d: \"%s\" --%s %s", id, q->text, chan+1, date);
			} else {
				ctx->send_msg(chan, "%s: Can't find that quote.", name);
			}
		} break;

		case ADD_QUOTE: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Usage: \\qadd <text>", name);
				break;
			}
			int id = 0;
			if(sb_count(*quotes) > 0){
				id = sb_last(*quotes).id + 1;
			}
			Quote q = {
				.id = id,
				.text = strdup(arg),
				.timestamp = time(0)
			};
			sb_push(*quotes, q);
			ctx->send_msg(chan, "%s: Added as quote %d.", name, id);
			quotes_save(NULL);
		} break;

		case DEL_QUOTE: {
			if(!*arg){
				ctx->send_msg(chan, "%s: Usage: \\qdel <id>", name);
				break;
			}
			char* end;
			int id = strtol(arg, &end, 0);
			if(!end || end == arg || *end || id < 0){
				ctx->send_msg(chan, "%s: That id doesn't look valid.", name);
				break;
			}
			Quote* q = get_quote(chan, id);
			if(q){
				int off = q - *quotes;
				sb_erase(*quotes, off);
				ctx->send_msg(chan, "%s: Deleted quote %d\n", name, id);
				quotes_save(NULL);
			} else {
				ctx->send_msg(chan, "%s: Can't find that quote.", name);
			}
		} break;

		case FIX_QUOTE: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Usage: \\qfix <id> <new_text>", name);
				break;
			}
			char* arg2;
			int id = strtol(arg, &arg2, 0);
			
			if(!arg2 || arg2 == arg || id < 0){
				ctx->send_msg(chan, "%s: That id doesn't look valid.", name);
				break;
			}
			if(*arg2 != ' '){
				ctx->send_msg(chan, "%s: Usage: \\qfix <id> <new_text>", name);
				break;
			}

			Quote* q = get_quote(chan, id);
			if(q){
				free(q->text);
				q->text = strdup(arg2 + 1);
				ctx->send_msg(chan, "%s: Updated quote %d.", name, id);
				quotes_save(NULL);
			} else {
				ctx->send_msg(chan, "%s: Can't find that quote.", name);
			}
		} break;

		case FIX_TIME: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Usage: \\qft <id> <timestamp>", name);
				break;
			}

			char* arg2;
			int id = strtol(arg, &arg2, 0);
			if(!arg2 || arg2 == arg || id < 0){
				ctx->send_msg(chan, "%s: That id doesn't look valid.", name);
				break;
			}
			if(*arg2 != ' '){
				ctx->send_msg(chan, "%s: Usage: \\qft <id> <YYYY-MM-DD hh:mm:ss>", name);
				break;
			}

			Quote* q = get_quote(chan, id);
			if(!q){
				ctx->send_msg(chan, "%s: Can't find that quote.", name);
				break;
			}

			struct tm timestamp;
			char* ret = strptime(arg2 + 1, "%F %T", &timestamp);
			if(ret){
				q->timestamp = mktime(&timestamp);
				ctx->send_msg(chan, "%s: Updated quote %d's timestamp successfully.", name, id);
				quotes_save(NULL);
			} else {
				ctx->send_msg(chan, "%s: Sorry, I don't understand that timestamp. Use YYYY-MM-DD hh:mm:ss", name);
			}

		} break;

		case LIST_QUOTES: {
			ctx->send_msg(chan, "%s: You can find a list of quotes at %s", name, gist_pub_url);
		} break;
	}
	//TODO: qfix, qsearch
}

static void quotes_join(const char* chan, const char* user){
	if(strcasecmp(user, ctx->get_username()) != 0) return;

	bool found = false;
	for(char** c = channels; c < sb_end(channels); ++c){
		if(strcasecmp(chan, *c) == 0){
			found = true;
			break;
		}
	}

	if(!found){
		fprintf(stderr, "mod_quotes: adding [%s]\n", chan);
		sb_push(channels, strdup(chan));
		sb_push(chan_quotes, 0);
	}
}

static const char desc_key[]    = "description";
static const char desc_val[]    = "IRC quotes";
static const char files_key[]   = "files";
static const char content_key[] = "content";
static const char readme_key[]  = " Quote List";
static const char readme_val[]  =
"Here are the quotes stored by insobot, in csv format, one file per channel. Times are UTC.";

static void quotes_save(FILE* file){

	yajl_gen json = yajl_gen_alloc(NULL);

	yajl_gen_map_open(json);

	yajl_gen_string(json, desc_key, sizeof(desc_key) - 1);
	yajl_gen_string(json, desc_val, sizeof(desc_val) - 1);

	yajl_gen_string(json, files_key, sizeof(files_key) - 1);
	yajl_gen_map_open(json);

	yajl_gen_string(json, readme_key, sizeof(readme_key) - 1);
	yajl_gen_map_open(json);
	yajl_gen_string(json, content_key, sizeof(content_key) - 1);
	yajl_gen_string(json, readme_val, sizeof(readme_val) - 1);
	yajl_gen_map_close(json);

	for(int i = 0; i < sb_count(channels); ++i){

		if(sb_count(chan_quotes[i]) == 0) continue;

		yajl_gen_string(json, channels[i], strlen(channels[i]));
		yajl_gen_map_open(json);

		yajl_gen_string(json, content_key, sizeof(content_key) - 1);
		
		char* csv = gen_escaped_csv(chan_quotes[i]);
		yajl_gen_string(json, csv, strlen(csv));
		sb_free(csv);

		yajl_gen_map_close(json);
	}

	yajl_gen_map_close(json);
	yajl_gen_map_close(json);

	size_t len = 0;
	const unsigned char* payload = NULL;

	yajl_gen_get_buf(json, &payload, &len);
	printf("Payload: [%zu] [%s]\n", len, payload);

	CURL* curl = curl_easy_init();

	struct curl_slist* slist = curl_slist_append(NULL, "Content-Type: application/json");

	char* data = NULL;

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_USERPWD, gist_auth);
	curl_easy_setopt(curl, CURLOPT_URL, gist_api_url);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, len);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
	curl_easy_setopt(curl, CURLOPT_WRITEHEADER, stderr);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &fwrite);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

	CURLcode ret = curl_easy_perform(curl);

	if(ret != 0){
		printf("CURL returned %d, %s\n", ret, curl_easy_strerror(ret));
		if(data){
			printf("RESPONSE: [%s]\n", data);
		}
	}

	sb_free(data);

	curl_slist_free_all(slist);

	curl_easy_cleanup(curl);

	yajl_gen_free(json);
}

