#include "ibstat.h"

#ifndef INSOBOT_DATA_PATH
	#error "You need to set INSOBOT_DATA_PATH in ibstat.h to point at your data folder"
#endif

// warning:
// I am not bothering to free memory, because this is a short-lived program. The OS'll take care of it.

struct ksv {
	char*  keys;
	size_t nkeys;
	char*  val;
};

static inline bool is_upper(char c) {
	return c >= 'A' && c <= 'Z';
}

sb(struct ksv) load_aliases(FILE* f, const char* channel) {

	const size_t chanlen = strlen(channel);
	sb(struct ksv) result = NULL;

	char* line = NULL;
	size_t cap = 0;
	ssize_t n;

	while((n = getline(&line, &cap, f)) != -1) {
		char* p = line;
		char* token;
		bool found = false;
		struct ksv alias = {};

		for(;;) {
			token = strsep(&p, " ");
			if(!token || is_upper(*token)) {
				break;
			}

			if(strncmp(token, channel, chanlen) == 0 && token[chanlen] == ',') {
				argz_add(&alias.keys, &alias.nkeys, strdup(token + chanlen + 1));
				found = true;
			}
		}

		if(!found || !token || !is_upper(*token)) {
			continue;
		}

		if(token && strncmp(token, "AUTHOR", 6) == 0) {
			// don't care about author
			token = strsep(&p, " ");
		}

		// TODO: token is the permission level here, do smth with that.

		if(p) {
			alias.val = strdup(p);
			sb_push(result, alias);
		}
	}

	return result;
}

struct psa {
	char* id;
	char* trigger;
	char* message;
	int freq_mins;
	bool when_live;
};

bool parse_psa_line(struct psa* psa, const char* line) {

	enum {
		S_BAD_REGEX = -5,
		S_NO_ID  = -4,
		S_NO_MSG = -3,
		S_NEGATIVE_FREQ = -2,
		S_UNKNOWN_TOKEN = -1,
		S_SUCCESS = 0,
		S_ID,
		S_MAIN,
		S_OPT_LIVE,
		S_OPT_TRIGGER,
		S_OPT_TTL,
		S_MSG
	} state = S_ID;

	static const struct {
		const char* name;
		int state;
	} opts[] = {
		{ "live"   , S_OPT_LIVE },
		{ "trigger", S_OPT_TRIGGER },
		{ "ttl"    , S_OPT_TTL },
	};

	char token[128];
	int len;
	const char* p = line;

	do {
		switch(state){
			case S_ID: {
				if(sscanf(p, " %127s%n", token, &len) == 1){
					p += len;
					psa->id = strdup(token);
					state = S_MAIN;
				} else {
					state = S_NO_ID;
				}
			} break;

			case S_MAIN: {
				if(sscanf(p, " +%127s%n", token, &len) == 1){
					bool got_opt = false;

					for(size_t i = 0; i < countof(opts); ++i){
						if(strcmp(opts[i].name, token) == 0){
							state = opts[i].state;
							got_opt = true;
							break;
						}
					}

					if(got_opt){
						p += len;
					} else {
						state = S_UNKNOWN_TOKEN;
					}
				} else if (sscanf(p, " %dm%n", &psa->freq_mins, &len) == 1){
					if(psa->freq_mins > 0){
						p += len;
						state = S_MSG;
					} else {
						state = S_NEGATIVE_FREQ;
					}
				} else {
					state = S_UNKNOWN_TOKEN;
				}
			} break;

			case S_OPT_LIVE: {
				psa->when_live = true;
				state = S_MAIN;
			} break;

			case S_OPT_TTL: {
				int ttl_mins;
				len = 0;
				if(sscanf(p, " %um%n", &ttl_mins, &len) == 1 && len) {
					p += len;
					//psa->keep_until = time(0) + ttl_mins * 60;
					state = S_MAIN;
				} else {
					state = S_NO_MSG;
				}
			} break;

			case S_OPT_TRIGGER: {
				state = S_NO_MSG;
				if(*p++ != ' ')
					break;

				char delim = *p++;
				if(delim != '\'' && delim != '"')
					delim = '\0';

				const char* end = strchr(p, delim);
				if(!end)
					break;

				psa->trigger = strndup(p, end - p);
				p = end + 1;
				state = S_MAIN;
			} break;

			case S_MSG: {
				if(*p++){
					psa->message = strdup(p);
					state = S_SUCCESS;
				} else {
					state = S_NO_MSG;
				}
			} break;

			default: {
				state = S_UNKNOWN_TOKEN;
			} break;
		}
	} while(state > 0);

	return state == S_SUCCESS;
}

