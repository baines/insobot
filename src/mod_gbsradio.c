#include "module.h"
#include "inso_utils.h"
#include "stb_sb.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

static bool gbs_init (const IRCCoreCtx*);
static void gbs_msg  (const char*, const char*, const char*);
static void gbs_tick (time_t);
static void gbs_load (void);
static void gbs_quit (void);

const IRCModuleCtx irc_mod_ctx = {
	.name        = "gbsradio",
	.desc        = "Listen to MC insobot's hot 8-bit mixtape",
	.on_init     = &gbs_init,
	.on_msg      = &gbs_msg,
	.on_tick     = &gbs_tick,
//	.on_modified = &gbs_load,
	.on_quit     = &gbs_quit,
	.help_url    = "https://github.com/baines/minigbs",
};

struct pkt_song {
	char name[32];
	int  track;
	int  votes;
};

struct pkt {
	struct pkt_song songs[3];
	int duration;
	int remaining;
};

struct file {
	char* filename;
	char title[32];
};

struct track {
	int file_idx;
	int trackno;
	int votes;
};

static const IRCCoreCtx* ctx;

static sb(struct file)  filelist;
static sb(struct track) tracklist;
static sb(int) clients;

static uint32_t tracklist_idx;
static uint32_t voter_hashes[128];
static int      hash_idx;

static time_t next_song_time;
static int    sock_minigbs;
static int    sock_listen;

#define PATH_INSOBOT "\0insobot_gbsradio"
static const struct sockaddr_un addr_insobot = {
	.sun_family = AF_UNIX,
	.sun_path = PATH_INSOBOT,
};

static bool gbs_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	gbs_load();

	sock_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if(sock_listen == -1){
		perror("socket (ib)");
		return false;
	}

	if(bind(sock_listen, &addr_insobot, sizeof(sa_family_t) + sizeof(PATH_INSOBOT) - 1) == -1){
		perror("bind");
		return false;
	}

	if(listen(sock_listen, 1) == -1){
		perror("listen");
		return false;
	}

	sock_minigbs = socket(AF_UNIX, SOCK_DGRAM, 0);
	if(sock_minigbs == -1){
		perror("socket (mgbs)");
		return false;
	}

	return true;
}

static uint32_t simple_hash(const char* str, size_t len){
	uint32_t hash = 6159;
	for(size_t i = 0; i < len; ++i){
		hash = hash * 187 + str[i];
	}
	return hash;
}

static void gbs_msg(const char* chan, const char* nick, const char* msg){

	const char *p, *prev_p = msg;
	uint32_t hash = simple_hash(nick, strlen(nick));

	for(int i = 0; i < 128; ++i){
		if(voter_hashes[i] == hash){
			printf("%s already voted\n", nick);
			return;
		}
	}

	while((p = strchr(prev_p, '#'))){
		if(p[1] >= '0' && p[1] <= '2'){
			printf("%s voted %c\n", nick, p[1]);
			struct track* t = tracklist + tracklist_idx + (p[1] - '0');
			t->votes++;
			voter_hashes[hash_idx] = hash;
			hash_idx = (hash_idx + 1) % 128;
			break;
		}
		prev_p = p + 1;
	}
}

#if 0
static intptr_t enabled_cb(intptr_t result, bool* found){
	*found = result;
	return 0;
}
#endif

static void tracks_shuffle(void){
	for(int i = sb_count(tracklist) - 1; i --> 1 ;){
		int j = rand() % (i+1);

		struct track tmp = tracklist[j];
		tracklist[j] = tracklist[i];
		tracklist[i] = tmp;
	}
}

#define DURATION 75

