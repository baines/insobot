#include <time.h>
#include <stdlib.h>
#include "module.h"
#include "utils.h"

static void notes_msg     (const char*, const char*, const char*);
static void notes_msg_out (const char*, const char*);
static bool notes_init    (const IRCCoreCtx*);
static void notes_mod_msg (const char*, const IRCModMsg*);
static void notes_ipc     (int, const uint8_t*, size_t);

const IRCModuleCtx irc_mod_ctx = {
	.name       = "notes",
	.desc       = "Records notes",
	.flags      = IRC_MOD_GLOBAL,
	.on_msg     = &notes_msg,
	.on_msg_out = &notes_msg_out,
	.on_init    = &notes_init,
	.on_mod_msg = &notes_mod_msg,
	.on_ipc     = &notes_ipc
};

enum { NOTE_NONE, NOTE_STREAM_START, NOTE_GENERIC };

typedef struct Note {
	int    type;
	time_t time;
	char*  channel;
	char*  author;
	char*  content;
} Note;

static Note notes[256];
static int note_index;

static void note_push(Note* new_note){

	Note* n = notes + note_index;
	if(n->type != NOTE_NONE){
		free(n->channel);
		free(n->content);
		free(n->author);
	}

	*n = *new_note;

	note_index = (note_index + 1) % ARRAY_SIZE(notes);
}

static const IRCCoreCtx* ctx;

static bool notes_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void notes_msg(const char* chan, const char* name, const char* msg){
	if(!inso_is_wlist(ctx, name)) return;

	// TODO: handle more types of notes
	if(strncasecmp(msg, "note(annotator): ", 17) == 0 && strcasestr(msg, "start")){

		Note n = {
			.type    = NOTE_STREAM_START,
			.time    = time(0),
			.channel = strdup(chan),
			.author  = strdup(name),
			.content = strdup(msg),
		};

		note_push(&n);

		char* ipc_msg;
		int len = asprintf(&ipc_msg, "%d %ld %s %s %s\n", n.type, n.time, n.channel, n.author, n.content);
		if(len != -1){
			ctx->send_ipc(0, ipc_msg, len + 1);
			free(ipc_msg);
		}
	}
}

static void notes_ipc(int target, const uint8_t* data, size_t data_len){
	if(!memchr(data, 0, data_len)) return;
	Note n = {};

	if(sscanf(data, "%d %ld %ms %ms %m[^\n]", &n.type, &n.time, &n.channel, &n.author, &n.content) == 5){
		puts("got note via ipc");
		note_push(&n);
	} else {
		free(n.channel);
		free(n.author);
		free(n.content);
	}
}

static void notes_msg_out(const char* chan, const char* msg){
	notes_msg(chan, ctx->get_username(), msg);
}

static void notes_mod_msg(const char* sender, const IRCModMsg* msg){

	if(strcmp(msg->cmd, "note_get_stream_start") == 0){
		const char* chans = (const char*) msg->arg;
		const char* chan_start = chans;
		const char* chan_end;
		time_t time = 0;

		do {
			chan_end = strchrnul(chan_start, ' ');
			const char* chan = strndupa(chan_start, chan_end - chan_start);

			for(int i = 0; i < ARRAY_SIZE(notes); ++i){
				if(notes[i].type != NOTE_STREAM_START) continue;
				if(strcmp(notes[i].channel, chan) != 0) continue;

				if(notes[i].time > time){
					time = notes[i].time;
				}
			}

			chan_start = chan_end + 1;

		} while(*chan_end);

		if(time){
			msg->callback(time, msg->cb_arg);
		}
	}
}
