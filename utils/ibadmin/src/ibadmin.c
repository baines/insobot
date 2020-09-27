#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <argz.h>
#include "ibadmin.h"

struct route {
	const char* method;
	const char* path;
	route_handler handler;
} routes[] = {
	{ "POST", "/login" , &handle_login },
	{ "POST", "/logout", &handle_logout },
	{ "POST", "/run"   , &handle_run },
	{ "GET" , "/"      , &handle_main },
	{ "GET" , "/login" , &handle_redir },
};

void handle_login (void)
{
	char ubuf[256] = "";
	char pbuf[256] = "";

	fgets(ubuf, sizeof(ubuf), stdin);
	fgets(pbuf, sizeof(pbuf), stdin);

	ubuf[strcspn(ubuf, "\r\n")] = '\0';
	pbuf[strcspn(pbuf, "\r\n")] = '\0';

	if(memcmp(ubuf, "user=", 5) != 0 || memcmp(pbuf, "pass=", 5) != 0) {
		printf("Status: 400 Bad Request\r\n");
		util_output(GETBIN(login_html), RESPONSE_HTML, 0);
		printf("<center>Invalid login</center>\n");
		puts("SHIET");
		return;
	}

	const char* user = ubuf + 5;
	const char* pass = pbuf + 5;

	char* cookie = auth_cookie_create(user, pass);
	if(!cookie) {
		printf("Status: 401 Unauthorized\r\n");
		util_output(GETBIN(login_html), RESPONSE_HTML, 0);
		printf("<center>Invalid login</center>\n");
		return;
	}

	printf("Status: 303 See Other\r\n");
	printf("Location: ./\r\n");
	printf("Set-Cookie: %s; HttpOnly; SameSite=Lax\r\n", cookie);
	printf("\r\n");
}

void handle_redir (void)
{
	printf("Status: 303 See Other\r\n");
	printf("Location: ./\r\n");
	printf("\r\n");
}

void handle_logout (void)
{
	printf("Status: 303 See Other\r\n");
	printf("Location: ./\r\n");
	printf("Set-Cookie: AUTH=; HttpOnly; SameSite=Lax; Max-Age=1\r\n");
	printf("\r\n");
}

#define PATH_INSOBOT "\0insobot_admin"
static const struct sockaddr_un addr_insobot = {
	.sun_family = AF_UNIX,
	.sun_path = PATH_INSOBOT,
};

void handle_run (void)
{
	char chan[64] = "";
	char cmd [510 - 64] = "";

	static const struct timeval tv = {
		.tv_sec = 8,
	};

	struct auth_user u;
	if(auth_cookie_read(&u)) {
		int fd;

		if((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
			util_exit(511);
		}

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		if(connect(fd, &addr_insobot, sizeof(sa_family_t) + sizeof(PATH_INSOBOT) - 1) == -1) {
			util_exit(522);
		}

		fgets(chan, sizeof(chan), stdin);
		fgets(cmd , sizeof(cmd), stdin);

		chan[strcspn(chan, "\r\n")] = '\0';
		cmd[strcspn(cmd, "\r\n")] = '\0';

		char* ent = NULL;
		bool found_chan = false;

		while((ent = argz_next(u.chan_argz, u.nchans, ent))) {
			if(strcmp(ent, chan) == 0) {
				found_chan = true;
				break;
			}
		}

		if(!found_chan) {
			util_exit(401);
		}

		char buf[512];
		snprintf(buf, sizeof(buf), "%s %s %s", u.user, chan, cmd);

		send(fd, buf, strlen(buf), 0);

		printf("Status: 200 OK\r\n");
		printf("Content-Type: text/plain\r\n");
		printf("\r\n");

		ssize_t n;
		while((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
			printf("%.*s\n", (int)n, buf);
		}

	} else {
		util_exit(401);
	}
}

void handle_main (void)
{
	printf("Status: 200 OK\r\n");

	struct auth_user u = {};
	if(auth_cookie_read(&u)) {

		sb(char) channel_html = NULL;

		char* ent = NULL;
		while((ent = argz_next(u.chan_argz, u.nchans, ent))) {
			const char* subst[] = {
				"chan", ent,
				NULL,
			};
			const char template[] = "\t\t<option>`chan|h`</option>\n";
			template_append(&channel_html, template, sizeof(template)-1, subst);
		}

		const char* subst[] = {
			"chans", channel_html,
			NULL,
		};

		template_puts(GETBIN(main_html), subst, RESPONSE_HTML, 0);
	} else {
		util_output(GETBIN(login_html), RESPONSE_HTML, 0);
	}
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
