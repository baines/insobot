#ifndef INSO_HT_H_
#define INSO_HT_H_

#ifdef INSO_HT_SPLIT
	#define INSO_HT_DECL
#else
	#define INSO_HT_DECL static inline
#endif

// Interface

typedef struct {
	size_t capacity;
	size_t orig_cap;
	size_t used;
	size_t offset;
	size_t elem_size;
	size_t (*hash_fn)(const void*);
	char*  memory;
} inso_ht;

#ifndef NDEBUG
	#define INSO_HT_DBG(fmt, ...) printf(fmt, ##__VA_ARGS__);
#else
	#define INSO_HT_DBG(fmt, ...)
#endif

typedef size_t (*inso_ht_hash_fn) (const void* entry);
typedef bool   (*inso_ht_cmp_fn)  (const void* entry, void* param);

INSO_HT_DECL void inso_ht_init (inso_ht*, size_t nmemb, size_t size, inso_ht_hash_fn);
INSO_HT_DECL void inso_ht_free (inso_ht*);
INSO_HT_DECL int  inso_ht_put  (inso_ht*, const void* elem);
INSO_HT_DECL bool inso_ht_get  (inso_ht*, void* outp, size_t hash, inso_ht_cmp_fn, void* param);
INSO_HT_DECL bool inso_ht_del  (inso_ht*, size_t hash, inso_ht_cmp_fn, void* param);
//INSO_HT_DECL int  inso_ht_ctl  (inso_ht*, int cmd, ...);

// Implementation

#if !defined(INSO_HT_SPLIT) || defined(INSO_HT_IMPL)

INSO_HT_DECL bool   inso_htpriv_get_i (inso_ht*, size_t*, size_t, inso_ht_cmp_fn, void*);
INSO_HT_DECL void   inso_htpriv_del_i (inso_ht*, size_t);
INSO_HT_DECL size_t inso_htpriv_align (size_t);

INSO_HT_DECL void inso_ht_init(inso_ht* ht, size_t nmemb, size_t size, inso_ht_hash_fn hash_fn){

	nmemb = inso_htpriv_align(nmemb);

	ht->capacity  = size * nmemb;
	ht->orig_cap  = ht->capacity;
	ht->used      = 0;
	ht->offset    = 0;
	ht->elem_size = size;
	ht->hash_fn   = hash_fn;
	ht->memory    = calloc(ht->capacity, 1);

	INSO_HT_DBG("ht_init: nmemb: %zu, cap: %zu\n", nmemb, ht->capacity);

	assert(ht->memory);
}

INSO_HT_DECL void inso_ht_free(inso_ht* ht){
	if(ht && !ht->memory){
		free(ht->memory);
		ht->memory = 0;
	}
}

INSO_HT_DECL int inso_ht_put(inso_ht* ht, const void* elem){
	assert(ht);
	assert(ht->memory);

	if(ht->used && (float)ht->used / (float)ht->capacity > 0.75f){

		ht->offset    = ht->capacity;
		ht->capacity *= 2;
		ht->memory    = realloc(ht->memory, ht->capacity);

		INSO_HT_DBG("ht_put: expanding table. off: %zu, cap: %zu\n", ht->offset, ht->capacity);

		assert(ht->memory);
		memset(ht->memory + ht->offset, 0, ht->capacity - ht->offset);
	}

	size_t limit = (ht->capacity - ht->offset) / ht->elem_size;
	size_t hash  = ht->hash_fn(elem);
	size_t off   = ht->offset / ht->elem_size;
	size_t coll  = 0;

	for(size_t count = 0; count < limit; ++count){
		size_t i = off + (hash + count) % limit;

		unsigned char v = 0;
		for(size_t j = 0; j < ht->elem_size; ++j){
			v |= ht->memory[i * ht->elem_size + j];
		}

		if(!v){
			memcpy(ht->memory + (i * ht->elem_size), elem, ht->elem_size);
			ht->used += ht->elem_size;
			return coll;
		} else {
			++coll;
		}
	}

	asm("int3");

	return 0;
}

