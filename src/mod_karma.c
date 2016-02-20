#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include "module.h"
#include "stb_sb.h"

static void karma_msg  (const char*, const char*, const char*);
static void karma_cmd  (const char*, const char*, const char*, int);
static void karma_nick (const char*, const char*);
static void karma_join (const char*, const char*);
static bool karma_save (FILE*);
static bool karma_init (const IRCCoreCtx*);

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
	.on_save  = &karma_save,
	.commands = DEFINE_CMDS (
		[KARMA_SHOW] = "\\karma !karma",
		[KARMA_TOP]  = "\\ktop  !ktop"
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

static const int karma_cooldown = 120;

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

	if(!ret) {
		printf("mod_karma: adding %s\n", name);
		KEntry k = {};
		sb_push(k.names, strdup(name));
		sb_push(klist, k);
		ret = &sb_last(klist);
		qsort(klist, sb_count(klist), sizeof(*klist), &karma_sort);
	}

	return ret;
}

static bool karma_update(KEntry* actor, const char* target, bool upvote){
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
		return true;
	}

	return false;
}

static const char ytmnd[] = "!ytmnd ";
static const char ytwnd[] = "!ytwnd ";

static void karma_check(KEntry* actor, const char* msg){

	const char* delim = msg;
	bool changes = false;
	time_t now = time(0);

	if(now - actor->last_give < karma_cooldown) return;

	const size_t dog_size = sizeof(ytmnd) - 1;
	if(
		strncasecmp(msg, ytmnd, dog_size) == 0 ||
		strncasecmp(msg, ytwnd, dog_size) == 0
	){
		const char* beg = msg + dog_size;
		const char* end = strchrnul(msg + dog_size, ' ');
		changes = karma_update(actor, strndupa(beg, end - beg), true);
	} else {
		for(const char* p = msg; *p; ++p){
			if(*p == ' ') delim = p + 1;

			if((*p == '+' && *(p+1) == '+') || (*p == '-' && *(p+1) == '-')){
				char* target;

				if(p == delim){
					// ++name

					char* end = strchrnul(p + 2, ' ');
					target = strndupa(p + 2, end - (p + 2));
				} else {
					// name++

					target = strndupa(delim, p - delim);
				}

				if((changes = karma_update(actor, target, *p == '+'))){
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
	karma_check(karma_add_name(name), msg);
}

static void mod_msg_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
}

static void karma_cmd(const char* chan, const char* name, const char* arg, int cmd){
	KEntry* actor = karma_add_name(name);

	bool admin = strcasecmp(chan + 1, name) == 0;
	MOD_MSG(ctx, "check_admin", name, &mod_msg_cb, &admin);

	printf("check_admin %s = %d\n", name, admin);

	bool wlist = false;
	MOD_MSG(ctx, "check_whitelist", name, &mod_msg_cb, &wlist);

	printf("check_wlist %s = %d\n", name, wlist);

	switch(cmd){
		case KARMA_SHOW: {
			if(!wlist) return;

			if(!*arg++){
				int total = actor->up - actor->down;
				ctx->send_msg(chan, "%s: You have %d karma [+%d|-%d].", name, total, actor->up, actor->down);
			} else {
				KEntry* k = karma_find(arg, false);
				if(k){
					int total = k->up - k->down;
					ctx->send_msg(chan, "%s: %s has %d karma [+%d|-%d].", name, arg, total, k->up, k->down);
				}
			}
		} break;

		case KARMA_TOP: {
			if(!admin) return;

			char msg_buf[256];
			char* msg_ptr = msg_buf;
			size_t sz = sizeof(msg_buf);

			int limit = sb_count(klist);
			if(limit > 3) limit = 3;

			for(int i = 0; i < limit; ++i){
				int tmp = snprintf(
					msg_ptr,
					sz,
					" [%s: %d]",
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
