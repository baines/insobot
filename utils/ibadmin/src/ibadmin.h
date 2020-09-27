#ifndef IBADMIN_H_
#define IBADMIN_H_
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <dirent.h>
#include <syslog.h>
#include "stb_sb.h"

#define ADMIN_ROOT "/var/www/ibadmin/"
#define exit_error(c) ({ syslog(LOG_NOTICE, "Error %d: %s:%d", c, __func__, __LINE__); util_exit(c); })

#define DBG(fmt, ...) syslog(LOG_USER | LOG_ERR, fmt, ##__VA_ARGS__)

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

char* auth_cookie_create (const char* username, const char* password);
bool  auth_cookie_read   (struct auth_user* output);

typedef void (*route_handler)(void);

void handle_login  (void);
void handle_logout (void);
void handle_run    (void);
void handle_main   (void);
void handle_redir  (void);

char* xsprintf (const char* fmt, ...) __attribute__((format(printf, 1, 2)));

extern char _binary_login_html_start[];
extern char _binary_login_html_end[];

extern char _binary_main_html_start[];
extern char _binary_main_html_end[];

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

#define _auto_close_ __attribute__((cleanup(af_fclose)))
#define _auto_free_  __attribute__((cleanup(af_free)))

static inline void af_fclose(FILE** fp) {
	if(*fp) {
		fclose(*fp);
	}
}

static inline void af_free(void* p) {
	free(*(void**)p);
}

#endif
