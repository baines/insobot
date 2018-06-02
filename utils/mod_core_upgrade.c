#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <sys/stat.h>
#include <argz.h>

bool argz_find(char* argz, size_t len, const char* needle){
	char* p = NULL;
	while((p = argz_next(argz, len, p))){
		if(strcmp(p, needle) == 0) break;
	}
	return p;
}

int main(void){

	FILE* out;
	{
		struct stat st;
		if(stat("data", &st) == -1 || !S_ISDIR(st.st_mode)){
			fprintf(stderr, "Can't find data/ dir. Are you running this from the right place?\n");
			return 1;
		}

		out = fopen("data/core.data", "r");
		if(out){
			fprintf(stderr, "data/core.data already exists, exiting.\n");
			return 1;
		}

		out = fopen("data/core.data", "w");
		if(!out){
			fprintf(stderr, "can't open data/core.data for writing.\n");
			return 1;
		}
	}

	puts("Starting conversion...");

	char* chans = NULL;
	size_t chans_len = 0;

	FILE* in = fopen("data/chans.data", "r");
	if(in){
		char* c;
		while(fscanf(in, "%ms", &c) == 1){
			argz_add(&chans, &chans_len, c);
			free(c);
		}

		fclose(in);
	}

	char* line = NULL;
	size_t line_len = 0;

	in = fopen("data/meta.data", "r");
	if(in){
		while(getline(&line, &line_len, in) != -1){
			char* chan = strtok(line, "\r\n\t ");
			if(!chan) continue;

			if(!argz_find(chans, chans_len, chan)){
				fputc(';', out);
			}

			fprintf(out, "%s\t", chan);

			char* mod;
			while((mod = strtok(NULL, "\r\n\t "))){
				fprintf(out, "%s ", mod);
			}
			fputc('\n', out);
		}
	}

	puts("Done!");

	return 0;
}
