#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <dlfcn.h>
#include <string.h>
#include "../../src/module.h"
#include "../../src/stb_sb.h"

sb(char) escape_html(const char* in);

extern char _binary_template_htm_start[];
extern char _binary_template_htm_end[];

extern char _binary_perms_txt_start[];
extern char _binary_perms_txt_end[];

struct module {
	void* handle;
	IRCModuleCtx* ctx;
};
static sb(struct module) modules;

enum perm {
	P_NONE,
	P_WLIST     = 'W',
	P_WLIST_ARG = 'R',
	P_ADMIN     = 'A',
};

struct cmd_perm {
	char* cmd;
	enum perm perm;
};
static sb(struct cmd_perm) cmd_perms;

static void mod_load(const char* path){

	void* h = dlopen(path, RTLD_LOCAL | RTLD_NOW);
	if(!h){
		fprintf(stderr, "Error loading %s: %s\n", path, dlerror());
		return;
	}

	IRCModuleCtx* ctx = dlsym(h, "irc_mod_ctx");
	if(!ctx){
		fprintf(stderr, "Error looking up irc_mod_ctx symbol in %s: %s\n", path, dlerror());
		return;
	}

	struct module m = {
		.handle = h,
		.ctx = ctx,
	};

	sb_push(modules, m);
}

static int mod_sort(struct module* a, struct module* b){
	return strcmp(a->ctx->name, b->ctx->name);
}

static void mod_write(struct module* m, FILE* main, FILE* side){
	if(!m->ctx->commands || !m->ctx->commands[0])
		return;

	fprintf(side, "\t\t<li><a href=\"#%1$s\">%1$s</a></li>\n", m->ctx->name);
	fprintf(main, "\t\t<div id=\"%1$s\" class=\"mod-heading\">%1$s</div>\n", m->ctx->name);

	if(m->ctx->help_url){
		fprintf(main, "\t\t<a class=\"exthelp\" target=\"blank\" rel=\"noopener\" href=\"%1$s\">See %1$s</a>\n", m->ctx->help_url);
	}

	fprintf(main, "\t\t<table class=\"cmds\">\n\t\t\t<thead><th>cmd</th><th>args</th><th>desc</th><th>?</th></thead>\n\t\t\t<tbody>\n");

	for(int i = 0; m->ctx->commands[i]; ++i){

		struct cmd_perm* perm = NULL;

		// command list
		{
			sb(char*) cmd_list = NULL;

			char* cmds = strdup(m->ctx->commands[i]), *c = cmds, *p;
			int count = 0;

			while((p = strsep(&c, " \t\r\n"))){
				if(!*p)
					break;

				sb_push(cmd_list, p);
				sb_each(cp, cmd_perms){
					if(strcmp(cp->cmd, p) == 0){
						perm = cp;
						break;
					}
				}
			}

			if(perm && perm->perm != P_WLIST_ARG){
				fprintf(main, "\t\t\t\t<tr class=\"w\">\n");
			} else {
				fprintf(main, "\t\t\t\t<tr>\n");
			}

			fprintf(main, "\t\t\t\t\t<td>");

			sb_each(c, cmd_list){
				if(c != cmd_list)
					fprintf(main, ", ");
				fprintf(main, "<kbd>%s</kbd>", *c);
			}

			sb_free(cmd_list);
			free(cmds);
		}

		// args / desc

		if(m->ctx->cmd_help && m->ctx->cmd_help[i]){
			char* help = strdupa(m->ctx->cmd_help[i]);
			char* p = strstr(help, "| ");

			sb(char) args;
			sb(char) desc;

			if(p){
				*p = 0;
				args = escape_html(help);
				desc = escape_html(p+1);
			} else {
				sb_push(args, 0);
				desc = escape_html(help);
			}

			fprintf(main, "</td><td>%s</td><td>%s</td>", args, desc);

			sb_free(args);
			sb_free(desc);
		} else {
			fprintf(main, "</td><td></td><td>(nondescript)</td>");
		}

		// permissions icon

		if(perm){
			switch(perm->perm){
				case P_WLIST:
					fprintf(main, "<td title=\"Whitelisted\">&#x1f512;</td>");
					break;
				case P_WLIST_ARG:
					fprintf(main, "<td title=\"Whitelisted (when argument given)\">&#x1f513;</td>");
					break;
				case P_ADMIN:
					fprintf(main, "<td title=\"Admin only\">&#x1f9d9;</td>");
					break;
			}
		} else {
			fprintf(main, "<td></td>");
		}

		fprintf(main, "\n\t\t\t\t</tr>\n");
	}

	fprintf(main, "\t\t\t</tbody>\n\t\t</table>\n");
}

int main(int argc, char** argv){

	if(argc < 2){
		fprintf(stderr, "Usage: %s <module_directory>\n", argv[0]);
		return 1;
	}

	// load permissions
	{
		char* p = _binary_perms_txt_start;
		char* q = _binary_perms_txt_end;
		char* p1;

		while((p1 = memchr(p, '\n', q - p))){
			*p1 = 0;
			enum perm perm = P_NONE;

			char* c = memchr(p, ' ', q - p1);
			if(c){
				*c = 0;
				perm = c[1];
			}

			if(perm != P_NONE){
				struct cmd_perm cp = {
					.cmd = p,
					.perm = perm,
				};
				sb_push(cmd_perms, cp);
			}

			p = p1 + 1;
		}
	}

	// load modules

	DIR* dir = opendir(argv[1]);
	if(!dir){
		perror("opendir");
		return 1;
	}

	struct dirent* d;
	while((d = readdir(dir))){
		size_t sz = strlen(d->d_name);

		if(sz > 4 && memcmp(d->d_name + sz - 3, ".so", 4) == 0){
			char buf[PATH_MAX];
			snprintf(buf, sizeof(buf), "%s/%s", argv[1], d->d_name);
			mod_load(buf);
		}
	}

	closedir(dir);

	// generate documentation html

	struct stream {
		char*  data;
		size_t size;
		FILE*  file;
	} st_main, st_side;

	st_main.file = open_memstream(&st_main.data, &st_main.size);
	st_side.file = open_memstream(&st_side.data, &st_side.size);

	qsort(modules, sb_count(modules), sizeof(struct module), (int(*)())&mod_sort);

	sb_each(m, modules){
		mod_write(m, st_main.file, st_side.file);
	}

	fclose(st_main.file);
	fclose(st_side.file);

	// merge with page template and output to stdout

	char* p = _binary_template_htm_start;
	char* q = _binary_template_htm_end;

	while(p != q){
		char* p0 = strchr(p, '`');
		if(p0){
			printf("%.*s", (int)(p0 - p), p);

			char* p1 = strchr(p0+1, '`');
			if(!p1){
				fprintf(stderr, "Error: unmatched ` in template.\n");
				return 1;
			}

			char* key = strndupa(p0+1, p1-(p0+1));

			/****/ if(strcmp(key, "main") == 0){
				puts(st_main.data);
			} else if(strcmp(key, "side") == 0){
				puts(st_side.data);
			} else {
				fprintf(stderr, "Warning: Unknown key '%s', skipping.\n", key);
			}

			p = p1 + 1;
		} else {
			printf("%.*s", (int)(q - p), p);
			p = q;
		}
	}
}
