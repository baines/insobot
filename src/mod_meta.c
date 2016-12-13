#include "module.h"
#include "stb_sb.h"
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "config.h"
#include "inso_utils.h"

static void meta_cmd   (const char*, const char*, const char*, int);
static bool meta_save  (FILE*);
static bool meta_check (const char*, const char*, int);
static bool meta_init  (const IRCCoreCtx*);
static void meta_join  (const char*, const char*);
static void meta_quit  (void);

enum { CMD_MODULES, CMD_MOD_ON, CMD_MOD_OFF, CMD_MOD_INFO };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "meta",
	.desc     = "Manages channel permissons of other modules",
	.priority = 10000,
	.flags    = IRC_MOD_GLOBAL,
	.on_cmd   = &meta_cmd,
	.on_save  = &meta_save,
	.on_meta  = &meta_check,
	.on_init  = &meta_init,
	.on_join  = &meta_join,
	.on_quit  = &meta_quit,
	.commands = DEFINE_CMDS (
		[CMD_MODULES]  = CONTROL_CHAR"m "     CONTROL_CHAR"modules",
		[CMD_MOD_ON]   = CONTROL_CHAR"mon "   CONTROL_CHAR"modon",
		[CMD_MOD_OFF]  = CONTROL_CHAR"moff "  CONTROL_CHAR"modoff",
		[CMD_MOD_INFO] = CONTROL_CHAR"minfo " CONTROL_CHAR"modinfo"
	)
};

static const IRCCoreCtx* ctx;

static char** channels;
static char*** enabled_mods_for_chan;

static char*** get_enabled_modules(const char* chan){
	for(int i = 0; i < sb_count(channels); ++i){
		if(strcmp(channels[i], chan) == 0) return enabled_mods_for_chan + i;
	}
	return NULL;
}

static void free_all_strs(char** strs){
	for(int i = 0; i < sb_count(strs); ++i){
		free(strs[i]);
	}
}

static void meta_quit(void){
	free_all_strs(channels);
	sb_free(channels);

	for(int i = 0; i < sb_count(enabled_mods_for_chan); ++i){
		free_all_strs(enabled_mods_for_chan[i]);
		sb_free(enabled_mods_for_chan[i]);
	}
	sb_free(enabled_mods_for_chan);
}

static bool reload_file(void){
	assert(ctx);

	int fd = open(ctx->get_datafile(), O_RDONLY);
	if(fd < 0) return false;

	char* file_contents = NULL;
	char buff[256];

	int rd = 0;
	while((rd = read(fd, buff, sizeof(buff))) > 0){
		memcpy(sb_add(file_contents, rd), buff, rd);
	}

	close(fd);
	sb_push(file_contents, 0);

	char* state = NULL;
	char* line = strtok_r(file_contents, "\r\n", &state);

	int chan_index = 0;

	meta_quit();

	while(line){
		char* line_state = NULL;
		char* word = strtok_r(line, " \t", &line_state);

		sb_push(channels, strdup(word));
		sb_push(enabled_mods_for_chan, 0);

		while((word = strtok_r(NULL, " \t", &line_state))){
			sb_push(enabled_mods_for_chan[chan_index], strdup(word));
		}

		line = strtok_r(NULL, "\r\n", &state);
		++chan_index;
	}

	sb_free(file_contents);

	return true;
}
 
static bool meta_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return reload_file();
}

static char** mod_find(char** haystack, const char* needle){
	for(int i = 0; i < sb_count(haystack); ++i){
		if(strcmp(haystack[i], needle) == 0) return haystack + i;
	}
	return NULL;
}

static void meta_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool has_cmd_perms = strcasecmp(chan+1, name) == 0 || inso_is_wlist(ctx, name);
	if(!has_cmd_perms) return;

	IRCModuleCtx** all_mods = ctx->get_modules(true);
	char***        our_mods = get_enabled_modules(chan);

	// this shouldn't happen, on_join should get the channel name before this can be called
	if(!our_mods){
		fprintf(stderr, "BUG: mod_meta.c: %s isn't on our list. Fix it!\n", chan);
		return;
	}

	char buff[1024];
	char *b = buff;
	size_t buflen = sizeof(buff);

	snprintf_chain(&b, &buflen, "Modules for %s: ", chan);

	switch(cmd){
		case CMD_MODULES: {
			for(; *all_mods; ++all_mods){
				const char* box = mod_find(*our_mods, (*all_mods)->name) ? "☑" : "☐";
				snprintf_chain(&b, &buflen, "%s %s, ", box, (*all_mods)->name);
			}
			ctx->send_msg(chan, "%s", buff);
		} break;

		case CMD_MOD_ON: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Which module?", name);
				break;
			}
			bool found = false;
			for(; *all_mods; ++all_mods){
				if(strcmp((*all_mods)->name, arg) == 0){
					found = true;
					if(mod_find(*our_mods, arg)){
						ctx->send_msg(chan, "%s: That module is already enabled here!", name);
					} else {
						sb_push(*our_mods, strdup(arg));
						ctx->send_msg(chan, "%s: Enabled module %s.", name, arg);
						ctx->save_me();
					}
				}
			}
			if(!found){
				ctx->send_msg(chan, "%s: I haven't heard of that module...", name);
			}
		} break;

		case CMD_MOD_OFF: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Which module?", name);
				break;
			}
			bool found = false;
			for(; *all_mods; ++all_mods){
				if(strcmp((*all_mods)->name, arg) == 0){
					found = true;
					char** m = mod_find(*our_mods, (*all_mods)->name);
					if(m){
						free(*m);
						sb_erase(*our_mods, m - *our_mods);
						ctx->send_msg(chan, "%s: Disabled module %s.", name, (*all_mods)->name);
						ctx->save_me();
					} else {
						ctx->send_msg(chan, "%s: That module is already disabled here!", name);
					}
				}
			}
			if(!found){
				ctx->send_msg(chan, "%s: I haven't heard of that module...", name);
			}
		} break;

		//XXX: this should maybe be part of mod_help instead?
		case CMD_MOD_INFO: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Which module?", name);
				break;
			}

			//XXX: all_mods was a bad choice of name for only channel mods...
			IRCModuleCtx** m = ctx->get_modules(false);

			bool found = false;
			for(; *m; ++m){
				if(strcasecmp((*m)->name, arg) == 0){
					ctx->send_msg(chan, "%s: %s: %s", name, (*m)->name, (*m)->desc);
					found = true;
					break;
				}
			}
			if(!found){
				ctx->send_msg(chan, "%s: I haven't heard of that module...", name);
			}
		} break;
	}
}

static bool meta_check(const char* modname, const char* chan, int callback_id){
	char*** mods = get_enabled_modules(chan);
	return mods && mod_find(*mods, modname);
}

static void meta_join(const char* chan, const char* name){
	if(strcasecmp(name, ctx->get_username()) != 0) return;

	if(!get_enabled_modules(chan)){
		sb_push(channels, strdup(chan));
		sb_push(enabled_mods_for_chan, 0);

		for(IRCModuleCtx** m = ctx->get_modules(true); *m; ++m){
			if((*m)->flags & IRC_MOD_DEFAULT){
				sb_push(sb_last(enabled_mods_for_chan), strdup((*m)->name));
			}
		}
	}
}

static bool meta_save(FILE* file){
	for(int i = 0; i < sb_count(channels); ++i){
		fputs(channels[i], file);
		for(int j = 0; j < sb_count(enabled_mods_for_chan[i]); ++j){
			fprintf(file, "\t%s", enabled_mods_for_chan[i][j]);
		}
		fputc('\n', file);
	}
	return true;

}

