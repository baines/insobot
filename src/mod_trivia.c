#include "module.h"
#include "inso_utils.h"
#include "inso_json.h"
#include "stb_sb.h"
#include <argz.h>
#include <stdbool.h>
#include <curl/curl.h>
#include <ctype.h>

static bool trivia_init (const IRCCoreCtx*);
static void trivia_msg  (const char* chan, const char* name, const char* msg);
static void trivia_cmd  (const char* chan, const char* name, const char* arg, int cmd);
static void trivia_load (void);
static bool trivia_save (FILE* f);
static void trivia_tick (time_t now);

enum { TRIVIA_CMD };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "trivia",
	.desc     = "Anime trivia game",
	.on_init  = &trivia_init,
	.on_cmd   = &trivia_cmd,
	.on_tick  = &trivia_tick,
	.on_msg   = &trivia_msg,
	.on_save  = &trivia_save,
	.commands = DEFINE_CMDS (
		[TRIVIA_CMD] = "!trivia"
	),
};

struct TriviaState {
	char* channel;
	char* question;
	char* answer_argz;
	size_t answer_sz;

	time_t hint;
	time_t end;
};

struct TriviaScore {
	char* username;
	int score;
};

static const IRCCoreCtx* ctx;
static sb(struct TriviaState) state;
static sb(struct TriviaScore) scores;

#include "trivia/anilist.c"
#include "trivia/levenshtein.c"

static bool trivia_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	trivia_load();
	return true;
}

static void trivia_free(struct TriviaState* ts) {
	free(ts->question);
	ts->question = NULL;

	free(ts->answer_argz);
	ts->answer_argz = NULL;
	ts->answer_sz = 0;
	ts->end = 0;
}

static void trivia_hint(struct TriviaState* ts) {
	assert(ts->channel);
	assert(ts->answer_argz);

	sb(char) hint = NULL;

	char* answer_copy = strdupa(ts->answer_argz);
	char* state = NULL;

	for(char* word = strtok_r(answer_copy, " ", &state); word; word = strtok_r(NULL, " ", &state)) {
		int wlen = strlen(word);
		int viz_letter_count = INSO_MAX(1, wlen / 4);

		for(int i = 0; i < wlen; ++i) {
			bool keep = (rand() % (wlen - i)) < viz_letter_count;
			if(keep) {
				sb_push(hint, word[i]);
				viz_letter_count--;
			} else {
				sb_push(hint, '_');
			}
		}

		sb_push(hint, ' ');
	}

	sb_push(hint, '\0');

	ctx->send_msg(ts->channel, "[Trivia] Hint: %s", hint);
	sb_free(hint);
}

static struct TriviaState* trivia_get(const char* chan) {
	sb_each(s, state) {
		if(strcmp(s->channel, chan) == 0) {
			return s;
		}
	}

	struct TriviaState ts = {
		.channel = strdup(chan)
	};
	sb_push(state, ts);

	return &sb_last(state);
}

static void trivia_tick(time_t now) {
	sb_each(ts, state) {
		if(ts->end == 0) {
			continue;
		}

		if(now > ts->end) {
			ctx->send_msg(ts->channel, "[Trivia] Time's up - the answer was: %s\n", ts->answer_argz);
			trivia_free(ts);
		} else if(ts->hint && now > ts->hint) {
			trivia_hint(ts);
			ts->hint = 0;
		}
	}
}

static float str_similarity(const char* _a, const char* _b) {
	char* a = strdupa(_a);
	int len = 0;

	for(char* p = a; *p; ++p) {
		*p = tolower(*p);
		++len;
	}

	char* b = strdupa(_b);
	for(char* p = b; *p; ++p) {
		*p = tolower(*p);
	}

	int n = levenshtein_n(a, len, b, strlen(b));
	return (float)(len - n) / (float)len;
}

static struct TriviaScore* score_get(const char* user) {
	sb_each(s, scores) {
		if(strcmp(s->username, user) == 0) {
			return s;
		}
	}

	struct TriviaScore s = {
		.username = strdup(user)
	};
	sb_push(scores, s);

	return &sb_last(scores);
}

static void trivia_msg(const char* chan, const char* name, const char* msg) {
	struct TriviaState* ts = trivia_get(chan);
	if(ts->end == 0) {
		return;
	}

	char* answer = NULL;
	while((answer = argz_next(ts->answer_argz, ts->answer_sz, answer))) {
		float f = str_similarity(msg, answer);
		if (f > 0.85) {
			if(f != 1.0f) {
				ctx->send_msg(ts->channel, "[Trivia] Correct! The answer was: %s (%.2f%%)", answer, f);
			} else {
				ctx->send_msg(ts->channel, "[Trivia] Correct! The answer was: %s", answer);
			}
			trivia_free(ts);

			struct TriviaScore* s = score_get(name);
			s->score++;

			ctx->save_me();

			break;
		}
	}
}

static void trivia_start(const char* chan) {
	struct TriviaState* ts = trivia_get(chan);

	// already in progress
	if(ts->end > 0) {
		return;
	}

	if(trivia_start_anilist(chan, ts)) {
		ctx->send_msg(chan, "[Trivia] %s", ts->question);
	} else {
		ctx->send_msg(chan, "[Trivia] error starting trivia :(");
	}
}

static int trivia_score_sort(struct TriviaScore* a, struct TriviaScore* b) {
	return b->score - a->score;
}

static void trivia_leaderboard(const char* chan) {
	qsort(scores, sb_count(scores), sizeof(*scores), (int(*)())&trivia_score_sort);
	size_t count = INSO_MIN(5U, sb_count(scores));

	char out[1024] = "";
	char* p = out;
	size_t sz = sizeof(out);

	for(size_t i = 0; i < count; ++i) {
		struct TriviaScore* s = scores + i;
		snprintf_chain(&p, &sz, "[#%d %s: %d] ", i + 1, s->username, s->score);
	}

	ctx->send_msg(chan, "[Trivia] High Scores: %s", out);
}

static void trivia_cmd(const char* chan, const char* name, const char* arg, int cmd){
	switch(cmd){
		case TRIVIA_CMD: {
			if(*arg++) {
				if(strcmp(arg, "leaderboard") == 0 || strcmp(arg, "scores") == 0) {
					trivia_leaderboard(chan);
				}
			} else {
				trivia_start(chan);
			}
		} break;
	}
}

static void trivia_load(void) {
	FILE* f = fopen(ctx->get_datafile(), "r");
	if(!f) return;

	char* user;
	int score;

	while(fscanf(f, "%ms %d", &user, &score) == 2){
		struct TriviaScore s = {
			.username = user,
			.score = score
		};
		sb_push(scores, s);
	}
}

static bool trivia_save(FILE* f) {
	sb_each(s, scores) {
		fprintf(f, "%s %d\n", s->username, s->score);
	}
	return true;
}
