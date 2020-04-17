#include "module.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include <inttypes.h>
#include <string.h>

static bool timer_init (const IRCCoreCtx*);
static void timer_cmd  (const char* chan, const char* name, const char* arg, int cmd);
static void timer_tick (time_t);
static bool timer_save (FILE*);

enum { TIMER_INFO, TIMER_ADD, TIMER_DEL, TIMER_LIST };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "timer",
	.desc     = "create timers",
	.on_init  = &timer_init,
	.on_cmd   = &timer_cmd,
	.on_tick  = &timer_tick,
	.on_save  = &timer_save,
	.flags    = IRC_MOD_GLOBAL,
	.commands = DEFINE_CMDS (
		[TIMER_INFO] = "!timer",
		[TIMER_ADD]  = "!timer+",
		[TIMER_DEL]  = "!timer-",
		[TIMER_LIST] = "!timers"
	),
	.cmd_help = DEFINE_CMDS (
		[TIMER_INFO] = "<id> | Shows info about a timer.",
		[TIMER_ADD]  = "<id> <expiry> [msg] | Adds a new countdown timer.",
		[TIMER_DEL]  = "<id> | Deletes a timer.",
		[TIMER_LIST] = "| Lists all the timer IDs for this channel."
	),
};

static const IRCCoreCtx* ctx;

struct timer {
	char* chan;
	char* id;
	char* msg;
	time_t expiry;
};

static sb(struct timer) timers;

static void timer_free(struct timer* t) {
	free(t->chan);
	free(t->id);
	free(t->msg);
	t->chan = t->id = t->msg = NULL;
	t->expiry = 0;
}

static void timer_load(void) {
	FILE* f = fopen(ctx->get_datafile(), "r");

	struct timer t = {};
	intmax_t expiry;
	time_t now = time(0);

	int fck = 0;
	while((fck = fscanf(f, "%ms %ms %" SCNiMAX " %m[^\n]", &t.chan, &t.id, &expiry, &t.msg)) >= 3) {
		if(expiry <= now) {
			timer_free(&t);
		} else {
			t.expiry = expiry;
			sb_push(timers, t);
		}
	}

	fclose(f);
}

static bool timer_save(FILE* f) {
	sb_each(t, timers) {
		intmax_t expiry = t->expiry;
		fprintf(f, "%s %s %" PRIiMAX " %s\n", t->chan, t->id, expiry, t->msg ?: "");
	}
	return true;
}

static struct timer* timer_get(const char* chan, const char* id) {
	sb_each(t, timers) {
		if(strcmp(chan, t->chan) == 0 && strcasecmp(t->id, id) == 0)
			return t;
	}
	return NULL;
}

static bool timer_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	timer_load();
	return true;
}

enum {
	ERR_NO_ID   = -1,
	ERR_BAD_FMT = -2,
};

static int timer_parse(char* buf, const char** id_out, const char** msg_out, time_t* expiry_out) {
	// <id> <expiry> <msg>

	const char* id = strsep(&buf, " ");
	if(!id)
		return ERR_NO_ID;

	const char* expiry = strsep(&buf, " ");
	if(!expiry)
		return ERR_BAD_FMT;

	unsigned long seconds = 0;
	unsigned long acc = 0;
	int next = -1;

	for(const char* c = expiry; *c; ++c) {
		if(*c >= '0' && *c <= '9') {
			acc *= 10;
			acc += *c - '0';
		} else if(next == -1 || *c == next){
			if(*c == 'h') {
				next = 'm';
				seconds += acc * 3600;
				acc = 0;
			} else if(*c == 'm') {
				next = 's';
				seconds += acc * 60;
				acc = 0;
			} else if(*c == 's') {
				break;
			} else if(*c == ':') {
				seconds = (seconds * 60) + acc;
				acc = 0;
				next = ':';
			}
		} else {
			return ERR_BAD_FMT;
		}
	}

	seconds += acc;
	*expiry_out = time(0) + seconds;
	*id_out = id;
	*msg_out = buf;

	return 0;
}

