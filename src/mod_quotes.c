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
	.on_save  = &quotes_save,
};

static const IRCCoreCtx* ctx;

static char* gist_auth;
static char* gist_url;

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

	printf("GIST_TOKEN: %s\n", url_buf);

	CURL* curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_USERPWD, gist_auth);

	snprintf(url_buf, sizeof(url_buf), "https://api.github.com/gists/%s", gist_id);
	gist_url = strdup(url_buf);

	printf("GIST_URL: %s\n", url_buf);
	
	char* data = NULL;

	curl_easy_setopt(curl, CURLOPT_URL, url_buf);
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

		load_csv(content->u.string, chan_quotes + i);
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

static void quotes_msg(const char* chan, const char* name, const char* msg){
/*
   \q    <num>
   \qadd <text>
   \qdel <num>
   \qfix <num> <text>
   \qs   <text>
*/
}

static void quotes_join(const char* chan, const char* user){
	if(strcasecmp(user, ctx->get_username())){
		for(char** c = channels; c < sb_end(channels); ++c){
			if(strcasecmp(chan, *c) == 0){
				sb_push(channels, strdup(chan));
				sb_push(chan_quotes, 0);
				break;
			}
		}
	}
}

static const char desc_key[]    = "description";
static const char desc_val[]    = "IRC quotes";
static const char files_key[]   = "files";
static const char content_key[] = "content";
static const char readme_key[]  = "_insobot_quotes";
static const char readme_val[]  =
"Here are the quotes stored by insobot, in csv format, one file per channel.";

static size_t curl_discard(char* ptr, size_t sz, size_t nmemb, void* data){
	return sz * nmemb;
}

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

	CURL* curl = curl_easy_init();

	struct curl_slist* slist = curl_slist_append(NULL, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_USERPWD, gist_auth);
	curl_easy_setopt(curl, CURLOPT_URL, gist_url);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, len);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
	curl_easy_setopt(curl, CURLOPT_WRITEHEADER, stderr);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &fwrite);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_discard);

	CURLcode ret = curl_easy_perform(curl);

	if(ret != 0){
		printf("CURL returned %d, %s\n", ret, curl_easy_strerror(ret));
	}

	curl_slist_free_all(slist);

	curl_easy_cleanup(curl);

	yajl_gen_free(json);
}

