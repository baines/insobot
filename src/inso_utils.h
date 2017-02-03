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

void   inso_curl_reset (void* curl, const char* url, char** data);
void*  inso_curl_init  (const char* url, char** data);

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
	char* buf = strdupa(path);
	char* prev_p = buf;
	char* p;

	while((p = strchr(prev_p, '/'))){
		*p = 0;
		if(access(buf, F_OK) != 0 && mkdir(buf, 00755) == -1){
			perror(__func__);
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

static inline void inso_permission_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
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

static inline void inso_dispname_cb(intptr_t result, intptr_t arg){
	// TODO: think of a good way to chain the mod msgs,
	// for example if more than one provides a display name.
	*(const char**)arg = (const char*)result;
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


#endif
