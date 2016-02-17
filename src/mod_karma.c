#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "module.h"
#include "stb_sb.h"

static void karma_msg  (const char*, const char*, const char*);
static bool karma_save (FILE*);
static bool karma_init (const IRCCoreCtx*);

const IRCModuleCtx irc_mod_ctx = {
	.name     = "karma",
	.desc     = "Tracks imaginary internet points",
	.flags    = IRC_MOD_DEFAULT,
	.on_msg   = &karma_msg,
	.on_init  = &karma_init,
	.on_save  = &karma_save,
};

static const IRCCoreCtx* ctx;

typedef struct KEntry_ {
	char** names;
	int up, down;
} KEntry;

static KEntry* klist;

static KEntry* karma_find(const char* name){
	for(KEntry* k = klist; k < sb_end(klist); ++k){
		for(char** n = k->names; n < sb_end(k->names); ++n){
			if(strcasecmp(*n, name) == 0){
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

static void karma_add_name(const char* name){
	if(!karma_find(name)){
		KEntry k = {};
		sb_push(k.names, strdup(name));
		sb_push(klist, k);
		qsort(klist, sb_count(klist), sizeof(*klist), &karma_sort);
	}
}

static void karma_check(const char* name, const char* msg){

	//TODO: prefix operators

	const char* delim = msg;
	bool changes = false;

	for(const char* p = msg; *p; ++p){
		if(*p == ' ') delim = p + 1;

		if((*p == '+' && *(p+1) == '+') || (*p == '-' && *(p+1) == '-')){
			char* target = strndup(delim, (p - delim));
			KEntry* k;
			if(strcasecmp(name, target) != 0 && (k = karma_find(target))){
				int* i = *p == '+' ? &k->up : &k->down;
				(*i)++;
				changes = true;
			}
			free(target);
			++p;
		}
	}

	if(changes){
		ctx->save_me();
	}

}

static void karma_msg(const char* chan, const char* name, const char* msg){

	karma_add_name(name);

	const char* arg = msg;
	enum { KARMA_SHOW, KARMA_TOP, KARMA_MERGE, KARMA_SPLIT, KARMA_CONFIRM };
	int cmd = ctx->check_cmds(
		&arg,
		"\\karma,!karma",
		"\\ktop,!ktop",
		"\\kmerge",
		"\\ksplit",
		"\\kconfirm",
		NULL
	);

	switch(cmd){
		case KARMA_SHOW: {
			KEntry* k = karma_find(name);
			if(k){
				int total = k->up - k->down;
				ctx->send_msg(chan, "%s: You have %d karma [+%d|-%d].", name, total, k->up, k->down);
			}
		} break;

		case KARMA_TOP: {
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
					klist[i].names[0],
					klist[i].up - klist[i].down
				);
				if(tmp > 0){
					sz -= tmp;
					msg_ptr += tmp;
				}
			}

			ctx->send_msg(chan, "Top karma:%s.", msg_buf);
		} break;

		case KARMA_MERGE: {
			// generate random num, ask name to \\kconfirm <num> or timeout after 1min
		} break;

		case KARMA_SPLIT: {
			// opposite of kmerge
		} break;

		case KARMA_CONFIRM: {
			// complete the ongoing merge / split operation if code correct + within timeout
		} break;

		default: {
			karma_check(name, msg);
		} break;
	}

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

		sb_push(klist, k);
		free(names);
	}

	fclose(f);

	qsort(klist, sb_count(klist), sizeof(*klist), &karma_sort);

}

static bool karma_save(FILE* f){

	qsort(klist, sb_count(klist), sizeof(*klist), &karma_sort);

	for(KEntry* k = klist; k < sb_end(klist); ++k){
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
