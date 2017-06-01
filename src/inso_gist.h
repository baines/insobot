#ifndef INSO_GIST_H
#define INSO_GIST_H
#include <stdbool.h>

typedef struct inso_gist inso_gist;

typedef struct inso_gist_file {
	char* name;
	char* content;
	struct inso_gist_file* next;
} inso_gist_file;

enum {
	INSO_GIST_OK,
	INSO_GIST_HTTP_ERROR,
	INSO_GIST_JSON_ERROR,
	INSO_GIST_304 = 304,
};

inso_gist* inso_gist_open   (const char* id  , const char* user, const char* token);
inso_gist* inso_gist_find   (const char* desc, const char* user, const char* token);
inso_gist* inso_gist_new    (const char* desc, const char* user, const char* token, const inso_gist_file* in, bool pub);
int        inso_gist_load   (inso_gist*, inso_gist_file** out);
int        inso_gist_save   (inso_gist*, const char* desc, const inso_gist_file* in);
void       inso_gist_lock   (inso_gist*);
void       inso_gist_unlock (inso_gist*);
void       inso_gist_close  (inso_gist*);

void inso_gist_file_add  (inso_gist_file**, const char* name, const char* content);
void inso_gist_file_free (inso_gist_file*);

#endif

// implementation

#ifdef INSO_IMPL
#undef INSO_IMPL
#include <curl/curl.h>
#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <errno.h>
#include "stb_sb.h"
#include "inso_utils.h"

void inso_gistpriv_seminit (inso_gist*);
int  inso_gistpriv_post    (inso_gist*, yajl_gen, const char*, const inso_gist_file*);

struct inso_gist {
	CURL* curl;
	char* auth;
	char* api_url;

	// Unfortunately, GitHub's Gist ETags are pretty broken.
	// The PATCH request gives an ETag that does not even match the new contents,
	// and when using an 'If-Match' header in a PATCH request with an invalid Etag,
	// their API returns 412 _BUT STILL MODIFIES THE CONTENTS_ which is super broken.
	// I emailed them about this ages ago, but it seems they have no intention of fixing it.
	// So for now, fall back to If-Modified-Since instead.
#if GITHUB_FIXED_THEIR_API
	char* etag;
#else
	time_t last_modified;
#endif

	int semaphore;
};

void inso_gistpriv_seminit(inso_gist* gist){
	assert(strlen(gist->api_url) > (29+8));
	const char* id = gist->api_url + 29;

	char keybuf[9] = {};
	memcpy(keybuf, id, 8);
	key_t key = strtoul(keybuf, NULL, 16);

	int setup_sem = 1;
	int sem_flags = IPC_CREAT | IPC_EXCL | 0666;
	while((gist->semaphore = semget(key, 1, sem_flags)) == -1){
		if(errno == EEXIST){
			sem_flags &= ~IPC_EXCL;
			setup_sem = 0;
		} else {
			perror("semget");
			break;
		}
	}

	if(setup_sem){
		if((semctl(gist->semaphore, 0, SETVAL, 1) == -1)){
			perror("semctl");
		}
	}
}

inso_gist* inso_gist_open(const char* id, const char* user, const char* token){
	inso_gist* gist = calloc(1, sizeof(*gist));
	assert(gist);

	asprintf_check(&gist->auth   , "%s:%s", user, token);
	asprintf_check(&gist->api_url, "https://api.github.com/gists/%s", id);

	gist->curl = curl_easy_init();

	inso_gistpriv_seminit(gist);

	return gist;
}

inso_gist* inso_gist_find(const char* find_desc, const char* user, const char* token){
	inso_gist* gist = calloc(1, sizeof(*gist));
	assert(gist);

	asprintf_check(&gist->auth, "%s:%s", user, token);

	char* data = NULL;
	gist->curl = inso_curl_init("https://api.github.com/gists", &data);
	curl_easy_setopt(gist->curl, CURLOPT_USERPWD, gist->auth);
	yajl_val root = NULL;

	long ret = inso_curl_perform(gist->curl, &data);
	if(ret < 0){
		printf("inso_gist_find: curl returned [%s]\n", curl_easy_strerror(-ret));
		goto out;
	} else if(ret != 200){
		printf("inso_gist_find: unexpected http code [%ld]\n", ret);
		goto out;
	}

	root = yajl_tree_parse(data, NULL, 0);
	if(!root || !YAJL_IS_ARRAY(root)){
		printf("inso_gist_find: json error.");
		goto out;
	}

	static const char* url_path[]  = { "url", NULL };
	static const char* desc_path[] = { "description", NULL };

	for(size_t i = 0; i < root->u.array.len; ++i){
		yajl_val obj  = root->u.array.values[i];
		yajl_val url  = yajl_tree_get(obj, url_path , yajl_t_string);
		yajl_val desc = yajl_tree_get(obj, desc_path, yajl_t_string);
		if(!url || !desc) continue;

		if(strcmp(desc->u.string, find_desc) == 0){
			gist->api_url = strdup(url->u.string);
			break;
		}
	}

out:
	sb_free(data);
	yajl_tree_free(root);

	if(gist->api_url){
		inso_gistpriv_seminit(gist);
		return gist;
	} else {
		inso_gist_close(gist);
		return NULL;
	}
}

