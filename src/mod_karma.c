#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include "module.h"
#include "stb_sb.h"
#include "inso_utils.h"

static void karma_msg      (const char*, const char*, const char*);
static void karma_cmd      (const char*, const char*, const char*, int);
static void karma_nick     (const char*, const char*);
static void karma_join     (const char*, const char*);
static bool karma_save     (FILE*);
static bool karma_init     (const IRCCoreCtx*);
static void karma_modified (void);
static void karma_mod_msg  (const char*, const IRCModMsg*);
static void karma_quit     (void);

enum { KARMA_SHOW, KARMA_TOP };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "karma",
	.desc     = "Tracks imaginary internet points",
	.flags    = IRC_MOD_GLOBAL,
	.on_msg   = &karma_msg,
	.on_cmd   = &karma_cmd,
	.on_nick  = &karma_nick,
	.on_join  = &karma_join,
	.on_init  = &karma_init,
	.on_quit  = &karma_quit,
	.on_save  = &karma_save,
	.on_modified = &karma_modified,
	.on_mod_msg  = &karma_mod_msg,
	.commands = DEFINE_CMDS (
		[KARMA_SHOW] = CONTROL_CHAR "karma " CONTROL_CHAR_2 "karma",
		[KARMA_TOP]  = CONTROL_CHAR "ktop "  CONTROL_CHAR_2 "ktop"
	)
};

static const IRCCoreCtx* ctx;

typedef struct KEntry_ {
	char** names;
	int up, down;
	time_t last_give;
	int active_idx;
} KEntry;

static KEntry* klist;

static const int karma_cooldown = 0;

static KEntry* karma_find(const char* name, bool adjust){
	for(KEntry* k = klist; k < sb_end(klist); ++k){
		for(char** n = k->names; n < sb_end(k->names); ++n){
			if(strcasecmp(*n, name) == 0){
				if(adjust){
					k->active_idx = n - k->names;
				}
				return k;
			}
		}
	}
	return NULL;
}

static int karma_sort(const void* _a, const void* _b){
	KEntry *a = (KEntry*)_a, *b = (KEntry*)_b;
	return (b->up - b->down) - (a->up - a->down);
}

static KEntry* karma_add_name(const char* name){
	KEntry* ret = karma_find(name, true);

	// TODO: add display name too?

	if(!ret) {
		KEntry k = {};
		sb_push(k.names, strdup(name));
		sb_push(klist, k);
		ret = &sb_last(klist);
		qsort(klist, sb_count(klist), sizeof(*klist), &karma_sort);
	}

	return ret;
}

