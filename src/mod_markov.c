#define STB_SB_MMAP
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <assert.h>
#include <regex.h>
#include <zlib.h>
#include "module.h"
#include "inso_utils.h"
#include "inso_ht.h"
#include "stb_sb.h"

static bool markov_init (const IRCCoreCtx*);
static void markov_quit (void);
static void markov_join (const char*, const char*);
static void markov_cmd  (const char*, const char*, const char*, int);
static void markov_msg  (const char*, const char*, const char*);
static void markov_mod_msg(const char* sender, const IRCModMsg* msg);
static bool markov_save (FILE*);
static void markov_stdin(const char* msg);

enum { MARKOV_SAY, MARKOV_ASK, MARKOV_INTERVAL, MARKOV_LENGTH, MARKOV_STATUS, MARKOV_SAVE };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "markov",
	.desc     = "Says incomprehensible stuff",
	.flags    = IRC_MOD_DEFAULT,
	.on_init  = &markov_init,
	.on_quit  = &markov_quit,
	.on_cmd   = &markov_cmd,
	.on_msg   = &markov_msg,
	.on_join  = &markov_join,
	.on_save  = &markov_save,
	.on_stdin = &markov_stdin,
	.on_mod_msg = &markov_mod_msg,
	.commands = DEFINE_CMDS (
		[MARKOV_SAY]      = CMD1("say"),
		[MARKOV_ASK]      = CMD1("ask"),
		[MARKOV_INTERVAL] = CMD1("interval") CMD1("gap"),
		[MARKOV_LENGTH]   = CMD1("len"),
		[MARKOV_STATUS]   = CMD1("status"),
		[MARKOV_SAVE]     = CMD1("msave")
	)
};

static const IRCCoreCtx* ctx;

// Types {{{

typedef uint32_t word_idx_t;

struct MarkovLinkKey_ {
	uint32_t val_idx;
	word_idx_t word_idx_1 : 24;
	word_idx_t word_idx_2 : 24;
} __attribute__ ((packed));
typedef struct MarkovLinkKey_ MarkovLinkKey;

typedef struct {
	word_idx_t word_idx : 24;
	uint8_t count;
	uint32_t next;
} MarkovLinkVal;

typedef struct {
	word_idx_t word_idx;
	uint32_t   total;
} WordInfo;

// }}}

// Global Variables {{{

static const char* bad_end_words[] = { "and", "the", "a", "as", "if", "i", ",", "/", NULL };
static const char* ignores[]       = { "hmh_bot", "hmd_bot", "drakebot_", "GitHub", NULL };
static const char* skip_words[]    = { "p", "d", "b", "o", "-p", "-d", "-b", "-o", NULL };

static const int say_cooldown = 300;
static time_t last_say;

static char* word_mem;

static inso_ht        chain_keys_ht;
static MarkovLinkVal* chain_vals;

static inso_ht word_ht;

static char rng_state_mem[256];
static struct random_data rng_state;

static regex_t url_regex;

static size_t max_chain_len = 16;
static size_t msg_chance = 150;

static word_idx_t start_sym_idx;
static word_idx_t end_sym_idx;

static uint32_t recent_hashes[128];
static size_t hash_idx;

static char** markov_nicks;

// }}}

// Hash Funcs {{{

static uint32_t markov_hash(const char* str, size_t len){
	uint32_t hash = 6159;
	for(int i = 0; i < len; ++i){
		hash = hash * 187 + str[i];
	}
	return hash;
}

static void markov_add_hash(const char* str, size_t len){
	recent_hashes[hash_idx] = markov_hash(str, len);
	hash_idx = (hash_idx + 1) % ARRAY_SIZE(recent_hashes);
}

static bool markov_check_dup(const char* str, size_t len){
	uint32_t hash = markov_hash(str, len);
	for(int i = 0; i < ARRAY_SIZE(recent_hashes); ++i){
		if(recent_hashes[i] == hash) return true;
	}
	return false;
}

