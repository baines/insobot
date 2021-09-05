#ifndef IB_STAT_H_
#define IB_STAT_H_
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <argz.h>
#include <syslog.h>
#include "stb_sb.h"

//#define INSOBOT_DATA_PATH "/somewhere/insobot/data"

#define countof(x) (sizeof(x)/sizeof(*x))
#define exit_error(c) ({ syslog(LOG_NOTICE, "Error %d: %s:%d", c, __func__, __LINE__); util_exit(c); })

sb(char) template_bake   (const char* data, size_t, const char** subst);
void     template_append (sb(char)* out, const char* data, size_t len, const char** subst);
void     template_puts   (const char* data, size_t len, const char** subst, int type, time_t mod);

void escape_html (sb(char)* out, const char* in);
void escape_json (sb(char)* out, const char* in);
void escape_csv  (sb(char)* out, const char* in);

void         util_exit    (int http_code);
const char*  util_getenv  (const char* name);
void         util_output  (const char* data, size_t len, int type, time_t mod);
void         util_headers (int type, time_t mod);

extern char _binary_main_html_start[];
extern char _binary_main_html_end[];

enum {
	RESPONSE_HTML,
	RESPONSE_CSV,
	RESPONSE_JSON,
	RESPONSE_RAW,
};

#define GETBIN(name) (const char*)_binary_##name##_start, (size_t)((char*)(&_binary_##name##_end) - (char*)(&_binary_##name##_start))

#endif
