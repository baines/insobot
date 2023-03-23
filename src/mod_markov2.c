#define STB_SB_MMAP
#include "module.h"
#include "inso_utils.h"
#include "inso_ht.h"
#include "stb_sb.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <regex.h>

#include "markov2/markov.h"
#include "markov2/state.c"
#include "markov2/learn.c"
#include "markov2/generate.c"
#include "markov2/load-save.c"

static bool markov_init (const IRCCoreCtx*);
static void markov_quit (void);
static void markov_cmd  (const char*, const char*, const char*, int);
static void markov_msg  (const char*, const char*, const char*);
static bool markov_save (FILE*);

enum { MARKOV_SAY, MARKOV_INTERVAL, MARKOV_SAVE };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "markov2",
	.desc     = "Says incomprehensible stuff",
	.flags    = IRC_MOD_DEFAULT,
	.on_init  = &markov_init,
	.on_quit  = &markov_quit,
	.on_cmd   = &markov_cmd,
	.on_msg   = &markov_msg,
	.on_save  = &markov_save,
	.commands = DEFINE_CMDS (
		[MARKOV_SAY]      = CMD("say"),
		[MARKOV_INTERVAL] = CMD("interval") CMD("gap"),
		[MARKOV_SAVE]     = CMD("msave")
	),
	.cmd_help = DEFINE_CMDS (
		[MARKOV_SAY]      = "| Instruct the bot say something random (5 minute cooldown).",
		[MARKOV_INTERVAL] = "<N> | Change the rate of random messages to 1 in N, e.g. N == 100 means after every 100 messages on average",
		[MARKOV_SAVE]     = "| Force a save of the markov data"
	)
};

static const IRCCoreCtx* ctx;

static size_t msg_chance;
static regex_t url_regex;
static regex_t say_regex;

static bool markov_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	unsigned int seed = rand();

	int fd = open("/dev/urandom", O_RDONLY);
	if(fd != -1){
		if(read(fd, &seed, sizeof(seed)) == -1){
			perror("markov_init: read");
		}
		close(fd);
	}

	initstate_r(seed, rng_state_mem, sizeof(rng_state_mem), &rng_state);
	setstate_r(rng_state_mem, &rng_state);

	sbmm_push(word_mem, 0);

	regcomp(&url_regex, "(www\\.|https?:\\/\\/|\\.com|\\.[a-zA-Z]\\/)", REG_ICASE | REG_EXTENDED | REG_NOSUB);
	regcomp(&say_regex, "^@?insobot[:,]? say (.*)$", REG_ICASE | REG_EXTENDED);

	chain_keys_ht.hash_fn   = &chain_key_hash;
	chain_keys_ht.elem_size = sizeof(MarkovLinkKey);
	chain_keys_ht.alloc_fn  = &ht_alloc;
	chain_keys_ht.free_fn   = &ht_free;

	word_ht.hash_fn   = &wordinfo_hash;
	word_ht.elem_size = sizeof(WordInfo);
	word_ht.alloc_fn  = &ht_alloc;
	word_ht.free_fn   = &ht_free;

	const char* filename = ctx->get_datafile();
	if(!markov_load(filename)){
		if(chain_keys_ht.memory) ht_free(chain_keys_ht.memory, chain_keys_ht.capacity);
		inso_ht_init(&chain_keys_ht, 4096, sizeof(MarkovLinkKey), &chain_key_hash);

		if(word_ht.memory) ht_free(word_ht.memory, word_ht.capacity);
		inso_ht_init(&word_ht, 4096, sizeof(WordInfo), &wordinfo_hash);
	}

	start_sym_idx = find_or_add_word("\002", 1, NULL);
	end_sym_idx   = find_or_add_word("\003", 1, NULL);

	msg_chance = 100;

	return true;
}

static void markov_quit(void){
	sbmm_free(word_mem);
	sbmm_free(chain_vals);

	inso_ht_free(&chain_keys_ht);
	inso_ht_free(&word_ht);

	regfree(&url_regex);
	regfree(&say_regex);
}

static void markov_send(const char* chan, const char* maybe_arg) {
	char best_output[1024];
	size_t best_size = 0;

	if(maybe_arg && *maybe_arg == ' ') {
		++maybe_arg;
	}

	for(int i = 0; i < 5; ++i) {
		char output[1024] = "";
		size_t result_size;

		if(!maybe_arg || !*maybe_arg) {
			printf("plain gen\n");
			result_size = markov_gen(output, sizeof(output));
		} else {
			printf("prefix gen\n");
			result_size = markov_gen_prefix(maybe_arg, output, sizeof(output));
		}

		if(result_size == 0) {
			continue;
		}

		if(*output == '.' || *output == '!' || *output == '/') {
			continue;
		}

		if(result_size > best_size) {
			best_size = result_size;
			memcpy(best_output, output, sizeof(best_output));
		}
	}

	if(best_size == 0) {
		return;
	}

	ctx->send_msg(chan, "%s%s", best_output, markov_get_punct());
}

static void markov_cmd(const char* chan, const char* name, const char* arg, int cmd){
	time_t now = time(0);

	static const int say_cooldown = 10;
	static time_t last_say;

	bool admin = inso_is_admin(ctx, name);

	switch(cmd){

		case MARKOV_SAY: {
			if(admin || now - last_say >= say_cooldown){
				markov_send(chan, arg);
				last_say = now;
			}
		} break;

		case MARKOV_INTERVAL: {
			if(!admin) break;

			if(*arg++){
				int chance = strtoul(arg, NULL, 0);
				if(chance != 0){
					msg_chance = chance;
				}
			}

			ctx->send_msg(chan, "%s: interval = %zu.", name, msg_chance);
		} break;

		case MARKOV_SAVE: {
			const char* owner = getenv("IRC_ADMIN");
			if(!owner || strcmp(name, owner) != 0)
				break;

			ctx->save_me();
			ctx->send_msg(chan, "%s: save complete.", name);
		} break;
	}
}

static void markov_msg(const char* chan, const char* name, const char* msg){

	if(*msg == '!' || *msg == '\\'){
		puts("skipping command.");
		return;
	}

	if(regexec(&url_regex, msg, 0, NULL, 0) == 0){
		puts("skipping url.");
		return;
	}

	bool send_a_msg = markov_rand(msg_chance) == 0;
	const char* args_ptr = NULL;

	regmatch_t matches[2] = {};
	if(regexec(&say_regex, msg, 2, matches, 0) == 0) {
		send_a_msg = markov_rand(10) > 0;
		args_ptr = msg + matches[1].rm_so;
		printf("args: [%s]\n", args_ptr);
	}

	if(send_a_msg) {
		markov_send(chan, args_ptr);
	} else {
		markov_learn_from_msg(msg);
	}
}

