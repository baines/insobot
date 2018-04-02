#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <yajl/yajl_tree.h>

#define SCHEDULE_FILE "/var/www/schedule.json"

void util_exit(int http_code){
	const char* msg = "Internal Server Error";

	switch(http_code){
		case 400: msg = "Bad Request"; break;
		case 403: msg = "Unauthorized"; break;
		case 404: msg = "Not Found"; break;
	}

	printf("Status: %d %s\r\n\r\n", http_code, msg);
	fflush(stdout);
	exit(0);
}

const char* util_getenv(const char* name){
	const char* result = getenv(name);
	if (!result) util_exit(400);
	return result;
}

void check_auth(void){
	const char* auth_recvd = util_getenv("HTTP_AUTHORIZATION");
	const char* auth_legit = util_getenv("SCHEDULE_AUTH");
	char buf[256];

	if(sscanf(auth_recvd, "Basic %255s", buf) == 1){
		if(strcmp(buf, auth_legit) != 0){
			util_exit(403);
		}
	} else {
		util_exit(400);
	}
}

int main(void){
	check_auth();

	if(strcmp(util_getenv("REQUEST_METHOD"), "POST") != 0){
		util_exit(400);
	}

	int len = atoi(util_getenv("CONTENT_LENGTH"));
	if(len <= 0 || len >= (1 << 20)){
		util_exit(400);
	}

	char* buf = malloc(len + 1);
	if(!buf || fread(buf, 1, len, stdin) != len){
		util_exit(500);
	}
	buf[len] = 0;

	if(!yajl_tree_parse(buf, NULL, 0)){
		util_exit(400);
	}

	int fd = open(SCHEDULE_FILE, O_RDWR);
	if(fd == -1){
		util_exit(500);
	}

	flock(fd, LOCK_EX);
	ftruncate(fd, 0);
	pwrite(fd, buf, len, 0);
	fdatasync(fd);
	close(fd);

	puts("Status: 204 No Content\r\n\r");
	fflush(stdout);

	return 0;
}
