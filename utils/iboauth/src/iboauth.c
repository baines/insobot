#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <argz.h>
#include <envz.h>
#include <curl/curl.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include "iboauth.h"
#include "uj.h"

void handle_twitch_redirect (void);
void handle_twitch_response (void);

struct route {
	const char* method;
	const char* path;
	route_handler handler;
} routes[] = {
	{ "GET", "/twitch"         , &handle_twitch_redirect },
	{ "GET", "/twitch_callback", &handle_twitch_response },
};

#define asprintf_check(...) ({       \
	if(asprintf(__VA_ARGS__) == -1){ \
		perror("asprintf");          \
		assert(0);                   \
	}                                \
})

static size_t inso_curl_callback(char* ptr, size_t sz, size_t nmemb, void* data)
{
	char** out = (char**)data;
	const size_t total = sz * nmemb;
	memcpy(sb_add(*out, total), ptr, total);
	return total;
}

static void inso_curl_reset(CURL* curl, const char* url, char** data)
{
	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &inso_curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8);
}

static CURL* inso_curl_init(const char* url, char** data)
{
	CURL* curl = curl_easy_init();
	inso_curl_reset(curl, url, data);
	return curl;
}

static long inso_curl_perform(CURL* curl, char** data)
{
	CURLcode curl_ret = curl_easy_perform(curl);

	if(data){
		sb_push(*data, 0);
	}

	long http_ret = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_ret);

	if(curl_ret != 0){
		return -curl_ret;
	} else {
		return http_ret;
	}
}

static void hexify(char out[static HASHLEN*2+1], const uint8_t in[static HASHLEN])
{
	static const char hex[] = "0123456789abcdef";
	char* p = out;

	for(int i = 0; i < HASHLEN; ++i) {
		*p++ = hex[in[i] >> 4];
		*p++ = hex[in[i] & 15];
	}

	*p = '\0';
}

static void create_state(char output[static HASHLEN*2+1], int offset)
{
	const char* remote_ip = util_getenv("REMOTE_ADDR");
	const char* secret = util_getenv("INSOBOT_TWITCH_CLIENT_SECRET");
	size_t time_key = (time(0) >> 9) + offset;

	char* input;
	asprintf_check(&input, "%zu:%s:%zu:%s", time_key, secret, time_key, remote_ip);

	uint8_t hash[HASHLEN];
	sha256(hash, input, strlen(input));
	hexify(output, hash);
}

void handle_twitch_redirect (void)
{
	const char* client_id = util_getenv("INSOBOT_TWITCH_CLIENT_ID");
	const char* redirect_uri = util_getenv("INSOBOT_TWITCH_REDIRECT");

	char state[HASHLEN*2+1];
	create_state(state, 0);

	char* location = NULL;
	asprintf_check(&location,
				   "https://id.twitch.tv/oauth2/authorize"
				   "?client_id=%s"
				   "&redirect_uri=%s"
				   "&response_type=code"
				   "&scope=openid+channel:manage:broadcast+channel:manage:polls+channel:read:polls"
				   "&claims={\"id_token\":{\"picture\":null,\"preferred_username\":null}}"
				   "&state=%s",
				   client_id,
				   redirect_uri,
				   state);

	printf("Status: 302 Found\r\n");
	printf("Location: %s\r\n", location);
	printf("\r\n");

	free(location);
}

struct uj_node* uj_get(struct uj_node* root, const char* key, enum uj_type type)
{
	if(!root || root->type != UJ_OBJ) {
		return NULL;
	}

	for(struct uj_kv* kv = root->obj; kv; kv = kv->next) {
		if(strcmp(key, kv->key) == 0) {
			return (kv->val && kv->val->type == type)
				? kv->val
				: NULL
				;
		}
	}

	return NULL;
}