static size_t wordinfo_hash(const void* arg){
	const WordInfo* w = arg;
	return markov_hash(word_mem + w->word_idx, strlen(word_mem + w->word_idx));
}

static bool wordinfo_cmp(const void* elem, void* param){
	const char* str = param;
	const WordInfo* w = elem;
	return strcmp(str, word_mem + w->word_idx) == 0;
}

static uint32_t hash6432shift(uint64_t key){
	key = (~key) + (key << 18);
	key = key ^ (key >> 31);
	key = key * 21;
	key = key ^ (key >> 11);
	key = key + (key << 6);
	key = key ^ (key >> 22);
	return (uint32_t)key;
}

static size_t chain_key_hash(const void* arg){
	const MarkovLinkKey* k = arg;
	return hash6432shift(((uint64_t)k->word_idx_1 << 32UL) | k->word_idx_2);
}

static bool chain_key_cmp(const void* elem, void* param){
	uint64_t* id = param;
	const MarkovLinkKey* key = elem;
	return (((uint64_t)key->word_idx_1 << 32UL) | key->word_idx_2) == *id;
}

// }}}

// Utility Funcs {{{

static void* ht_alloc(size_t n){
	void* mem = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(mem != MAP_FAILED);
	return mem;
}

static void ht_free(void* p, size_t n){
	assert(munmap(p, n) == 0);
}

static uint32_t markov_rand(uint32_t limit){
	int32_t x;

	do {
		random_r(&rng_state, &x);
	} while (x >= (RAND_MAX - RAND_MAX % limit));

	return x % limit;
}

static bool find_word_addref(const char* word, size_t word_len, word_idx_t* index){
	WordInfo* info;
	size_t hash = markov_hash(word, word_len);
	char* zword = strndupa(word, word_len);

	if((info = inso_ht_get(&word_ht, hash, &wordinfo_cmp, zword))){
		*index = info->word_idx;
		++info->total;
		return true;
	}

	return false;
}

static word_idx_t find_or_add_word(const char* word, size_t word_len){
	word_idx_t index;

	if(!find_word_addref(word, word_len, &index)){
		char* p = memcpy(sbmm_add(word_mem, word_len+1), word, word_len+1);
		index = p - word_mem;
		inso_ht_put(&word_ht, &(WordInfo){ index, 1 });
	}

	return index;
}

static MarkovLinkKey* find_key(word_idx_t a, word_idx_t b){
	uint64_t id = ((uint64_t)a << 32) | b;
	return inso_ht_get(&chain_keys_ht, hash6432shift(id), &chain_key_cmp, &id);
}

static const char* markov_get_punct(){
	size_t val = markov_rand(100);

	if(val < 67) return ".";
	if(val < 72) return "?";
	if(val < 85) return "!";
	if(val < 97) return "...";
	if(val < 98) return "‽";
	if(val < 99) return ". FailFish";

	return ". Kappa";
}

static void markov_add(word_idx_t indices[static 3]){

	MarkovLinkKey* key = find_key(indices[0], indices[1]);

	if(!key){
		MarkovLinkVal val = {
			.word_idx = indices[2],
			.count = 1,
			.next = -1
		};
		sbmm_push(chain_vals, val);

		MarkovLinkKey new_key = {
			.word_idx_1 = indices[0],
			.word_idx_2 = indices[1],
			.val_idx  = sbmm_count(chain_vals) - 1
		};
		inso_ht_put(&chain_keys_ht, &new_key);

	} else {
		bool found = false;
		ssize_t last_idx = -1;

		for(uint32_t i = key->val_idx; i != -1; i = chain_vals[i].next){
			if(chain_vals[i].word_idx == indices[2]){
				if(chain_vals[i].count < UCHAR_MAX) ++chain_vals[i].count;
				found = true;
				break;
			}

			last_idx = i;
		}

		if(!found){
			MarkovLinkVal val = {
				.word_idx = indices[2],
				.count = 1,
				.next = -1
			};
			sbmm_push(chain_vals, val);

			assert(last_idx != -1);
			chain_vals[last_idx].next = sbmm_count(chain_vals) - 1;
		}
	}
}

