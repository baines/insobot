#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "module.h"
#include "utils.h"
#include "ctype.h"

static void haiku_cmd (const char*, const char*, const char*, int);
static bool haiku_init(const IRCCoreCtx*);

enum { HAIKU, SYLLABLE_COUNT };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "haiku",
	.desc       = "(poorly) generate haikus",
	.on_cmd     = &haiku_cmd,
	.on_init    = &haiku_init,
	.commands   = DEFINE_CMDS (
		[HAIKU]          = CMD("haiku"),
		[SYLLABLE_COUNT] = CMD("scount")
	)
};

static const IRCCoreCtx* ctx;

static const char vowels[] = "aeiouy";

// this is a load of crap
static int syllable_estimate(const char* _word){
	int count = 0;
	bool found_vowel = false;
	bool prev_vowel = false;
	bool ends_in_e = false;
	int consecutive_consonants = 0;

	char* word = strdupa(_word);

	for(char* c = word; *c; ++c){

		*c = tolower(*c);

		if(
			(c == word && *c == 'y') ||
			((isalpha(*c) || ispunct(*c)) && !strchr(vowels, *c)))
		{
			consecutive_consonants++;
			prev_vowel = false;
			continue;
		}

		if(
			consecutive_consonants < 2 &&
			*c == 'e' &&
			(c[1] == '\0' ||
			(c[1] == 's' && c[2] == '\0') ||
			(c[1] == 'd' && c[2] == '\0') ||
			strcmp(c, "e's") == 0 ||
			strcmp(c, "e'd") == 0)
		){
			ends_in_e = true;
			break;
		}

		consecutive_consonants = 0;

		if(isdigit(*c)){
			prev_vowel = false;
		} else {
			found_vowel = true;
		}

		if(prev_vowel){
			prev_vowel = false;
		} else {
			count++;
			prev_vowel = true;
		}
	}

	if(count == 0 || !found_vowel){
		if(ends_in_e){
			count = 1;
		} else {
			count = 0;
			for(const char* c = word; *c; ++c){
				if(isalnum(*c) || !strchr("'-,/~", *c)) ++count;
			}
		}
	}

	return count;
}

static void haiku_markov_cb(intptr_t result, intptr_t arg){
	if(result && !*(char*)arg){
		*(char**)arg = (char*)result;
	} else if(result){
		free((char*)result);
	}
}

static void haiku_cmd (const char* chan, const char* name, const char* arg, int cmd){
	if(!inso_is_wlist(ctx, name)) return;

	if(cmd == SYLLABLE_COUNT && *arg++){
		char* word = strndupa(arg, strchrnul(arg, ' ') - arg);
		ctx->send_msg(chan, "%s: I think [%s] has %d syllables.", name, word, syllable_estimate(word));
		return;
	}

	if(cmd != HAIKU) return;

	int syl_required[] = { 5, 7, 5 };
	char output[256] = {};

	char* state;
	char* markov_sentence = NULL;
	MOD_MSG(ctx, "markov_gen", 0, &haiku_markov_cb, &markov_sentence);
	if(!markov_sentence){
		ctx->send_msg(chan, "Alas this module / requires mod_markov as well. / No haikus for you.");
		puts("mod_haiku: null markov gen?");
		return;
	}

	char* word = strtok_r(markov_sentence, " ", &state);

	for(int i = 0; i < ARRAY_SIZE(syl_required); ++i){
		int syl = 0;
		char buffer[256] = {};

		if(word){
			*word = toupper(*word);
		}

		while(true){
			while(!word){
				free(markov_sentence);
				markov_sentence = NULL;

				MOD_MSG(ctx, "markov_gen", 0, &haiku_markov_cb, &markov_sentence);
				if(!markov_sentence){
					puts("mod_haiku: null markov gen?");
					return;
				}
				word = strtok_r(markov_sentence, " ", &state);
				*word = toupper(*word);
			}

			int n = syllable_estimate(word);

			if(*buffer)	inso_strcat(buffer, sizeof(buffer), " ");
			inso_strcat(buffer, sizeof(buffer), word);
			
			if((word = strtok_r(NULL, " ", &state)) == NULL){
				inso_strcat(buffer, sizeof(buffer), ".");
			}
			
			syl += n;
			if(syl + n >= syl_required[i]){
				break;
			}
		}

		if(syl == syl_required[i]){
			inso_strcat(output, sizeof(output), buffer);
			if(i != ARRAY_SIZE(syl_required) - 1){
				inso_strcat(output, sizeof(output), " / ");
			}
		} else {
			--i;
		}
	}

	free(markov_sentence);
	ctx->send_msg(chan, "%s.", output);
}

static bool haiku_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

