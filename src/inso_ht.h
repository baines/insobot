#ifndef INSO_HT_H_
#define INSO_HT_H_
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

// Interface

typedef struct {
	size_t capacity;
	size_t prev_cap;
	size_t used;
	size_t elem_size;
	size_t rehash_idx;
	size_t (*hash_fn)(const void*);
	char*  memory;
	char*  prev_memory;

	// set these manually before init if you want custom allocation
	void* (*alloc_fn)(size_t);
	void  (*free_fn)(void*, size_t);
} inso_ht;

#ifndef NDEBUG
	#include <stdio.h>
	#define INSO_HT_DBG(fmt, ...) printf(fmt, ##__VA_ARGS__);
#else
	#define INSO_HT_DBG(fmt, ...)
#endif

typedef size_t (*inso_ht_hash_fn) (const void* entry);
typedef bool   (*inso_ht_cmp_fn)  (const void* entry, void* param);

void  inso_ht_init (inso_ht*, size_t nmemb, size_t size, inso_ht_hash_fn);
void  inso_ht_free (inso_ht*);
void* inso_ht_put  (inso_ht*, const void* elem);
void* inso_ht_get  (inso_ht*, size_t hash, inso_ht_cmp_fn, void* param);
bool  inso_ht_del  (inso_ht*, size_t hash, inso_ht_cmp_fn, void* param);
bool  inso_ht_tick (inso_ht*);

#endif

#define INSO_HT_VERSION 2

// Implementation

#ifdef INSO_IMPL

static inline void*  inso_htpriv_put   (inso_ht*, const void*);
static inline bool   inso_htpriv_get_i (inso_ht*, intptr_t*, size_t, inso_ht_cmp_fn, void*);
static inline void   inso_htpriv_del_i (inso_ht*, intptr_t);
static inline bool   inso_htpriv_empty (inso_ht*, const char*);
static inline size_t inso_htpriv_align (size_t);

void inso_ht_init(inso_ht* ht, size_t nmemb, size_t size, inso_ht_hash_fn hash_fn){
	assert(ht);
	memset(ht, 0, sizeof(*ht));

	nmemb = inso_htpriv_align(nmemb);

	ht->capacity  = nmemb;
	ht->elem_size = size;
	ht->hash_fn   = hash_fn;

	if(ht->alloc_fn && ht->free_fn){
		ht->memory = ht->alloc_fn(ht->capacity * ht->elem_size);
	} else {
		ht->memory = calloc(ht->capacity, ht->elem_size);
	}

	INSO_HT_DBG("ht_init: nmemb: %zu, cap: %zu\n", nmemb, ht->capacity);

	assert(ht->memory);
}

void inso_ht_free(inso_ht* ht){
	if(ht && ht->memory){
		bool custom = ht->alloc_fn && ht->free_fn;

		if(custom){
			ht->free_fn(ht->memory, ht->capacity * ht->elem_size);
		} else {
			free(ht->memory);
		}

		if(ht->prev_memory){
			if(custom){
				ht->free_fn(ht->prev_memory, ht->prev_cap * ht->elem_size);
			} else {
				free(ht->prev_memory);
			}
		}
		memset(ht, 0, sizeof(*ht));
	}
}

void* inso_ht_put(inso_ht* ht, const void* elem){
	assert(ht);
	assert(ht->memory);
	inso_ht_tick(ht);
	
	if(ht->used && (float)ht->used / (float)ht->capacity > 0.75f){

		while(inso_ht_tick(ht));

		ht->prev_cap    = ht->capacity;
		ht->capacity   *= 2;
		ht->prev_memory = ht->memory;
		ht->rehash_idx  = 0;
		ht->used        = 0;
		
		if(ht->alloc_fn && ht->free_fn){
			ht->memory = ht->alloc_fn(ht->capacity * ht->elem_size);
		} else {
			ht->memory = calloc(ht->capacity, ht->elem_size);
		}

		INSO_HT_DBG("ht_put: expanding table. %zu -> %zu\n", ht->prev_cap, ht->capacity);

		assert(ht->memory);
	}

	return inso_htpriv_put(ht, elem);
}


void* inso_ht_get(inso_ht* ht, size_t hash, inso_ht_cmp_fn cmp, void* param){
	assert(ht);
	assert(ht->memory);
	inso_ht_tick(ht);

	intptr_t index;
	if(inso_htpriv_get_i(ht, &index, hash, cmp, param)){
		char* mem = ht->memory;

		if(index < 0){
			index = -(index+1);
			mem = ht->prev_memory;
		}

		return mem + index * ht->elem_size;
	}

	return NULL;
}