static bool karma_update(const char* chan, KEntry* actor, const char* target, bool upvote){
	KEntry* k;

	bool narcissist = false;
	for(char** n = actor->names; n < sb_end(actor->names); ++n){
		if(strcasecmp(*n, target) == 0){
			narcissist = true;
			break;
		}
	}

	if(!narcissist && (k = karma_find(target, false))){
		int* i = upvote ? &k->up : &k->down;
		(*i)++;

		if(!upvote){
			actor->down++;
		}

		// for miblo... what were you smoking? :P

		const char* link = NULL;

		switch(k->up - k->down){

			case 33: {
				switch(rand()%5){
					case 0: link = "https://youtu.be/sNIF6EfL4aQ"; break;
					case 1: link = "https://youtu.be/KskeSH4_9zQ"; break;
					case 2: link = "https://youtu.be/Er3CzM534Qs"; break; // d7
					case 3: link = "https://youtu.be/wsYL_46VOLU"; break;
					case 4: link = "https://youtu.be/5AC65UldyxQ"; break;
				};
			} break;

			case 45: {
				link = rand()%2? "https://youtu.be/LtTORnD9wWw" : "https://youtu.be/lM7H0ooV_o8";
			} break;

			case 78: link = "https://youtu.be/zMS63my4DuA"; break;

			case 314:
			case 3141:
			case 31415: {
				link = "https://youtu.be/0rYsPmVtHRM";
			} break;

			case 18 : link = "https://youtu.be/B-oU2xlViRQ"; break;
			case 27 : link = "https://youtu.be/tLsQsirjmeo"; break;
			case 40 : link = "https://youtu.be/QyQzDmnDriY"; break;
			case 50 : link = "https://youtu.be/_8Aytn3Fcu0"; break;
			case 65 : link = "https://youtu.be/zA52uNzx7Y4"; break;
			case 69 : link = "https://youtu.be/lMKhq8l_9mI"; break;
			case 88 : link = "https://youtu.be/IJ1DIFv0ZyY"; break;
			case 96 : link = "https://youtu.be/WgQ7qOCDFYQ"; break;
			case 101: link = "https://youtu.be/D3m99nfBUHg"; break;
			case 144: link = "https://youtu.be/NjxNnqTcHhg"; break;
			case 147: link = "https://youtu.be/7GkgoeKHsXI"; break;
			case 180: link = "https://youtu.be/VRef1-6imT8"; break;
			case 213: link = "https://youtu.be/KDwgQyJ_uzg"; break;
			case 256: link = "https://youtu.be/jt5tRaV3iY0"; break;
			case 360: link = "https://youtu.be/cGMWL8cOeAU"; break;
			case 386: link = "https://youtu.be/VsE0BwQ3l8U"; break;
			case 400: link = "https://youtu.be/z5LW07FTJbI"; break;
			case 420: link = "https://youtu.be/WeYsTmIzjkw"; break;
			case 444: link = "https://youtu.be/8URkBbDsF9o"; break;
			case 512: link = "https://youtu.be/dulxbKkj9Wg"; break;
			case 666: link = "https://youtu.be/WxnN05vOuSM"; break;
			case 777: link = "https://youtu.be/gzfgUCOzUoY"; break;
			case 888: link = "https://youtu.be/8NUesc8pXj4"; break;
			case 925: link = "https://youtu.be/UbxUSsFXYo4"; break;
		}

		if(link){
			ctx->send_msg(chan, "Happy birthday %s %s", inso_dispname(ctx, target), link);
		}

		return true;
	}

	return false;
}

static const char ytmnd[] = "!ytmnd ";
static const char ytwnd[] = "!ytwnd ";
static const char yatmnd[] = "!yatmnd ";
static const char non_name_chars[] = " ~!\"'$%&*()<>@;#.,/:?";

static void karma_check(const char* chan, KEntry* actor, const char* msg){

	const char* delim = msg;
	bool changes = false;
	time_t now = time(0);

	if(now - actor->last_give < karma_cooldown) return;

	size_t dog_size = sizeof(ytmnd) - 1;
	if(
		strncasecmp(msg, ytmnd, dog_size) == 0 ||
		strncasecmp(msg, ytwnd, dog_size) == 0 ||
		(++dog_size && strncasecmp(msg, yatmnd, dog_size) == 0)
	){
		const char* beg = msg + dog_size;
		size_t len = strcspn(beg, non_name_chars);
		changes = karma_update(chan, actor, strndupa(beg, len), true);
	} else {
		for(const char* p = msg; *p; ++p){
			if(strchr(non_name_chars, *p)){
				delim = p + 1;
			} else if((*p == '+' && *(p+1) == '+') || (*p == '-' && *(p+1) == '-')){
				char* target;

				if(p == delim){
					// ++name

					size_t len = strcspn(p + 2, non_name_chars);
					target = strndupa(p + 2, len);

				} else {
					// name++

					target = strndupa(delim, p - delim);
				}

				if((changes = karma_update(chan, actor, target, *p == '+'))){
					break;
				}
				++p;
			}
		}
	}

	if(changes){
		actor->last_give = time(0);
		ctx->save_me();
	}

}

static void karma_msg(const char* chan, const char* name, const char* msg){
	karma_check(chan, karma_add_name(name), msg);
}