static void timer_cmd(const char* chan, const char* name, const char* arg, int cmd){

	const char* nick = inso_dispname(ctx, name);
	time_t now = time(0);

	switch(cmd){
		case TIMER_INFO: {
			if(!*arg++) {
				ctx->send_msg(chan, "%s: usage: !timer <id>", nick);
				break;
			}

			struct timer* t = timer_get(chan, arg);
			if(!t) {
				ctx->send_msg(chan, "%s: timer <%s> not found.", nick, arg);
				break;
			}

			char timebuf[64];
			char* p = timebuf;
			size_t sz = sizeof(timebuf);

			size_t diff = t->expiry - now;
			if(diff >= (60*60*24)) {
				snprintf_chain(&p, &sz, "%zud", diff / (60*60*24));
				diff %= (60*60*24);
			}

			if(diff >= (60*60)) {
				snprintf_chain(&p, &sz, "%zuh", diff / (60*60));
				diff %= (60*60);
			}

			if(diff >= 60) {
				snprintf_chain(&p, &sz, "%zum", diff / 60);
				diff %= 60;
			}

			snprintf_chain(&p, &sz, "%zus", diff);

			ctx->send_msg(chan, "%s: timer [%s] expires in %s with message %s.", nick, t->id, timebuf, t->msg ?: "(none)");
		} break;

		case TIMER_ADD: {
			if(!*arg++) {
				ctx->send_msg(chan, "%s: usage: !timer+ <id> <expiry> [message]", nick);
				break;
			}

			char* buf = strdupa(arg);

			const char* id;
			const char* msg;
			time_t expiry;

			int err = timer_parse(buf, &id, &msg, &expiry);

			if(err == ERR_NO_ID) {
				ctx->send_msg(chan, "%s: Missing ID.", nick);
				break;
			}

			if(err == ERR_BAD_FMT) {
				ctx->send_msg(chan, "%s: Couldn't parse expiry. Use e.g. \"5m\" or \"2m30s\".", nick);
				break;
			}

			struct timer* t = timer_get(chan, id);
			if(t) {
				timer_free(t);
			} else {
				t = sb_add(timers, 1);
			}

			t->chan = strdup(chan);
			t->id = strdup(id);
			t->msg = msg ? strdup(msg) : NULL;
			t->expiry = expiry;

			ctx->send_msg(chan, "%s: Timer %s created/updated.", nick, id);
			ctx->save_me();
		} break;

		case TIMER_DEL: {
			if(!*arg++) {
				ctx->send_msg(chan, "%s: usage: !timer- <id>", nick);
				break;
			}

			struct timer* t = timer_get(chan, arg);
			if(!t) {
				ctx->send_msg(chan, "%s: Unknown timer.", nick);
				break;
			}

			timer_free(t);
			sb_erase(timers, t - timers);

			ctx->send_msg(chan, "%s: timer deleted.", nick);
			ctx->save_me();
		} break;

		case TIMER_LIST: {
			char buf[512] = "";
			char* p = buf;
			size_t sz = sizeof(buf);

			sb_each(t, timers) {
				if(strcmp(t->chan, chan) != 0)
					continue;

				const char* fmt = t == timers ? "%s" : ", %s";
				snprintf_chain(&p, &sz, fmt, t->id);
			}

			if(!*buf)
				strcpy(buf, "(none)");

			ctx->send_msg(chan, "%s: timers: %s", nick, buf);
		} break;
	}
}

static void timer_tick(time_t now) {
	sb_each(t, timers) {
		if(now < t->expiry)
			continue;

		ctx->send_msg(t->chan, "⏰ Timer [%s] expired! %s", t->id, t->msg ?: "");

		sb_erase(timers, t - timers);
		--t;
	}
}