bool inso_ht_del(inso_ht* ht, size_t hash, inso_ht_cmp_fn cmp, void* param){
	assert(ht);
	assert(ht->memory);
	inso_ht_tick(ht);

	size_t index;
	if(inso_htpriv_get_i(ht, &index, hash, cmp, param)){
		inso_htpriv_del_i(ht, index);
		return true;
	}

	return false;
}

bool inso_ht_tick(inso_ht* ht){
	if(!ht->prev_memory) return false;

	size_t i;
	for(i = ht->rehash_idx; i < ht->prev_cap; ++i){
		void* ptr = ht->prev_memory + i * ht->elem_size;
		if(!inso_htpriv_empty(ht, ptr)){
			inso_htpriv_put(ht, ptr);
			ht->rehash_idx = ++i;
			break;
		}
	}

	if(i >= ht->prev_cap){
		if(ht->alloc_fn && ht->free_fn){
			ht->free_fn(ht->prev_memory, ht->prev_cap * ht->elem_size);
		} else {
			free(ht->prev_memory);
		}
		ht->prev_memory = NULL;
		INSO_HT_DBG("done rehashing table.\n");
	}

	return ht->prev_memory;
}

//////////////////////////////////

static inline void* inso_htpriv_put(inso_ht* ht, const void* elem){

	// TODO: robin-hood hashing?

	size_t hash  = ht->hash_fn(elem);

	for(size_t count = 0; count < ht->capacity; ++count){
		size_t i  = (hash + count) % ht->capacity;
		void* ptr = ht->memory + i * ht->elem_size;

		if(inso_htpriv_empty(ht, ptr)){
			ht->used++;
			return memcpy(ptr, elem, ht->elem_size);
		}
	}

	assert(!"ht_put: no space? wat.");

	return NULL;
}

// TODO: i think the negative index == in secondary table was a bad idea, just make this rehash
// and return the new index?

static inline bool inso_htpriv_get_i(inso_ht* ht, intptr_t* idx, size_t hash, inso_ht_cmp_fn cmp, void* param){

	char*  mem[] = { ht->memory   , ht->prev_memory };
	size_t cap[] = { ht->capacity , ht->prev_cap    };

	for(size_t n = 0; n < 2; ++n){
		size_t limit = cap[n];

		for(size_t count = 0; count < limit; ++count){
			size_t i  = (hash + count) % limit;
			void* ptr = mem[n] + i * ht->elem_size;

			if(inso_htpriv_empty(ht, ptr)) break;

			if((!n || i >= ht->rehash_idx) && cmp(ptr, param)){
				*idx = n ? -(i+1) : i;
				return true;
			}
		}

		if(!ht->prev_memory) break;
	}

	return false;
}

static inline void inso_htpriv_del_i(inso_ht* ht, intptr_t idx){

	if(idx < 0){
		memset(ht->prev_memory - (idx+1), 0, ht->elem_size);
		return;
	}

	memset(ht->memory + idx * ht->elem_size, 0, ht->elem_size);
	INSO_HT_DBG("ht_del: starting. idx=%zu, cap=%zu\n", idx, ht->capacity);

	for(size_t count = 1; count < ht->capacity; ++count){
		size_t i  = (idx + count) % ht->capacity;
		void* ptr = ht->memory + i * ht->elem_size;

		if(inso_htpriv_empty(ht, ptr)){
			INSO_HT_DBG("ending\n");
			return;
		}

		size_t hash = ht->hash_fn(ptr);
		size_t idx2 = hash % ht->capacity;

		INSO_HT_DBG("hash=%zu, idx2=%zu, ", hash, idx2);

		if(idx2 <= (size_t)idx){
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

	assert(!"ht_del: not found and no empty slots? wtf");
}

static inline size_t inso_htpriv_align(size_t i){
	size_t shift = __builtin_clzl(i - 1) ^ (__WORDSIZE - 1);
	return UINTMAX_C(1) << ++shift;
}

static inline bool inso_htpriv_empty(inso_ht* ht, const char* ptr){

	const size_t len4 = ht->elem_size >> 2;
	for(const uint32_t *p = (const uint32_t*)ptr, *q = (const uint32_t*)(ptr + ht->elem_size); p < q; ++p){
		if(*p) return false;
	}

	char a = 0;
	const char* p = ptr + len4;
	switch(ht->elem_size & 3){
		case 3: a |= *p++;
		case 2: a |= *p++;
		case 1: a |= *p++;
	}

	return a == 0;
}

#endif
