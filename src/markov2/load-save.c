#include "markov.h"
#include <zlib.h>

static bool markov_load(const char* file){
	gzFile f = gzopen(file, "rb");
	uint32_t word_size = 0, val_size = 0, version = 0;
	char fourcc[4];

#define GZREAD(f, ptr, sz) if(gzread(f, ptr, sz) < (int)(sz)) goto fail

	GZREAD(f, fourcc, 4);
	if(memcmp(fourcc, "IBMK", 4) != 0){
		fputs("markov_load: invalid file format.\n", stderr);
		goto fail;
	}

	GZREAD(f, &version, 4);
	if(version != 3){
		fputs("markov_load: invalid version.\n", stderr);
		goto fail;
	}

	GZREAD(f, &word_size, sizeof(word_size));
	GZREAD(f, &val_size , sizeof(val_size));

	//GZREAD(f, &word_max, sizeof(word_max));
	//GZREAD(f, &word_grand_total, sizeof(word_grand_total));

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

#if INSO_HT_VERSION == 2
	chain_keys_ht.used     /= chain_keys_ht.elem_size;
	chain_keys_ht.capacity /= chain_keys_ht.elem_size;

	word_ht.used     /= word_ht.elem_size;
	word_ht.capacity /= word_ht.elem_size;
#endif

#undef GZREAD

	gzclose(f);
	return true;

fail:
	printf("markov: couldn't read file.\n");
	gzclose(f);
	return false;
}

static bool markov_save(FILE* file){
	printf("mod_markov: now saving...\n");

	uint32_t word_size = sbmm_count(word_mem) - 1;
	uint32_t val_size  = sbmm_count(chain_vals);
	uint32_t version   = 3;

	while(inso_ht_tick(&chain_keys_ht));
	while(inso_ht_tick(&word_ht));

	gzFile f = gzdopen(dup(fileno(file)), "wb");

#define GZWRITE(f, ptr, sz) if(gzwrite(f, ptr, sz) < (int)(sz)) goto fail

	GZWRITE(f, "IBMK"  , 4);
	GZWRITE(f, &version, 4);

	GZWRITE(f, &word_size, sizeof(word_size));
	GZWRITE(f, &val_size , sizeof(val_size));

	//GZWRITE(f, &word_max, sizeof(word_max));
	//GZWRITE(f, &word_grand_total, sizeof(word_grand_total));

#if INSO_HT_VERSION == 2
	{
		uint32_t cap  = chain_keys_ht.capacity * chain_keys_ht.elem_size;
		uint32_t used = chain_keys_ht.used     * chain_keys_ht.elem_size;
		GZWRITE(f, &cap , 4);
		GZWRITE(f, &used, 4);
	}

	{
		uint32_t cap  = word_ht.capacity * word_ht.elem_size;
		uint32_t used = word_ht.used     * word_ht.elem_size;
		GZWRITE(f, &cap , 4);
		GZWRITE(f, &used, 4);
	}
#else
	GZWRITE(f, &chain_keys_ht.capacity, 4);
	GZWRITE(f, &chain_keys_ht.used    , 4);

	GZWRITE(f, &word_ht.capacity, 4);
	GZWRITE(f, &word_ht.used    , 4);
#endif

	GZWRITE(f, word_mem + 1, word_size);
	GZWRITE(f, chain_vals, sizeof(MarkovLinkVal) * val_size);

	GZWRITE(f, chain_keys_ht.memory, chain_keys_ht.capacity * chain_keys_ht.elem_size);
	GZWRITE(f, word_ht.memory, word_ht.capacity * word_ht.elem_size);

#undef GZWRITE

	printf("mod_markov: save complete.\n");
	gzclose(f);
	return true;

fail:
	printf("mod_markov: error saving file.\n");
	gzclose(f);
	return false;
}