static void markov_replace(char** msg, const char* from, const char* to){
	size_t from_len = strlen(from);
	size_t to_len = strlen(to);
	size_t msg_len = sb_count(*msg);

	char* p;
	size_t off = 0;

	while((p = strstr(*msg + off, from))){

		off = p - *msg;

		if(to_len > from_len){
			memset(sb_add(*msg, to_len - from_len), 0, to_len - from_len);
		} else {
			stb__sbn(*msg) -= (from_len - to_len);
		}

		p = *msg + off;

		const char* end_p = *msg + msg_len;
		memmove(p + to_len, p + from_len, end_p - (p + from_len));
		memcpy(p, to, to_len);

		off += to_len;

		msg_len += (to_len - from_len);
	}

}

// }}}

// Generation {{{

static size_t markov_gen(char* buffer, size_t buffer_len){
	if(!buffer_len) return 0;
	*buffer = 0;

	MarkovLinkKey* key = find_key(start_sym_idx, start_sym_idx);
	assert(key);

	int chain_len = 1 + markov_rand(max_chain_len);
	int links = 0;
	bool should_end = false;

	do {
		size_t total = 0;
		size_t end_count = 0;

		MarkovLinkVal* val = chain_vals + key->val_idx;
		do {
			if(val->word_idx == end_sym_idx) end_count = val->count;
			total += val->count;
		} while(val->next != -1 && (val = chain_vals + val->next));

		assert(total);
		ssize_t count = markov_rand(total);

		should_end =
			(links >= chain_len && end_count > (total / 2)) ||
			(links >= chain_len * 1.5f && end_count) ||
			(links >= chain_len * 2.0f);

		val = chain_vals + key->val_idx;
		while((count -= val->count) >= 0){
			val = chain_vals + val->next;
		}

		if(val->word_idx == end_sym_idx){
			break;
		}

		const char* word = word_mem + val->word_idx;

		for(const char** c = bad_end_words; *c; ++c){
			if(strcmp(*c, word) == 0){
				should_end = false;
				break;
			}
		}

		if(*buffer && strcmp(word, ",") != 0){
			inso_strcat(buffer, buffer_len, " ");
		}
		inso_strcat(buffer, buffer_len, word);

		key = find_key(key->word_idx_2, val->word_idx);
		assert(key);

	} while(!should_end);

	return strlen(buffer);
}

static bool markov_gen_formatted(char* msg, size_t msg_len){
	int num_sentences = markov_rand(10) < 8 ? 1 : 2;

	while(num_sentences--){
		int attempts = 0;

		size_t buff_len = msg_len;
		char* buff = alloca(msg_len);

		size_t tmp_len;

		do {
			tmp_len = markov_gen(buff, buff_len);

			if(*buff == ','){
				tmp_len -= 2;
				memmove(buff, buff + 2, tmp_len);
			}
		} while(attempts++ < 5 && markov_check_dup(buff, tmp_len));

		buff_len = tmp_len;

		if(attempts >= 5){
			puts("Couldn't get a good message, giving up.");
			return false;
		}

		markov_add_hash(buff, buff_len);

		*buff = toupper(*buff);
		memcpy(msg, buff, buff_len);
		msg[buff_len] = 0;

		msg += buff_len;
		msg_len -= buff_len;

		if(num_sentences){
			int written = 0;
			written = INSO_MAX(written, inso_strcat(msg, msg_len, markov_get_punct()));
			written = INSO_MAX(written, inso_strcat(msg, msg_len, " "));

			msg += written;
			msg_len -= written;
		}
	}

	inso_strcat(msg, msg_len, markov_get_punct());

	return true;
}

