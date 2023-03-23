#ifndef MARKOV_H_
#define MARKOV_H_

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#define STB_SB_MMAP
#include "../stb_sb.h"
#include "../inso_ht.h"

#define MAX(a,b) ((a)>(b)?(a):(b))
#define countof(x) (sizeof(x)/sizeof(*(x)))

typedef uint32_t word_idx_t;

typedef struct __attribute__((packed)) {
	uint32_t val_idx;
	word_idx_t word_idx_1 : 24;
	word_idx_t word_idx_2 : 24;
} MarkovLinkKey;

typedef struct {
	word_idx_t word_idx : 24;
	uint8_t count;
	uint32_t next;
} MarkovLinkVal;

typedef struct {
	word_idx_t word_idx;
	uint32_t   total;
} WordInfo;

// variables (state.c)

extern inso_ht word_ht;
extern inso_ht chain_keys_ht;
extern sb(MarkovLinkVal) chain_vals;

extern sb(char) word_mem;

extern word_idx_t start_sym_idx;
extern word_idx_t end_sym_idx;

extern char rng_state_mem[256];
extern struct random_data rng_state;

extern const char* skip_words[];

// funcs

sb(char) markov_preprocess_msg(const char* msg);
void markov_learn_from_msg (const char* msg);
word_idx_t find_or_add_word (const char* word, size_t word_len, uint32_t* total);
word_idx_t find_word_any_case (const char* word, size_t word_len);
size_t markov_gen_3(char* buffer, size_t buffer_len, word_idx_t sym0, word_idx_t sym1);

#include "util.h"
#endif
