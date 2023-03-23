#include "markov.h"

inso_ht word_ht;
inso_ht chain_keys_ht;
sb(MarkovLinkVal) chain_vals;

sb(char) word_mem;

word_idx_t start_sym_idx;
word_idx_t end_sym_idx;

char rng_state_mem[256];
struct random_data rng_state;

const char* skip_words[] = { "p", "d", "b", "o", "-p", "-d", "-b", "-o", NULL };