static void markov_send(const char* chan){
	char buffer[256];
	if(!markov_gen_formatted(buffer, sizeof(buffer))) return;
	ctx->send_msg(chan, "%s", buffer);
}

static void markov_reply(const char* chan, const char* nick){
	char buffer[256];
	if(!markov_gen_formatted(buffer, sizeof(buffer))) return;
	ctx->send_msg(chan, "@%s: %s", nick, buffer);
}

static void markov_ask(const char* chan){
	char buffer[256];
	if(!markov_gen_formatted(buffer, sizeof(buffer))) return;

	size_t len = strlen(buffer);
	if(len && ispunct(buffer[len-1])){
		buffer[len-1] = '?';
	} else if(sizeof(buffer) - len > 1){
		buffer[len] = '?';
		buffer[len+1] = 0;
	}
	ctx->send_msg(chan, "Q: %s", buffer);
}
// }}}

// Loading/Saving {{{

static bool markov_load(){
	gzFile f = gzopen(ctx->get_datafile(), "rb");
	uint32_t word_size = 0, val_size = 0, version = 0;
	char fourcc[4];

#define GZREAD(f, ptr, sz) if(gzread(f, ptr, sz) < sz) goto fail

	GZREAD(f, fourcc, 4);
	if(memcmp(fourcc, "IBMK", 4) != 0){
		fputs("markov_load: invalid file format.", stderr);
		goto fail;
	}

	GZREAD(f, &version, 4);
	if(version != 3){
		fputs("markov_load: invalid version.", stderr);
		goto fail;
	}

	GZREAD(f, &word_size, sizeof(word_size));
	GZREAD(f, &val_size , sizeof(val_size));

	GZREAD(f, &chain_keys_ht.capacity, 4);
	GZREAD(f, &chain_keys_ht.used    , 4);

	GZREAD(f, &word_ht.capacity, 4);
	GZREAD(f, &word_ht.used    , 4);

	GZREAD(f, sbmm_add(word_mem  , word_size), word_size);
	GZREAD(f, sbmm_add(chain_vals, val_size) , val_size * sizeof(MarkovLinkVal));

	chain_keys_ht.memory = ht_alloc(chain_keys_ht.capacity);
	GZREAD(f, chain_keys_ht.memory, chain_keys_ht.capacity);

	word_ht.memory = ht_alloc(word_ht.capacity);
	GZREAD(f, word_ht.memory, word_ht.capacity);

#undef GZREAD

	gzclose(f);
	return true;

fail:
	puts("markov: couldn't read file.");
	gzclose(f);
	return false;
}

static bool markov_save(FILE* file){
	puts("mod_markov: now saving...");

	uint32_t word_size = sbmm_count(word_mem) - 1;
	uint32_t val_size  = sbmm_count(chain_vals);
	uint32_t version   = 3;
	
	while(inso_ht_tick(&chain_keys_ht));
	while(inso_ht_tick(&word_ht));

	gzFile f = gzdopen(dup(fileno(file)), "wb");

#define GZWRITE(f, ptr, sz) if(gzwrite(f, ptr, sz) < sz) goto fail

	GZWRITE(f, "IBMK"  , 4);
	GZWRITE(f, &version, 4);

	GZWRITE(f, &word_size, sizeof(word_size));
	GZWRITE(f, &val_size , sizeof(val_size));

	GZWRITE(f, &chain_keys_ht.capacity, 4);
	GZWRITE(f, &chain_keys_ht.used    , 4);

	GZWRITE(f, &word_ht.capacity, 4);
	GZWRITE(f, &word_ht.used    , 4);

	GZWRITE(f, word_mem + 1, word_size);
	GZWRITE(f, chain_vals, sizeof(MarkovLinkVal) * val_size);

	GZWRITE(f, chain_keys_ht.memory, chain_keys_ht.capacity);
	GZWRITE(f, word_ht.memory, word_ht.capacity);

#undef GZWRITE

	puts("mod_markov: save complete.");
	gzclose(f);
	return true;

fail:
	puts("mod_markov: error saving file.");
	gzclose(f);
	return false;
}