static void gbs_tune(struct track* t){
	struct file* f = filelist + t->file_idx;

	printf("Tuning to track [%s:%d]\n", f->title, t->trackno);

#define PATH_CTRL  "\0minigbs_ctrl"

	static struct sockaddr_un ctrl_addr = {
		.sun_family = AF_UNIX,
		.sun_path = PATH_CTRL,
	};

	char buf[4096];
	snprintf(buf, sizeof(buf), "%d %s", t->trackno, f->filename);
	sendto(sock_minigbs, buf, strlen(buf) + 1, 0, &ctrl_addr, sizeof(sa_family_t) + sizeof(PATH_CTRL) - 1);


#if 0
	const char** chans = ctx->get_channels();
	for(const char** c = chans; *c; ++c){
		bool found = false;
		MOD_MSG(ctx, "core_enabled", *c, &enabled_cb, &found);
		if(found){
			ctx->send_msg(*c, "Selected song: [%s] Track %02d.", f->title, t->trackno);
		}
	}
#endif
	//ctx->send_msg("#insofaras", "Selected song: [%s] Track %02d.", f->title, t->trackno);

	for(int i = 0; i < 3; ++i){
		struct track* t = tracklist + tracklist_idx + i;
		t->votes = 0;
	}

	tracklist_idx += 3;

	if(tracklist_idx >= sb_count(tracklist)){
		tracks_shuffle();
		tracklist_idx = 0;
	}

	next_song_time = time(0) + DURATION;
	memset(voter_hashes, 0, sizeof(voter_hashes));
}

static void gbs_tick(time_t now){
	if(sb_count(tracklist) < 3)
		return;

	int cfd;
	while((cfd = accept(sock_listen, NULL, NULL)) != -1){
		printf("new client: %d\n", cfd);
		sb_push(clients, cfd);
	}

	struct pkt pkt = {};
	pkt.duration = DURATION;
	pkt.remaining = INSO_MAX(0, next_song_time - now);

	if(now > next_song_time){
		struct track* t = tracklist + tracklist_idx;
		struct track* maxt = t + rand() % 3;

		for(int i = 0; i < 3; ++i){
			int maxv = maxt->votes;

			if(t[i].votes > maxv || (t[i].votes == maxv && (rand() % 2) == 0)){
				maxt = t + i;
			}
		}

		gbs_tune(maxt);
	}

	for(int i = 0; i < 3; ++i){
		struct track* t = tracklist + tracklist_idx + i;

		pkt.songs[i] = (struct pkt_song){
			.track = t->trackno,
			.votes = t->votes,
		};

		struct file* f = filelist + t->file_idx;
		strncpy(pkt.songs[i].name, f->title, 32);
	}

	for(size_t i = 0; i < sb_count(clients); ++i){
		int fd = clients[i];

		if(send(fd, &pkt, sizeof(pkt), 0) == -1){
			printf("rm bad client: %d\n", fd);
			sb_erase(clients, i);
			--i;
		}
	}
}

static void gbs_load(void){
	FILE* f = fopen(ctx->get_datafile(), "r");

	sb_free(tracklist);
	sb_each(f, filelist)
		free(f->filename);
	sb_free(filelist);

	char line[2048];
	while(fgets(line, sizeof(line), f)){
		char* state;

		char* title    = strtok_r(line, "|\r\n", &state);
		char* tracks   = strtok_r(NULL, "|\r\n", &state);
		char* filename = strtok_r(NULL, "|\r\n", &state);

		if(!title || !tracks || !filename)
			continue;

		char filename2[4096];
		snprintf(filename2, sizeof(filename2), "/home/alex/Music/gbsradio/%s", filename);
		char* filepath = canonicalize_file_name(filename2);
		if(!filepath)
			continue;

		struct file file = { .filename = filepath };
		strncpy(file.title, title, 32);
		sb_push(filelist, file);

		for(char* t = strtok_r(tracks, ",", &state); t; t = strtok_r(NULL, ",", &state)){
			int n = atoi(t);

			struct track track = {
				.file_idx = sb_count(filelist) - 1,
				.trackno = n,
			};
			sb_push(tracklist, track);
		}
	}

	printf("loaded %zu tracks\n", sb_count(tracklist));

	tracks_shuffle();

	fclose(f);
}

static void gbs_quit(void){
	close(sock_listen);
	close(sock_minigbs);

	sb_each(c, clients){
		close(*c);
	}
	sb_free(clients);

	sb_each(f, filelist){
		free(f->filename);
	}
	sb_free(filelist);
	sb_free(tracklist);
}
