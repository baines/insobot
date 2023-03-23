
static inline uint32_t markov_hash(const char* str, size_t len){
	uint32_t hash = 6159;
	for(size_t i = 0; i < len; ++i){
		hash = hash * 187 + tolower(str[i]);
	}
	return hash;
}

static inline size_t wordinfo_hash(const void* arg){
	const WordInfo* w = arg;
	return markov_hash(word_mem + w->word_idx, strlen(word_mem + w->word_idx));
}

static inline bool wordinfo_cmp(const void* elem, void* param){
	const char* str = param;
	const WordInfo* w = elem;
	return strcmp(str, word_mem + w->word_idx) == 0;
}

static inline uint32_t hash6432shift(uint64_t key){
	key = (~key) + (key << 18);
	key = key ^ (key >> 31);
	key = key * 21;
	key = key ^ (key >> 11);
	key = key + (key << 6);
	key = key ^ (key >> 22);
	return (uint32_t)key;
}

static inline size_t chain_key_hash(const void* arg){
	const MarkovLinkKey* k = arg;
	return hash6432shift(((uint64_t)k->word_idx_1 << 32UL) | k->word_idx_2);
}

static inline bool chain_key_cmp(const void* elem, void* param){
	uint64_t* id = param;
	const MarkovLinkKey* key = elem;
	return (((uint64_t)key->word_idx_1 << 32UL) | key->word_idx_2) == *id;
}

static inline void* ht_alloc(size_t n){
	void* mem = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(mem != MAP_FAILED);
	return mem;
}

static inline void ht_free(void* p, size_t n){
	assert(munmap(p, n) == 0);
}

static inline void markov_replace(char** msg, const char* from, const char* to){
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

static inline uint32_t markov_rand(uint32_t limit){
	int32_t x;

	do {
		random_r(&rng_state, &x);
	} while ((size_t)x >= (RAND_MAX - RAND_MAX % limit));

	return x % limit;
}

static inline MarkovLinkKey* find_key(word_idx_t a, word_idx_t b){
	uint64_t id = ((uint64_t)a << 32UL) | b;
	return inso_ht_get(&chain_keys_ht, hash6432shift(id), &chain_key_cmp, &id);
}

static inline bool word_needs_space(const char* word) {
	return strcmp(word, ",") != 0;
}
