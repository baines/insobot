#if !defined(INSO_UTILS_H_) && !defined(INSO_IMPL)
#define INSO_UTILS_H_
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include "module.h"

#define INSO_MIN(x, y)({ \
	typeof(x) _x = (x);  \
	typeof(y) _y = (y);  \
	_x <= _y ? _x : _y;  \
})

#define INSO_MAX(x, y)({ \
	typeof(x) _x = (x);  \
	typeof(y) _y = (y);  \
	_x > _y ? _x : _y;   \
})

#define ARRAY_SIZE(x)({                                          \
	_Static_assert(                                              \
		!__builtin_types_compatible_p(typeof(x), typeof(&x[0])), \
		"!!!! ARRAY_SIZE used on a pointer !!!!"                 \
	);                                                           \
	sizeof(x) / sizeof(*x);                                      \
})

#define asprintf_check(...) ({       \
	if(asprintf(__VA_ARGS__) == -1){ \
		perror("asprintf");          \
		assert(0);                   \
	}                                \
})

#define isizeof(x) ((int)(sizeof(x)))

#define array_each(p, arr) for(typeof(*arr)* p = arr; p < arr + ARRAY_SIZE(arr); ++p)

#define ib_assert(cond) ({ \
	bool __ib_cond_res = (cond); \
	if(!__ib_cond_res){ \
		fprintf(stderr, "%s:%d: %s: Assertion '%s' failed.\n", __FILE__, __LINE__, __func__, #cond); \
		const char* dbgchan = getenv("INSOBOT_DEBUG_CHAN"); \
		if(dbgchan){ \
			ctx->send_msg(dbgchan, "%s:%d: %s: Assertion '%s' failed.", __FILE__, __LINE__, __func__, #cond); \
		} \
	} \
	__ib_cond_res; \
})

void   inso_curl_reset   (void* curl, const char* url, char** data);
void*  inso_curl_init    (const char* url, char** data);
long   inso_curl_perform (void* curl, char** data);

// returns num bytes copied, or -(num reuired) and doesn't copy anything if not enough space.
static inline int inso_strcat(char* buf, size_t sz, const char* str){
	char* p = buf;
	while(*p) ++p;

	sz -= (p - buf);

	size_t len = strlen(str) + 1;

	if(len <= sz){
		memcpy(p, str, len);
		return len;
	} else {
		return sz - len;
	}
}

static inline bool inso_mkdir_p(const char* path){
	char* buf = alloca(strlen(path)+2);
	char* p   = stpcpy(buf, path);

	if(p > buf && p[-1] != '/'){
		p[0] = '/';
		p[1] = '\0';
	}

	char* prev_p = buf;
	while((p = strchr(prev_p, '/'))){
		*p = 0;
		if(*buf && access(buf, F_OK) != 0 && mkdir(buf, 00755) == -1){
			fprintf(stderr, "%s: error: %m (buf=[%s])\n", __func__, buf);
			return false;
		}
		*p = '/';
		prev_p = p+1;
	}

	return true;
}

static inline void snprintf_chain(char** bufp, size_t* sizep, const char* fmt, ...){
	va_list v;
	va_start(v, fmt);

	int printed = vsnprintf(*bufp, *sizep, fmt, v);

	if(printed > 0 && (size_t)printed <= *sizep){
		*sizep -= printed;
		*bufp += printed;
	}

	va_end(v);
}

static inline bool inso_in_chan(const IRCCoreCtx* ctx, const char* chan){
	const char** list = ctx->get_channels();
	while(*list){
		if(strcasecmp(*list, chan) == 0) return true;
		++list;
	}
	return false;
}

static inline int inso_match_cmd(const char* msg, const char* cmd, bool skip_control){

	size_t len_adjust = 0;
	if(skip_control && strchr(CONTROL_CHAR CONTROL_CHAR_2, *msg)){
		++msg;
		++len_adjust;
	}

	do {
		if(skip_control){
			if(strncmp(cmd, CONTROL_CHAR, sizeof(CONTROL_CHAR)-1) == 0){
				cmd += sizeof(CONTROL_CHAR)-1;
			} else if(strncmp(cmd, CONTROL_CHAR_2, sizeof(CONTROL_CHAR_2)-1) == 0){
				cmd += sizeof(CONTROL_CHAR_2)-1;
			}
		}

		const char* cmd_end = strchrnul(cmd, ' ');
		const size_t sz = cmd_end - cmd;

		if(strncasecmp(msg, cmd, sz) == 0 && (msg[sz] == ' ' || msg[sz] == '\0')){
			return sz + len_adjust;
		}

		while(*cmd_end == ' '){
			++cmd_end;
		}

		cmd = cmd_end;
	} while(*cmd);

	return -1;
}

static inline void time_diff_string(time_t start, time_t end, char* buf, size_t buf_sz){

	time_t time_diff = end - start;

	static const struct {
		const char* unit;
		int limit;
		int divisor;
	} time_info[] = {
		{ "s", 60, 1},
		{ "m", (60*60), 60 },
		{ "h", (60*60*24), (60*60) },
		{ "d", (60*60*24*365), (60*60*24) }
	};

	for(size_t i = 0; i < ARRAY_SIZE(time_info); ++i){
		if(time_diff < time_info[i].limit){
			snprintf(buf, buf_sz, "%d%s ago", (int)time_diff / time_info[i].divisor, time_info[i].unit);
			break;
		}
	}

	if(!*buf){
		struct tm tm = {};
		gmtime_r(&start, &tm);
		strftime(buf, buf_sz, "%F", &tm);
	}
}

static inline intptr_t inso_permission_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
	return 0;
}

static inline bool inso_is_wlist(const IRCCoreCtx* ctx, const char* name){
	bool result = false;
	MOD_MSG(ctx, "check_whitelist", name, &inso_permission_cb, &result);
	return result;
}

static inline bool inso_is_admin(const IRCCoreCtx* ctx, const char* name){
	bool result = false;
	MOD_MSG(ctx, "check_admin", name, &inso_permission_cb, &result);
	return result;
}

static inline intptr_t inso_dispname_cb(intptr_t result, intptr_t arg){
	// TODO: think of a good way to chain the mod msgs,
	// for example if more than one provides a display name.
	*(const char**)arg = (const char*)result;
	return 0;
}

// XXX: taking a char* param is a bit misleading since this will only work
//      on the name for the current message.
static inline const char* inso_dispname(const IRCCoreCtx* ctx, const char* fallback){
	const char* result = fallback;
	MOD_MSG(ctx, "display_name", fallback, &inso_dispname_cb, &result);
	return result;
}

#endif

// implementation

#ifdef INSO_IMPL

#include "stb_sb.h"
#include <string.h>
#include <curl/curl.h>

size_t inso_curl_callback(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = (char**)data;
	const size_t total = sz * nmemb;

	memcpy(sb_add(*out, total), ptr, total);

	return total;
}

void inso_curl_reset(CURL* curl, const char* url, char** data){
	curl_easy_reset(curl);

	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &inso_curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8);
}

CURL* inso_curl_init(const char* url, char** data){
	CURL* curl = curl_easy_init();
	inso_curl_reset(curl, url, data);
	return curl;
}

long inso_curl_perform(CURL* curl, char** data){
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


#endif
