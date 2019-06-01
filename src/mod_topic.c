#include <ctype.h>
#include "module.h"
#include "module_msgs.h"
#include "inso_utils.h"
#include "stb_sb.h"

static bool topic_init    (const IRCCoreCtx*);
static void topic_cmd     (const char* chan, const char* name, const char* arg, int cmd);
static void topic_msg     (const char* chan, const char* name, const char* msg);
static void topic_tick    (time_t);
static bool topic_save    (FILE*);
static void topic_mod_msg (const char*, const IRCModMsg*);

enum { TOPIC_SAY, TOPIC_SET, TOPIC_CLEAR };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "topic",
	.desc       = "Maintains stream topics",
	.on_init    = &topic_init,
	.on_cmd     = &topic_cmd,
	.on_msg     = &topic_msg,
	.on_tick    = &topic_tick,
	.on_save    = &topic_save,
	.on_mod_msg = &topic_mod_msg,
	.commands = DEFINE_CMDS (
		[TOPIC_SAY]   = CMD("topic"),
		[TOPIC_SET]   = CMD("settopic") CMD("topic+"),
		[TOPIC_CLEAR] = CMD("cleartopic") CMD("clrtopic") CMD("clt") CMD("topic-")
	),
	.cmd_help = DEFINE_CMDS (
		[TOPIC_SAY]   = "| Recalls today's topic.",
		[TOPIC_SET]   = "| Sets today's topic.",
		[TOPIC_CLEAR] = "| Clears today's topic."
	)
};

static const IRCCoreCtx* ctx;

struct chan {
	char   name[64];
	char   topic[512];
	time_t ask_time;
	bool   waiting;
};

static sb(struct chan) topic_chans;

static void topic_load(void){
	sb_free(topic_chans);

	FILE* f = fopen(ctx->get_datafile(), "r");

	struct chan c;
	unsigned long tmp_time;
	time_t now = time(0);

	while(fscanf(f, "%63s %lu %511[^\n]\n", c.name, &tmp_time, c.topic) == 3){
		if(now - tmp_time > (20*60*60))
			continue;

		c.ask_time = tmp_time;
		sb_push(topic_chans, c);
	}

	fclose(f);
}

static bool topic_save(FILE* f){
	sb_each(c, topic_chans){
		fprintf(f, "%s %lu %s\n", c->name, c->ask_time, c->topic);
	}
	return true;
}

static struct chan* topic_lookup_or_create(const char* chan){
	sb_each(c, topic_chans){
		if(strcmp(chan, c->name) == 0){
			return c;
		}
	}

	struct chan c = {};
	*stpncpy(c.name, chan, sizeof(c.name)-1) = '\0';
	sb_push(topic_chans, c);

	return &sb_last(topic_chans);
}

static bool topic_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	topic_load();
	return true;
}

static void topic_msg(const char* chan, const char* name, const char* msg){
	if(*msg == '@')
		++msg;

	const char* bot_name = ctx->get_username();
	size_t bot_name_len = strlen(bot_name);
	time_t now = time(0);

	if(strncmp(msg, bot_name, bot_name_len) == 0 && !isalpha(msg[bot_name_len])){
		size_t off = strspn(msg + bot_name_len, " ,:@.-");
		const char* topic = msg + bot_name_len + off;

		struct chan* c = topic_lookup_or_create(chan);

		if(*topic && c->waiting){
			if(now - c->ask_time < 4*60){
				*stpncpy(c->topic, topic, sizeof(c->topic)-1) = '\0';
				c->waiting = false;
				ctx->send_msg(chan, "%s: thanks.", inso_dispname(ctx, name));
			}
		}
	}
}

static void topic_cmd(const char* chan, const char* name, const char* arg, int cmd){
	time_t now = time(0);
	struct chan* c = topic_lookup_or_create(chan);

	if(cmd == TOPIC_SAY){
		if(c->ask_time && (now - c->ask_time > 12*60*60)){
			memset(c->topic, 0, sizeof(c->topic));
			c->ask_time = 0;
		}

		if(*c->topic){
			ctx->send_msg(chan, "@%s: Today's topic: %s", inso_dispname(ctx, name), c->topic);
		} else {
			ctx->send_msg(chan, "@%s: The topic isn't set, what should it be?", inso_dispname(ctx, name));
			c->ask_time = time(0);
			c->waiting = true;
		}
	} else {
		if(!inso_is_wlist(ctx, name))
			return;

		if(cmd == TOPIC_SET){
			if(*arg++){
				*stpncpy(c->topic, arg, sizeof(c->topic)-1) = '\0';
				ctx->send_msg(chan, "%s: topic set.", name);
			} else {
				ctx->send_msg(chan, "%s: Usage !settopic <new topic>", name);
			}
		} else if(cmd == TOPIC_CLEAR){
			memset(c->topic, 0, sizeof(c->topic));
			c->ask_time = 0;
			ctx->send_msg(chan, "%s: topic cleared.", name);
		}
	}
}

static void topic_tick(time_t now){
	sb_each(c, topic_chans){
		if(!*c->topic && c->ask_time && c->ask_time < now && !c->waiting){
			ctx->send_msg(c->name, "What is the topic for today?");
			c->waiting = true;
		}
	}
}

static intptr_t topic_enabled_check(intptr_t result, bool* arg){
	*arg = result;
	return 0;
}

static void topic_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "note_added") == 0){
		NoteMsg* note = (NoteMsg*)msg->arg;

		bool enabled_for_chan = false;
		MOD_MSG(ctx, "check_chan_enabled", note->channel, &topic_enabled_check, &enabled_for_chan);

		if(!enabled_for_chan)
			return;

		if(note->type == NOTE_STREAM_START){
			struct chan* c = topic_lookup_or_create(note->channel);
			memset(c->topic, 0, sizeof(c->topic));
			c->ask_time = time(0) + 90;
			c->waiting = false;
		}
	}
}