sb(struct psa) load_psas(FILE* f, const char* channel) {

	char* line = NULL;
	size_t cap = 0;
	ssize_t n;

	sb(struct psa) result = NULL;

	while((n = getline(&line, &cap, f)) != -1) {
		struct psa psa = {};

		char* p = line;
		char* chan = strsep(&p, " ");

		if(!chan || strcmp(chan, channel) != 0) {
			continue;
		}

		if(parse_psa_line(&psa, p)) {
			sb_push(result, psa);
		}
	}

	return result;
}

void stat_channel(const char* input) {
	char channel[256];
	snprintf(channel, sizeof(channel), "#%s", input);

	FILE* alias_file = fopen(INSOBOT_DATA_PATH "/alias.data", "r");
	FILE* psa_file   = fopen(INSOBOT_DATA_PATH "/psa.data", "r");

	sb(char) alias_html = NULL;
	sb(char) psa_html = NULL;

	if(alias_file) {
		sb(struct ksv) res = load_aliases(alias_file, channel);

		sb_each(a, res) {
			sb(char) key_buf = NULL;
			char* key = NULL;
			while((key = argz_next(a->keys, a->nkeys, key))) {
				size_t len = strlen(key);
				sb_push(key_buf, '!');
				memcpy(sb_add(key_buf, len), key, len);
				sb_push(key_buf, ',');
				sb_push(key_buf, ' ');
			}

			size_t n = sb_count(key_buf);
			if(n > 2) {
				key_buf[n-1] = '\0';
				key_buf[n-2] = '\0';
			}

			sb_push(key_buf, 0);

			static const char template[] = "<tr><td><kbd>`keys|h`</kbd></td><td>`val|h`</td></tr>\n";
			const char* subst[] = {
				"keys", key_buf,
				"val", a->val,
			};
			template_append(&alias_html, template, sizeof(template)-1, subst);
		}
	}

	if(psa_file) {
		sb(struct psa) psas = load_psas(psa_file, channel);

		sb_each(p, psas) {
			static const char template[] = "<tr><td><kbd>`id|h`</kbd></td><td><kbd>`trigger|h`</kbd></td><td>`cooldown|h`</td><td>`rsp|h`</td></tr>\n";
			static char cooldown_buf[64];
			snprintf(cooldown_buf, sizeof(cooldown_buf), "%d Mins", p->freq_mins);
			const char* subst[] = {
				"id", p->id,
				"trigger", p->trigger ?: "<Periodic>",
				"rsp", p->message,
				"cooldown", cooldown_buf,
			};
			template_append(&psa_html, template, sizeof(template)-1, subst);
		}
	}

	const char* subst[] = {
		"chan", channel,
		"aliases", alias_html,
		"psas", psa_html,
	};

	printf("Status: 200 OK\r\n");
	template_puts(GETBIN(main_html), subst, RESPONSE_HTML, 0);
}

int main (void) {

	const char* path = util_getenv("PATH_INFO");
	const char* method = util_getenv("REQUEST_METHOD");

	if(strcmp(method, "GET") != 0) {
		util_exit(405);
	}

	size_t path_len = strlen(path);
	if(path_len < 1 || *path != '/') {
		util_exit(501);
	}

	--path_len;
	if(strcspn(path+1, ".<>;/\\ ") != path_len) {
		util_exit(400);
	}

	stat_channel(path+1);

	return 0;
}
