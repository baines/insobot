#include "module.h"
#include "stb_sb.h"
#include <string.h>
#include <ctype.h>

static void alias_msg  (const char*, const char*, const char*);
static void alias_save (FILE*);
static bool alias_init (const IRCCoreCtx*);

const IRCModuleCtx irc_mod_ctx = {
	.name     = "alias",
	.desc     = "Allows defining simple responses to !commands",
	.flags    = IRC_MOD_DEFAULT,
	.on_save  = &alias_save,
	.on_msg   = &alias_msg,
	.on_init  = &alias_init,
};

static const IRCCoreCtx* ctx;

//TODO: aliases should be per-channel
static char** alias_keys;
static char** alias_vals;

static bool alias_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	FILE* f = fopen(ctx->get_datafile(), "rb");

	char *key, *val;

	while(fscanf(f, "%ms %m[^\n]", &key, &val) == 2){
		fprintf(stderr, "Got alias: [%s] = [%s]\n", key, val);
		sb_push(alias_keys, key);
		sb_push(alias_vals, val);
	}
		
	return true;
}

static void whitelist_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
}

static void do_alias_cmd(const char* chan, const char* name, const char* arg, int cmd){

	enum { CMD_ALIAS_ADD, CMD_ALIAS_DEL };

	bool has_cmd_perms = strcasecmp(chan+1, name) == 0;
	if(!has_cmd_perms){
		ctx->send_mod_msg(&(IRCModMsg){
			.cmd      = "check_whitelist",
			.arg      = (intptr_t)name,
			.callback = &whitelist_cb,
			.cb_arg   = (intptr_t)&has_cmd_perms
		});
	}
	if(!has_cmd_perms) return;

	switch(cmd){
		case CMD_ALIAS_ADD: {
			if(!*arg++ || !isalnum(*arg)) goto usage_add;

			const char* space = strchr(arg, ' ');
			if(!space) goto usage_add;

			char* key = strndupa(arg, space - arg);
			for(char* k = key; *k; ++k) *k = tolower(*k);

			bool found = false;
			for(int i = 0; i < sb_count(alias_keys); ++i){
				if(strcmp(key, alias_keys[i]) == 0){
					free(alias_vals[i]);
					alias_vals[i] = strdup(space+1);
					found = true;
				}
			}

			if(!found){
				sb_push(alias_keys, strdup(key));
				sb_push(alias_vals, strdup(space+1));
			}

			ctx->send_msg(chan, "%s: Alias %s set.", name, key);
		} break;

		case CMD_ALIAS_DEL: {
			if(!*arg++ || !isalnum(*arg)) goto usage_del;

			bool found = false;
			for(int i = 0; i < sb_count(alias_keys); ++i){
				if(strcasecmp(arg, alias_keys[i]) == 0){
					found = true;

					free(alias_keys[i]);
					sb_erase(alias_keys, i);
					
					free(alias_vals[i]);
					sb_erase(alias_vals, i);

					ctx->send_msg(chan, "%s: Removed alias %s.\n", name, arg);
					break;
				}
			}

			if(!found){
				ctx->send_msg(chan, "%s: That alias doesn't exist.", name);
			}
		} break;
	}

	ctx->save_me();

	return;

usage_add:
	ctx->send_msg(chan, "%s: Usage: \\alias <key> <text>", name); return;
usage_del:
	ctx->send_msg(chan, "%s: Usage: \\unalias <key>", name); return;
}

static void alias_msg(const char* chan, const char* name, const char* msg){

	const char* m = msg;
	int i = ctx->check_cmds(&m, "\\alias", "\\unalias", NULL);
	if(i >= 0){

		do_alias_cmd(chan, name, m, i);

	} else if(*msg == '!'){

		int index = -1;
		const char* arg = NULL;
		size_t arg_len = 0;

		for(int i = 0; i < sb_count(alias_keys); ++i){
			size_t alias_len = strlen(alias_keys[i]);

			if(strncasecmp(msg + 1, alias_keys[i], alias_len) == 0){
				index = i;
				arg = msg + alias_len + 1;

				while(*arg == ' '){
					++arg;
				}

				if(*arg){
					arg_len = strlen(arg);
				}

				break;
			}
		}
		if(index < 0) return;

		size_t name_len = strlen(name);
		char* msg_buf = NULL;

		for(const char* str = alias_vals[index]; *str; ++str){
			if(*str == '%' && *(str + 1) == 't'){
				memcpy(sb_add(msg_buf, name_len), name, name_len);
				++str;
			} else if(*str == '%' && *(str + 1) == 'a'){
				if(arg && *arg && arg_len){
					memcpy(sb_add(msg_buf, arg_len), arg, arg_len);
				}
				++str;
			} else {
				sb_push(msg_buf, *str);
			}
		}
		sb_push(msg_buf, 0);

		ctx->send_msg(chan, "%s", msg_buf);
		sb_free(msg_buf);
	}
}

static void alias_save(FILE* file){
	for(int i = 0; i < sb_count(alias_keys); ++i){
		fprintf(file, "%s\t%s\n", alias_keys[i], alias_vals[i]);
	}
}

