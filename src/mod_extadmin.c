#include "module.h"
#include "inso_utils.h"
#include "stb_sb.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

static bool extadmin_init   (const IRCCoreCtx*);
static void extadmin_tick   (time_t now);
static void extadmin_quit   (void);
static void extadmin_filter (size_t, const char*, char*, size_t);

const IRCModuleCtx irc_mod_ctx = {
	.name      = "extadmin",
	.desc      = "external admin",
	.on_init   = &extadmin_init,
	.on_quit   = &extadmin_quit,
	.on_tick   = &extadmin_tick,
	.on_filter = &extadmin_filter,
};

static const IRCCoreCtx* ctx;
static int sock;

struct client {
	int fd;
	sb(size_t) filter_ids;
	time_t connected_at;
};

static sb(struct client) clients;

#define PATH_INSOBOT "\0insobot_admin"
static const struct sockaddr_un addr_insobot = {
	.sun_family = AF_UNIX,
	.sun_path = PATH_INSOBOT,
};

static bool extadmin_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if(sock == -1){
		perror("socket (ib)");
		return false;
	}

	if(bind(sock, &addr_insobot, sizeof(sa_family_t) + sizeof(PATH_INSOBOT) - 1) == -1){
		perror("bind");
		return false;
	}

	if(listen(sock, 10) == -1){
		perror("listen");
		return false;
	}

	return true;
}

static void run_command(struct client* c, char* buf, size_t n) {
	char chan[64];
	char name[64];
	int off = 0;

	if(sscanf(buf, "%63s %63s %n", name, chan, &off) == 2 && off) {
		printf("running admin command: %s/%s/%s\n", chan, name, buf + off);

		size_t cmd_id_before = ctx->get_info(IRC_INFO_NEXT_CMD_ID);

		ctx->gen_event(IRC_CB_MSG, chan, name, buf + off);

		size_t cmd_id_after = ctx->get_info(IRC_INFO_NEXT_CMD_ID);

		while(cmd_id_before < cmd_id_after) {
			sb_push(c->filter_ids, cmd_id_before);
			++cmd_id_before;
		}
	}
}

static void extadmin_tick(time_t now) {

	int cfd;
	while((cfd = accept4(sock, NULL, NULL, SOCK_NONBLOCK)) != -1){
		printf("new client: %d\n", cfd);

		struct client c = {
			.fd = cfd,
			.connected_at = now
		};

		sb_push(clients, c);
	}

	sb_each(c, clients) {
		char buf[512];
		ssize_t n = recv(c->fd, buf, sizeof(buf)-1, 0);

		if(n == -1) {
			if(errno != EAGAIN) {
				close(c->fd);
				sb_erase(clients, c - clients);
				--c;
			}
			continue;
		}

		buf[n] = '\0';

		if(n > 0 && buf[n-1] == '\n') {
			buf[n-1] = '\0';
		}

		run_command(c, buf, n);
	}
}

static void extadmin_quit(void) {
	close(sock);

	sb_each(c, clients) {
		close(c->fd);
	}

	sb_free(clients);
}

static void extadmin_filter (size_t id, const char* chan, char* msg, size_t msg_len) {
	char* tmp = alloca(msg_len+2);
	memcpy(tmp, msg, msg_len);
	tmp[msg_len] = '\0';

	ctx->strip_colors(tmp);
	msg_len = strlen(tmp);
	tmp[msg_len+0] = '\n';
	tmp[msg_len+1] = '\0';

	sb_each(c, clients) {
		if(sb_count(c->filter_ids) == 0) {
			continue;
		}

		sb_each(i, c->filter_ids) {
			if(id != *i)
				continue;

			send(c->fd, tmp, msg_len+1, 0);
			*msg = 0;

			sb_erase(c->filter_ids, i - c->filter_ids);
			break;
		}

		if(sb_count(c->filter_ids) == 0) {
			close(c->fd);
			sb_erase(clients, c - clients);
			--c;
		}
	}
}
