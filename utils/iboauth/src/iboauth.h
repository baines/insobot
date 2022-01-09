#ifndef IBOAUTH_H_
#define IBOAUTH_H_
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <dirent.h>
#include <syslog.h>
#include "stb_sb.h"

#define exit_error(c) ({ syslog(LOG_NOTICE, "Error %d: %s:%d", c, __func__, __LINE__); util_exit(c); })
#define DBG(fmt, ...) syslog(LOG_USER | LOG_ERR, fmt, ##__VA_ARGS__)
#define HASHLEN 32

#define TWITCH_DATA_FILE "/home/alex/prj/insobot/data/twitch.data"

struct auth_user {
    char* user;
    char* chan_argz;
    size_t nchans;
};

void         util_exit    (int http_code);
const char*  util_getenv  (const char* name);
void         util_output  (const char* data, size_t len, int type, time_t mod);
void         util_headers (int type, time_t mod);

sb(char) template_bake   (const char* data, size_t, const char** subst);
void     template_append (sb(char)* out, const char* data, size_t len, const char** subst);
void     template_puts   (const char* data, size_t len, const char** subst, int type, time_t mod);

void escape_html (sb(char)* out, const char* in);
void escape_json (sb(char)* out, const char* in);
void escape_csv  (sb(char)* out, const char* in);

void sha256 (uint8_t hash[static HASHLEN], const void* input, size_t len);

char* unbase64(const char* in, size_t len);

typedef void (*route_handler)(void);

extern char _binary_linked_html_start[];
extern char _binary_linked_html_end[];

enum {
	RESPONSE_HTML,
	RESPONSE_CSV,
	RESPONSE_JSON,
	RESPONSE_RAW,
};

#define GETBIN(name) (const char*)_binary_##name##_start, (size_t)((char*)(&_binary_##name##_end) - (char*)(&_binary_##name##_start))

#define countof(x)({                                          \
	_Static_assert(                                              \
		!__builtin_types_compatible_p(typeof(x), typeof(&x[0])), \
		"!!!! ARRAY_SIZE used on a pointer !!!!"                 \
	);                                                           \
	sizeof(x) / sizeof(*x);                                      \
})

#endif
