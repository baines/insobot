#ifndef INSOBOT_UTILS_H_
#define INSOBOT_UTILS_H_
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

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

static inline void snprintf_chain(char** bufp, size_t* sizep, const char* fmt, ...){
	va_list v;
	va_start(v, fmt);

	int printed = vsnprintf(*bufp, *sizep, fmt, v);

	if(printed > 0){
		*sizep -= printed;
		*bufp += printed;
	}

	va_end(v);
}

static inline char* tz_push(const char* tz){
	char* oldtz = getenv("TZ");
	if(oldtz) oldtz = strdup(oldtz);

	setenv("TZ", tz, 1);
	tzset();

	return oldtz;
}

static inline void tz_pop(char* oldtz){
	if(oldtz){
		setenv("TZ", oldtz, 1);
		free(oldtz);
	} else {
		unsetenv("TZ");
	}
	tzset();
}
#endif