// }}}

// IRC Callbacks {{{

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

	chain_keys_ht.hash_fn   = &chain_key_hash;
	chain_keys_ht.elem_size = sizeof(MarkovLinkKey);
	chain_keys_ht.alloc_fn  = &ht_alloc;
	chain_keys_ht.free_fn   = &ht_free;

	word_ht.hash_fn   = &wordinfo_hash;
	word_ht.elem_size = sizeof(WordInfo);
	word_ht.alloc_fn  = &ht_alloc;
	word_ht.free_fn   = &ht_free;

	if(!markov_load()){
		if(chain_keys_ht.memory) ht_free(chain_keys_ht.memory, chain_keys_ht.capacity);
		inso_ht_init(&chain_keys_ht, 4096, sizeof(MarkovLinkKey), &chain_key_hash);

		if(word_ht.memory) ht_free(word_ht.memory, word_ht.capacity);
		inso_ht_init(&word_ht, 4096, sizeof(WordInfo), &wordinfo_hash);
	}

	start_sym_idx = find_or_add_word("^", 1);
	end_sym_idx   = find_or_add_word("$", 1);

	return true;
}

static void markov_quit(void){
	sbmm_free(word_mem);
	sbmm_free(chain_vals);

	for(int i = 0; i < sb_count(markov_nicks); ++i){
		free(markov_nicks[i]);
	}
	sb_free(markov_nicks);

	inso_ht_free(&chain_keys_ht);
	inso_ht_free(&word_ht);

	regfree(&url_regex);
}

