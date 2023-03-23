#include "markov.h"

static int debug;

static word_idx_t find_word_addref(const char* word, size_t word_len, uint32_t* total){
	WordInfo* info;

	size_t hash = markov_hash(word, word_len);
	char* ntword = strndupa(word, word_len);

	if((info = inso_ht_get(&word_ht, hash, &wordinfo_cmp, ntword))){
		info->total++;
		if(total){
			*total = info->total;
		}
		return info->word_idx;
	}

	return 0;
}

word_idx_t find_or_add_word(const char* word, size_t word_len, uint32_t* total){
	word_idx_t index;

	if(!(index = find_word_addref(word, word_len, total))){
		char* p = memcpy(sbmm_add(word_mem, word_len+1), word, word_len+1);
		index = p - word_mem;
		inso_ht_put(&word_ht, &(WordInfo){ index, 1 });
		if(total){
			*total = 1;
		}
	}

	return index;
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

		if(debug) {
			printf("put key: [%s:%d] [%s:%d]\n", word_mem + indices[0], indices[0], word_mem + indices[1], indices[1]);
		}

		MarkovLinkKey new_key = {
			.word_idx_1 = indices[0],
			.word_idx_2 = indices[1],
			.val_idx  = sbmm_count(chain_vals) - 1
		};
		inso_ht_put(&chain_keys_ht, &new_key);

	} else {
		bool found = false;
		ssize_t last_idx = -1;

		for(uint32_t i = key->val_idx; i != UINT32_MAX; i = chain_vals[i].next){
			if(chain_vals[i].word_idx == indices[2]){

				if(chain_vals[i].count == 255){
					// adjust all counts for this key >> 1
					for(uint32_t j = key->val_idx; j != UINT32_MAX; j = chain_vals[j].next){
						unsigned int c = chain_vals[j].count;
						chain_vals[j].count = MAX(c >> 1, 1);
					}
				}

				++chain_vals[i].count;
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

sb(char) markov_preprocess_msg(const char* _msg) {
	size_t msg_len = strlen(_msg);
	sb(char) msg = NULL;
	memcpy(sb_add(msg, msg_len + 1), _msg, msg_len + 1);

	// strip various punctuation
	if(*msg == '@') *msg = ' ';

	for(unsigned char* p = msg; *p; ++p){
		if(*p < ' ') *p = ' ';
	}

	markov_replace(&msg, ". ", " \003 ");
	markov_replace(&msg, "! ", " \003 ");
	markov_replace(&msg, "? ", " \003 ");

	markov_replace(&msg, ",", " , ");

	for(char* p = msg; *p; ++p){
		if(strchr(".!?@;`^$(){}[]\"", *p)) *p = ' ';
	}

	markov_replace(&msg, "  ", " ");

	return msg;
}

void markov_learn_from_msg(const char* _msg) {

	sb(char) msg = markov_preprocess_msg(_msg);

	// the chain to be added by markov_add(). key = [0]+[1], value = [2]
	word_idx_t words[] = { start_sym_idx, start_sym_idx, 0 };

	char* state = NULL;
	char* word = strtok_r(msg, " ", &state);

	for(; word; word = strtok_r(NULL, " ", &state)){

		// skip words from hardcoded list
		bool skip = false;
		for(const char** c = skip_words; *c; ++c){
			if(strcmp(word, *c) == 0){
				skip = true;
				break;
			}
		}
		if(skip) continue;

		size_t len = strlen(word);
		word_idx_t idx;
		uint32_t wcount;

		// change too long words into "something"...
		if(len > 24){
			len = 9;
			idx = find_or_add_word("something", len, &wcount);
		} else {
			idx = find_or_add_word(word, len, &wcount);
		}

		// skip empty sentences + triplicates
		if ((idx == end_sym_idx && words[1] == start_sym_idx) ||
			(idx == words[1]    && words[1] == words[0])){

			words[0] = words[1];
			words[1] = words[2];
			continue;
		}

		// skip single word sentences
		if (idx == end_sym_idx && words[0] == start_sym_idx) {

			words[0] = words[1];
			words[1] = words[2];
			continue;
		}

		words[2] = idx;
		markov_add(words);

		// shift words down, or start a new sentence if this was the end symbol.
		if(idx == end_sym_idx){
			words[0] = start_sym_idx;
			words[1] = start_sym_idx;
		} else {
			words[0] = words[1];
			words[1] = words[2];
		}
	}

	words[2] = end_sym_idx;
	if(words[1] != start_sym_idx) {
		markov_add(words);
	}

	sb_free(msg);
}