void handle_twitch_response (void)
{
	const char* query = util_getenv("QUERY_STRING");
	char* argz;
	size_t lenz;

	if(argz_create_sep(query, '&', &argz, &lenz) != 0){
		util_exit(400);
	}

	char* code  = envz_get(argz, lenz, "code");
	char* state = envz_get(argz, lenz, "state");
	if(!code || !state){
		util_exit(400);
	}

	char real_state[HASHLEN*2+1];
	create_state(real_state, 0);

	if(strcmp(real_state, state) != 0) {
		create_state(real_state, -1);
		if(strcmp(real_state, state) != 0) {
			util_exit(400);
		}
	}

	const char* client_id = util_getenv("INSOBOT_TWITCH_CLIENT_ID");
	const char* client_secret = util_getenv("INSOBOT_TWITCH_CLIENT_SECRET");
	const char* redirect_uri = util_getenv("INSOBOT_TWITCH_REDIRECT");
	char* data = NULL;
	char* url;

	asprintf_check(&url,
				   "https://id.twitch.tv/oauth2/token"
				   "?client_id=%s"
				   "&client_secret=%s"
				   "&code=%s"
				   "&grant_type=authorization_code"
				   "&redirect_uri=%s",
				   client_id,
				   client_secret,
				   code,
				   redirect_uri);

	CURL* curl = inso_curl_init(url, &data);
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0);

	if(inso_curl_perform(curl, &data) == 200) {
		struct uj_node* json = uj_parse(data, strlen(data), NULL);
		if(!json) {
			util_exit(400);
		}

		struct uj_node* access  = uj_get(json, "access_token", UJ_STR);
		struct uj_node* refresh = uj_get(json, "refresh_token", UJ_STR);
		struct uj_node* id      = uj_get(json, "id_token", UJ_STR);
		//struct uj_node* expiry  = uj_get(json, "expires_in", UJ_INT);

		if(!access || !refresh || !id) {
			util_exit(400);
		}

		int dot_count = 0;
		const char* payload = NULL;

		for(char* p = id->str; *p; ++p) {
			if(*p == '.') {
				if(!payload) {
					payload = p+1;
				}
				++dot_count;
			}
		}

		if(dot_count != 2) {
			exit_error(500);
		}

		char* jwtstr = unbase64(payload, strnlen(payload, 4096));
		if(!jwtstr) {
			exit_error(500);
		}

		struct uj_node* jwt = uj_parse(jwtstr, strlen(jwtstr), NULL);
		if(!jwt) {
			exit_error(500);
		}

		struct uj_node* userid = uj_get(jwt, "sub", UJ_STR);
		struct uj_node* username = uj_get(jwt, "preferred_username", UJ_STR);
		struct uj_node* avatar = uj_get(jwt, "picture", UJ_STR);
		if(!userid || !username) {
			exit_error(500);
		}

		int twitch_data_file = open(TWITCH_DATA_FILE, O_RDWR | O_CREAT, 0644);
		if(twitch_data_file == -1) {
			exit_error(500);
		}

		if(flock(twitch_data_file, LOCK_EX) == -1) {
			exit_error(500);
		}

		if(lseek(twitch_data_file, 0, SEEK_END) == -1) {
			exit_error(500);
		}

		dprintf(twitch_data_file, "OAUTH\t%s %s %s\n", userid->str, access->str, refresh->str);

		close(twitch_data_file);

		const char* params[] = {
			"name", username->str,
			"pic", avatar ? avatar->str : "",
			NULL
		};

		printf("Status: 200 OK\r\n");
		template_puts(GETBIN(linked_html), params, RESPONSE_HTML, 0);

		fflush(stdout);
		exit(0);
	}

	util_exit(400);
}

int main (void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	srand(time(NULL) ^ getpid() ^ ts.tv_nsec);

	const char* path = util_getenv("PATH_INFO");
	const char* method = util_getenv("REQUEST_METHOD");

	bool bad_method = false;

	for(size_t i = 0; i < countof(routes); ++i) {
		const struct route* r = routes + i;

		if(strcmp(r->path, path) == 0) {
			if(strcmp(r->method, method) == 0) {
				r->handler();
				fflush(stdout);
				return EXIT_SUCCESS;
			} else {
				bad_method = true;
			}
		}
	}

	if(bad_method) {
		util_exit(405);
	}

	printf("Status: 404 Not Found\r\n");
	printf("Content-Type: text/html\r\n");
	printf("\r\n");
	printf("<h1>404 Not Found</h1>\n");
	fflush(stdout);

	return 0;
}