inso_gist* inso_gist_new(const char* desc, const char* user, const char* token, const inso_gist_file* in, bool pub){
	inso_gist* gist = calloc(1, sizeof(*gist));
	assert(gist);

	asprintf_check(&gist->auth, "%s:%s", user, token);

	char* data = NULL;
	gist->curl = inso_curl_init("https://api.github.com/gists", &data);

	yajl_gen json = yajl_gen_alloc(NULL);
	yajl_gen_map_open(json);

	yajl_gen_string(json, "public", 6);
	yajl_gen_bool(json, pub);

	static const char* url_path[]  = { "url", NULL };

	if(inso_gistpriv_post(gist, json, desc, in) == INSO_GIST_OK){
		sb_push(data, 0);
		yajl_val root = yajl_tree_parse(data, NULL, 0);
		yajl_val url  = yajl_tree_get(root, url_path , yajl_t_string);

		if(url){
			gist->api_url = strdup(url->u.string);
		}

		yajl_tree_free(root);
	}

	sb_free(data);

	if(gist->api_url){
		inso_gistpriv_seminit(gist);
		return gist;
	} else {
		inso_gist_close(gist);
		return NULL;
	}
}

#if GITHUB_FIXED_THEIR_API
static size_t inso_gist_header_cb(char* buffer, size_t size, size_t nelem, void* arg){
	char* etag;
	inso_gist* gist = arg;

	if(buffer && sscanf(buffer, "ETag: %m[^\r\n]", &etag) == 1){
		if(gist->etag){
			free(gist->etag);
		}
		printf("inso_gist: ETag: %s\n", etag);
		gist->etag = etag;
	}

	return size * nelem;
}
#else
static size_t inso_gist_header_cb(char* buffer, size_t size, size_t nelem, void* arg){
	struct tm date;
	inso_gist* gist = arg;

	if(buffer && strptime(buffer, "Date: %a, %d %b %Y %H:%M:%S GMT", &date)){
		gist->last_modified = timegm(&date);
		printf("inso_gist: Date: %ld\n", gist->last_modified);
	}

	return size * nelem;
}
#endif

static size_t inso_gist_noop_cb(char* buffer, size_t size, size_t nelem, void* arg){
	return size * nelem;
}

