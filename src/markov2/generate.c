#include "markov.h"

struct word_case_match {
	const char* target;
	word_idx_t indices[32];
	size_t totals[32];
	size_t count;
};

static bool wordinfo_case_scan(const void* elem, void* param) {
	struct word_case_match* match = param;
	const WordInfo* w = elem;

	if(match->count >= 32) {
		return 1;
	}

	if(strcasecmp(word_mem + w->word_idx, match->target) == 0) {
		match->indices[match->count] = w->word_idx;
		match->totals[match->count] = w->total;
		match->count++;
	}

	return 0;
}

word_idx_t find_word_any_case(const char* word, size_t word_len) {

	size_t hash = markov_hash(word, word_len);
	char* ntword = strndupa(word, word_len);

	struct word_case_match match = {
		.target = ntword,
	};

	inso_ht_get(&word_ht, hash, &wordinfo_case_scan, &match);

	size_t overall_total = 0;
	for(size_t i = 0; i < match.count; ++i) {
		overall_total += match.totals[i];
		//printf("match: %s: %zu\n", word_mem + match.indices[i], match.totals[i]);
	}

	if(overall_total == 0) {
		return 0;
	}

	int target = markov_rand(overall_total);
	word_idx_t idx = 0;
	for(size_t i = 0; i < match.count; ++i) {
		idx = match.indices[i];
		target -= match.totals[i];
		if(target < 0) {
			break;
		}
	}

	return idx;
}

size_t markov_gen_3(char* buffer, size_t buffer_len, word_idx_t sym0, word_idx_t sym1){
	if(!buffer_len) return 0;
	*buffer = 0;

	MarkovLinkKey* key = find_key(sym0, sym1);
	if(!key) {
		printf("(no key for [%s:%d/%s:%d])\n", word_mem + sym0, sym0, word_mem + sym1, sym1);
		return 0;
	} else {
		if(sym0 != start_sym_idx) {
			strcat(buffer, word_mem + sym0);
			if(word_needs_space(word_mem + sym1)) {
				strcat(buffer, " ");
			}
		}

		if(sym1 != start_sym_idx) {
			strcat(buffer, word_mem + sym1);
		}
	}

	const size_t max_len = 20;

	for(size_t nwords = 0; nwords < max_len; ++nwords){
		size_t total = 0;

		MarkovLinkVal* val = chain_vals + key->val_idx;

		do {
			total += val->count;
		} while(val->next != UINT32_MAX && (val = chain_vals + val->next));

		assert(total);

		const char* word;

		// try a few times to get a good word
		for(size_t picks = 0; picks < 5; ++picks){
			ssize_t count = markov_rand(total);

			val = chain_vals + key->val_idx;
			word = word_mem + val->word_idx;
			ssize_t sub = val->count;

			while((count -= sub) >= 0){
				val = chain_vals + val->next;
				word = word_mem + val->word_idx;
				sub = val->count;
			}
		}

		if(val->word_idx == end_sym_idx){
			//printf("landed on end sym natural\n");
			break;
		}

		if(*buffer && word_needs_space(word)){
			strcat(buffer, " ");
		}
		strcat(buffer, word);

		word_idx_t tmp = key->word_idx_2;
		key = find_key(key->word_idx_2, val->word_idx);
		if(!key){
			printf("NO KEY?! [%s] [%s]\n", word_mem + tmp, word_mem + val->word_idx);
			break;
		}
	}

	return strlen(buffer);
}

static size_t markov_gen_2(char* buffer, size_t buffer_len, word_idx_t start){
	return markov_gen_3(buffer, buffer_len, start_sym_idx, start);
}

static size_t markov_gen(char* buffer, size_t buffer_len){
	return markov_gen_2(buffer, buffer_len, start_sym_idx);
}

static const char* markov_get_punct(){
	size_t val = markov_rand(100);

	if(val < 25) return "";
	if(val < 35) return "!";
	if(val < 45) return " ...";
	if(val < 50) return " TehePelo";
	if(val < 55) return " PunOko";
	if(val < 60) return " Kyaruok";
	if(val < 65) return " SMH";
	if(val < 70) return " ChenBased";
	if(val < 75) return " NOTED";
	if(val < 80) return " KannaSip";
	if(val < 85) return " SataniaCry";
	if(val < 90) return " GearScare";
	if(val < 92) return " MyHonestReaction";
	if(val < 94) return " JahyTrip";
	if(val < 96) return " monkaLaugh";
	if(val < 98) return " comfyWorryClap";

	return " NepGlare";
}

static size_t markov_gen_prefix(const char* prefix, char* output, size_t output_len) {

	char* out = output;
	char* end = out + output_len;

	sb(char) msg = markov_preprocess_msg(prefix);
	char* state = NULL;

	word_idx_t words[] = { start_sym_idx, start_sym_idx, start_sym_idx };

	char* word = strtok_r(msg, " ", &state);
	if(!word) {
		return 0;
	}

	for(; word; word = strtok_r(NULL, " ", &state)) {

		word_idx_t idx = find_word_any_case(word, strlen(word));
		if(idx == 0) {
			return 0;
		}

		words[0] = words[1];
		words[1] = words[2];
		words[2] = idx;

		if(words[0] != start_sym_idx) {
			int len = snprintf(out, end - out, "%s", word_mem + words[0]);
			out += len;

			if(word_needs_space(word_mem + words[1])) {
				*out++ = ' ';
				*out = '\0';
			}
		}
	}

	size_t len = markov_gen_3(out, end - out, words[1], words[2]);
	sb_free(msg);

	return len;
}
