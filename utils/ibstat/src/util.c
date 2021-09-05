#include "ibstat.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>

void util_exit(int http_code){
	const char* msg = "Internal Server Error";

	switch(http_code){
		case 400:
			msg = "Bad Request"; break;
		case 401:
			msg = "Unauthorized"; break;
		case 403:
			msg = "Forbidden"; break;
		case 404:
			msg = "Not Found"; break;
		case 405:
			msg = "Method Not Allowed"; break;
		case 406:
			msg = "Not Acceptable"; break;
	}

	char errbuf[8] = "";
	snprintf(errbuf, sizeof(errbuf), "%d", http_code);

	printf("Status: %d %s\r\n", http_code, msg);
	util_output("", 0, RESPONSE_HTML, 0);
	fflush(stdout);
	exit(0);
}

const char* util_getenv(const char* name){
	const char* result = getenv(name);
	if (!result) util_exit(400);
	return result;
}

static const char* response_mime[] = {
	[RESPONSE_HTML] = "text/html",
	[RESPONSE_JSON] = "application/json",
	[RESPONSE_CSV]  = "text/csv",
	[RESPONSE_RAW]  = "text/plain",
};

void util_headers(int type, time_t mod){
	printf("Content-Type: %s; charset=utf-8\r\n", response_mime[type]);

	if(mod){
		char time_buf[256] = "";
		struct tm tm = {};
		gmtime_r(&mod, &tm);
		strftime(time_buf, sizeof(time_buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);

		if(*time_buf){
			printf("Last-Modified: %s\r\n", time_buf);
		}
	}

	printf("\r\n");
}

void util_output(const char* data, size_t len, int type, time_t mod){
	util_headers(type, mod);
	printf("%.*s\n", (int)len, data);
}

__attribute__((format(printf, 1, 2)))
char* xsprintf(const char* fmt, ...) {
	va_list va;
	va_start(va, fmt);

	char* ret = NULL;
	if(vasprintf(&ret, fmt, va) == -1) {
		util_exit(500);
	}

	va_end(va);

	return ret;
}