static void markov_cmd(const char* chan, const char* name, const char* arg, int cmd){
	time_t now = time(0);

	bool admin = inso_is_admin(ctx, name);

	switch(cmd){

		case MARKOV_SAY: {
			if(admin || now - last_say >= say_cooldown){
				markov_send(chan);
				last_say = now;
			}
		} break;

		case MARKOV_ASK: {
			if(admin || now - last_say >= say_cooldown){
				markov_ask(chan);
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

		case MARKOV_LENGTH: {
			if(!admin) break;

			if(*arg++){
				int len = strtoul(arg, NULL, 0);
				if(len != 0){
					max_chain_len = len;
				}
			}

			ctx->send_msg(chan, "%s: length = %zu.", name, max_chain_len);
		} break;

		case MARKOV_STATUS: {
			if(!admin) break;

			ctx->send_msg(
				chan,
				"%s: markov status: [words: %zu/%.2fMB] [keys: %zu/%.2fMB] [vals: %d/%.2fMB]",
				name,
				word_ht.used / word_ht.elem_size,
				(sbmm_count(word_mem) + word_ht.used) / (1024.f*1024.f),
				chain_keys_ht.used / chain_keys_ht.elem_size,
				chain_keys_ht.used / (1024.f*1024.f),
				sbmm_count(chain_vals),
				(sbmm_count(chain_vals) * sizeof(MarkovLinkVal)) / (1024.f*1024.f)
			);

		} break;

		case MARKOV_SAVE: {
			if(strcmp(name, BOT_OWNER) != 0) break;
			ctx->save_me();
			ctx->send_msg(chan, "%s: save complete.", name);
		} break;
	}

}

static void markov_msg(const char* chan, const char* name, const char* _msg){

	markov_join(chan, name);

	if(*_msg == '!' || *_msg == '\\'){
		puts("skipping command.");
		return;
	}

	if(regexec(&url_regex, _msg, 0, NULL, 0) == 0){
		puts("skipping url.");
		return;
	}

	for(const char** n = ignores; *n; ++n){
		if(strcasecmp(*n, name) == 0){
			return;
		}
	}

	size_t msg_len = strlen(_msg);
	char* msg = NULL;
	memcpy(sb_add(msg, msg_len + 1), _msg, msg_len + 1);

	ctx->strip_colors(msg);

	for(char* c = msg; c < sb_end(msg); ++c){
		*c = tolower(*c);
	}

	const char* bot_name = ctx->get_username();
	size_t      bot_name_len = strlen(bot_name);
	const char* name_pats[] = { "@%s", "%s:", "%s," };
	char        name_buf[256];
	bool        found_name = false;

	assert(bot_name_len + 2 < sizeof(name_buf));
	for(size_t i = 0; i < ARRAY_SIZE(name_pats); ++i){
		snprintf(name_buf, sizeof(name_buf), name_pats[i], bot_name);
		if(strcasestr(msg, name_buf)){
			found_name = true;
			break;
		}
	}

	if(found_name && markov_rand(3)){
		markov_reply(chan, name);
	}

	if(*msg == '@') *msg = ' ';
	
	for(char* p = msg; *p; ++p){
		if(*p < ' ' || *p >= 127) *p = ' ';
		else if(*p == '$') *p = '@';
	}

	markov_replace(&msg, ". ", " $ ");
	markov_replace(&msg, "! ", " $ ");
	markov_replace(&msg, "? ", " $ ");

	markov_replace(&msg, ",", " , ");

	for(char* p = msg; *p; ++p){
		if(strchr(".!?@:;`^(){}[]\"", *p)) *p = ' ';
	}

	markov_replace(&msg, "  ", " ");

	word_idx_t words[] = { start_sym_idx, start_sym_idx, 0 };

	char* state = NULL;
	char* word = strtok_r(msg, " ", &state);

	printf("Adding:");

	for(; word; word = strtok_r(NULL, " ", &state)){

		bool skip = false;
		for(const char** c = skip_words; *c; ++c){
			if(strcmp(word, *c) == 0){
				skip = true;
				break;
			}
		}

		if(skip) continue;

		printf(" [%s]", word);

		size_t len = strlen(word);
		word_idx_t idx = 0;

		if(len > 24){
			len = 9;
			idx = find_or_add_word("something", len);
		} else {
			//TODO: remove ++ --

			for(char** c = markov_nicks; c < sb_end(markov_nicks); ++c){
				if(strcasecmp(word, *c) == 0){
					skip = true;
					break;
				}
			}

			if(skip) continue;

			idx = find_or_add_word(word, len);
		}

		if(idx == end_sym_idx && words[1] == start_sym_idx) continue;

		if(idx == words[1] && words[1] == words[0]) continue;

		words[2] = idx;
		markov_add(words);

		if(idx == end_sym_idx){
			words[0] = start_sym_idx;
			words[1] = start_sym_idx;
		} else {
			words[0] = words[1];
			words[1] = words[2];
		}
	}

	puts(".");

	words[2] = end_sym_idx;
	if(words[1] != start_sym_idx) markov_add(words);

	if(markov_rand(msg_chance) == 0){
		markov_send(chan);
	}

	sb_free(msg);
}

static void markov_join(const char* chan, const char* name){

	if(strcasecmp(name, ctx->get_username()) == 0) return;

	for(int i = 0; i < sb_count(markov_nicks); ++i){
		if(strcasecmp(name, markov_nicks[i]) == 0){
			return;
		}
	}
	sb_push(markov_nicks, strdup(name));
}

static void markov_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "markov_gen") == 0){

		char* buffer = malloc(256);
		if(!markov_gen(buffer, 256)){
			free(buffer);
			return;
		}

		msg->callback((intptr_t)buffer, msg->cb_arg);
	}
}

static void markov_stdin(const char* msg){
	if(strcmp(msg, "msave") == 0){
		ctx->save_me();
	}
}

// }}}