int inso_gist_load(inso_gist* gist, inso_gist_file** out){
	assert(gist);

	char* data = NULL;
	inso_curl_reset(gist->curl, gist->api_url, &data);
	curl_easy_setopt(gist->curl, CURLOPT_USERPWD, gist->auth);
	curl_easy_setopt(gist->curl, CURLOPT_HEADERFUNCTION, &inso_gist_header_cb);
	curl_easy_setopt(gist->curl, CURLOPT_HEADERDATA, gist);

	struct curl_slist* headers = NULL;

#if GITHUB_FIXED_THEIR_API
	if(gist->etag){
		char buf[1024];
		snprintf(buf, sizeof(buf), "If-None-Match: %s", gist->etag);
		headers = curl_slist_append(NULL, buf);
		curl_easy_setopt(gist->curl, CURLOPT_HTTPHEADER, headers);
	}
#else
	curl_easy_setopt(gist->curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
	curl_easy_setopt(gist->curl, CURLOPT_TIMEVALUE, gist->last_modified);
#endif

	CURLcode ret = curl_easy_perform(gist->curl);

	if(headers){
		curl_slist_free_all(headers);
	}

	long http_code = 0;
	curl_easy_getinfo(gist->curl, CURLINFO_RESPONSE_CODE, &http_code);

	if(ret == CURLE_OK && http_code == 304){
		sb_free(data);
		return INSO_GIST_304;
	}

	if(ret != 0 || http_code != 200){
		sb_free(data);
		return INSO_GIST_HTTP_ERROR;
	}

	sb_push(data, 0);

	static const char* files_path[]   = { "files"  , NULL };
	static const char* content_path[] = { "content", NULL };

	yajl_val root  = yajl_tree_parse(data, NULL, 0);
	yajl_val files = yajl_tree_get(root, files_path, yajl_t_object);

	sb_free(data);

	inso_gist_file* file_list = NULL;

	if(!root || !files){
		goto json_error;
	}

	for(size_t i = 0; i < files->u.object.len; ++i){
		yajl_val file = files->u.object.values[i];
		if(!YAJL_IS_OBJECT(file)){
			goto json_error;
		}

		yajl_val content = yajl_tree_get(file, content_path, yajl_t_string);
		if(!content){
			goto json_error;
		}

		inso_gist_file_add(&file_list, files->u.object.keys[i], content->u.string);
	}

	if(out){
		*out = file_list;
	} else {
		inso_gist_file_free(file_list);
	}

	yajl_tree_free(root);
	return INSO_GIST_OK;

json_error:
	yajl_tree_free(root);
	inso_gist_file_free(file_list);
	return INSO_GIST_JSON_ERROR;
}

int inso_gist_save(inso_gist* gist, const char* desc, const inso_gist_file* in){
	yajl_gen json = yajl_gen_alloc(NULL);
	yajl_gen_map_open(json);

	inso_curl_reset(gist->curl, gist->api_url, NULL);
	curl_easy_setopt(gist->curl, CURLOPT_CUSTOMREQUEST, "PATCH");
	curl_easy_setopt(gist->curl, CURLOPT_WRITEFUNCTION, &inso_gist_noop_cb);

	return inso_gistpriv_post(gist, json, desc, in);
}

int inso_gistpriv_post(inso_gist* gist, yajl_gen json, const char* desc, const inso_gist_file* in){
	assert(gist);
	assert(json);

	static const char desc_key[]    = "description";
	static const char files_key[]   = "files";
	static const char content_key[] = "content";

	// XXX: caller is expected to open the map to start with.
	//      even though this func will close it and free the json

	yajl_gen_string(json, desc_key , sizeof(desc_key)  - 1);
	yajl_gen_string(json, desc     , strlen(desc));
	yajl_gen_string(json, files_key, sizeof(files_key) - 1);

	yajl_gen_map_open(json);
	for(const inso_gist_file* file = in; file; file = file->next){
		yajl_gen_string(json, file->name, strlen(file->name));

		if(file->content){
			yajl_gen_map_open(json);
			yajl_gen_string(json, content_key, sizeof(content_key) - 1);
			yajl_gen_string(json, file->content, strlen(file->content));
			yajl_gen_map_close(json);
		} else {
			yajl_gen_null(json);
		}
	}
	yajl_gen_map_close(json);
	yajl_gen_map_close(json);

	size_t len = 0;
	const unsigned char* payload = NULL;
	yajl_gen_get_buf(json, &payload, &len);

	struct curl_slist* slist = NULL;
	slist = curl_slist_append(slist, "Content-Type: application/json");
	slist = curl_slist_append(slist, "Expect:");

	curl_easy_setopt(gist->curl, CURLOPT_USERPWD, gist->auth);
	curl_easy_setopt(gist->curl, CURLOPT_POST, 1);
	curl_easy_setopt(gist->curl, CURLOPT_POSTFIELDS, payload);
	curl_easy_setopt(gist->curl, CURLOPT_POSTFIELDSIZE, len);
	curl_easy_setopt(gist->curl, CURLOPT_HTTPHEADER, slist);
	curl_easy_setopt(gist->curl, CURLOPT_HEADERFUNCTION, &inso_gist_header_cb);
	curl_easy_setopt(gist->curl, CURLOPT_HEADERDATA, gist);

	CURLcode ret = curl_easy_perform(gist->curl);

	double seconds = 0.;
	curl_easy_getinfo(gist->curl, CURLINFO_TOTAL_TIME, &seconds);
	printf("inso_gist: Upload took [%.2f] seconds.\n", seconds);

	curl_slist_free_all(slist);
	yajl_gen_free(json);

	return ret == 0 ? INSO_GIST_OK : INSO_GIST_HTTP_ERROR;
}

void inso_gist_file_add(inso_gist_file** file, const char* name, const char* content){
	assert(file);
	assert(name);

	inso_gist_file** f = file;
	while(*f) f = &(*f)->next;

	*f = calloc(1, sizeof(**f));
	(*f)->name = strdup(name);

	if(content){
		(*f)->content = strdup(content);
	}
}

void inso_gist_file_free(inso_gist_file* file){
	while(file){
		inso_gist_file* tmp = file;

		if(file->name){
			free(file->name);
		}

		if(file->content){
			free(file->content);
		}

		file = file->next;

		free(tmp);
	}
}

void inso_gist_lock(inso_gist* gist){
	assert(gist);

	struct sembuf lock = {
		.sem_op = -1,
		.sem_flg = SEM_UNDO
	};
	semop(gist->semaphore, &lock, 1);
}

void inso_gist_unlock(inso_gist* gist){
	assert(gist);

	struct sembuf unlock = {
		.sem_op = 1,
		.sem_flg = SEM_UNDO
	};
	semop(gist->semaphore, &unlock, 1);
}

void inso_gist_close(inso_gist* gist){
	if(!gist) return;

	free(gist->api_url);
	free(gist->auth);

#if GITHUB_FIXED_THEIR_API
	if(gist->etag){
		free(gist->etag);
	}
#endif

	curl_easy_cleanup(gist->curl);

	free(gist);

	// free semaphore?
}

#endif