INSO_HT_DECL bool inso_ht_get(inso_ht* ht, void* outp, size_t hash, inso_ht_cmp_fn cmp, void* param){
	assert(ht);
	assert(ht->memory);

	size_t index;
	if(inso_htpriv_get_i(ht, &index, hash, cmp, param)){
		memcpy(outp, ht->memory + index * ht->elem_size, ht->elem_size);
		return true;
	}

	return false;
}

INSO_HT_DECL bool inso_ht_del(inso_ht* ht, size_t hash, inso_ht_cmp_fn cmp, void* param){
	assert(ht);
	assert(ht->memory);

	size_t index;
	if(inso_htpriv_get_i(ht, &index, hash, cmp, param)){
		inso_htpriv_del_i(ht, index);
		return true;
	}

	return false;
}

//////////////////////////////////

INSO_HT_DECL bool
inso_htpriv_get_i(inso_ht* ht, size_t* idx, size_t hash, inso_ht_cmp_fn cmp, void* param){
	// TODO optimization:
	// MRU moving (robin-hood hashing)
	// use priority func set by inso_ht_ctl instead of MRU if set.
	
	for(size_t off = 0, cap = ht->orig_cap; cap <= ht->capacity; off = cap, cap *= 2){
		size_t limit = (cap - off) / ht->elem_size;

		INSO_HT_DBG("ht_get_i: off=%zu, cap=%zu, limit=%zu\n", off, cap, limit);

		for(size_t count = 0; count < limit; ++count){
			size_t i = (off / ht->elem_size) + (hash + count) % limit;

			unsigned char v = 0;
			for(size_t j = 0; j < ht->elem_size; ++j){
				v |= ht->memory[i * ht->elem_size + j];
			}

			if(!v) break;

			if(cmp(ht->memory + i * ht->elem_size, param)){
				*idx = i;
				return true;
			}
		}
	}

	return false;
}

INSO_HT_DECL void
inso_htpriv_del_i(inso_ht* ht, size_t idx){

	memset(ht->memory + idx * ht->elem_size, 0, ht->elem_size);

	size_t cap = ht->orig_cap / ht->elem_size;
	size_t off = 0;

	while(cap <= idx){
		off = cap;
		cap *= 2;
	}

	size_t limit = cap - off;

	INSO_HT_DBG("ht_del: starting. idx=%zu, cap=%zu, off=%zu, lim=%zu.\n", idx, cap, off, limit);

	for(size_t count = 1; count < limit; ++count){
		size_t i = off + (idx - off + count) % limit;
		
		unsigned char v = 0;
		for(size_t j = 0; j < ht->elem_size; ++j){
			v |= ht->memory[i * ht->elem_size + j];
		}

		INSO_HT_DBG("ht_del: %zu: v=%02x, idx=%zu, ", i, v, idx);

		if(!v){
			INSO_HT_DBG("ending\n");
			return;
		}

		size_t hash = ht->hash_fn(ht->memory + i * ht->elem_size);
		size_t idx2 = off + hash % limit;

		INSO_HT_DBG("hash=%zu, idx2=%zu, ", hash, idx2);

		if(idx2 <= idx){
			INSO_HT_DBG("swapping.");
			memcpy(
				ht->memory + idx  * ht->elem_size,
				ht->memory + idx2 * ht->elem_size,
				ht->elem_size
			);
			memset(ht->memory + idx2 * ht->elem_size, 0, ht->elem_size);
			idx = i;
		}
		INSO_HT_DBG("\n");
	}

	asm("int3");
}

INSO_HT_DECL size_t inso_htpriv_align(size_t i){
	size_t shift = __builtin_clzl(i - 1) ^ (__WORDSIZE - 1);
	return UINTMAX_C(1) << ++shift;
}

#endif
#endif