static void karma_cmd(const char* chan, const char* name, const char* arg, int cmd){
	KEntry* actor = karma_add_name(name);

	bool admin = strcasecmp(chan + 1, name) == 0 || inso_is_admin(ctx, name);
	bool wlist = inso_is_wlist(ctx, name);

	switch(cmd){
		case KARMA_SHOW: {
			const char* dispname = inso_dispname(ctx, name);
			if(!*arg++){
				int total = actor->up - actor->down;
				ctx->send_msg(chan, "%s: You have %d karma [+%d|-%d].", dispname, total, actor->up, actor->down);
			} else {
				if(!wlist && strcmp(arg, name) != 0) return;
				KEntry* k = karma_find(arg, false);
				if(k){
					int total = k->up - k->down;
					ctx->send_msg(chan, "%s: %s has %d karma [+%d|-%d].", dispname, arg, total, k->up, k->down);
				}
			}
		} break;

		case KARMA_TOP: {
			if(!admin) return;

			char msg_buf[256];
			char* msg_ptr = msg_buf;
			size_t sz = sizeof(msg_buf);

			int limit = sb_count(klist);
			int requested = 3;

			if(*arg++){
				requested = strtol(arg, NULL, 0);
			}

			if(requested > 10) requested = 10;
			if(requested < 1) requested = 1;

			if(requested < limit) limit = requested;

			for(int i = 0; i < limit; ++i){
				int tmp = snprintf(
					msg_ptr,
					sz,
					" |%s: %d|",
					klist[i].names[klist[i].active_idx],
					klist[i].up - klist[i].down
				);
				if(tmp > 0){
					sz -= tmp;
					msg_ptr += tmp;
				}
			}

			ctx->send_msg(chan, "Top karma:%s.", msg_buf);
		} break;
	}
}

static void karma_nick(const char* prev, const char* cur){
	KEntry* k = karma_find(prev, false);
	if(!k) return;

	bool already_known = false;
	for(char** n = k->names; n < sb_end(k->names); ++n){
		if(strcasecmp(*n, cur) == 0){
			k->active_idx = n - k->names;
			already_known = true;
			break;
		}
	}

	if(!already_known){
		sb_push(k->names, strdup(cur));
		k->active_idx = sb_count(k->names) - 1;
	}
}

static void karma_join(const char* chan, const char* name){
	karma_add_name(name);
}

static void karma_load(void){
	char* names;
	int up, down;

	FILE* f = fopen(ctx->get_datafile(), "r");

	while(fscanf(f, "%ms %d:%d\n", &names, &up, &down) == 3){
		KEntry k = { .up = up, .down = down };
		char *state, *name = strtok_r(names, ":", &state);

		for(; name; name = strtok_r(NULL, ":", &state)){
			sb_push(k.names, strdup(name));
		}

		k.active_idx = sb_count(k.names) - 1;

		sb_push(klist, k);
		free(names);
	}

	fclose(f);

	qsort(klist, sb_count(klist), sizeof(*klist), &karma_sort);
}

static bool karma_save(FILE* f){

	qsort(klist, sb_count(klist), sizeof(*klist), &karma_sort);

	for(KEntry* k = klist; k < sb_end(klist); ++k){
		if(k->up == 0 && k->down == 0) continue;

		for(char** name = k->names; name < sb_end(k->names); ++name){
			fprintf(f, "%s:", *name);
		}
		fprintf(f, " %d:%d\n", k->up, k->down);
	}

	return true;
}

static bool karma_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	karma_load();
	return true;
}

static void karma_quit(void){
	for(int i = 0; i < sb_count(klist); ++i){
		for(int j = 0; j < sb_count(klist[i].names); ++j){
			free(klist[i].names[j]);
		}
		sb_free(klist[i].names);
	}
	sb_free(klist);
}

static void karma_modified(void){
	karma_quit();
	karma_load();
}

static void karma_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "karma_get") == 0){
		int karma = 0;
		KEntry* k = karma_find((const char*)msg->arg, false);
		if(k) karma = k->up - k->down;

		msg->callback(karma, msg->cb_arg);
	}
}
