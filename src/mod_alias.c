#include "module.h"
#include "stb_sb.h"
#include <string.h>
#include <ctype.h>
#include "utils.h"

static void alias_msg  (const char*, const char*, const char*);
static void alias_cmd  (const char*, const char*, const char*, int);
static bool alias_save (FILE*);
static bool alias_init (const IRCCoreCtx*);

enum { ALIAS_ADD, ALIAS_DEL, ALIAS_LIST, ALIAS_SET_PERM };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "alias",
	.desc     = "Allows defining simple responses to !commands",
	.flags    = IRC_MOD_DEFAULT,
	.on_save  = &alias_save,
	.on_msg   = &alias_msg,
	.on_cmd   = &alias_cmd,
	.on_init  = &alias_init,
	.commands = DEFINE_CMDS (
		[ALIAS_ADD]      = CONTROL_CHAR"alias ",
		[ALIAS_DEL]      = CONTROL_CHAR"unalias "    CONTROL_CHAR"delalias "   CONTROL_CHAR"rmalias ",
		[ALIAS_LIST]     = CONTROL_CHAR"lsalias "    CONTROL_CHAR"lsa "        CONTROL_CHAR"listalias "    CONTROL_CHAR"listaliases ",
		[ALIAS_SET_PERM] = CONTROL_CHAR"chaliasmod " CONTROL_CHAR"chamod "       CONTROL_CHAR"aliasaccess "  CONTROL_CHAR"setaliasaccess "
	)
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

static void alias_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool has_cmd_perms = strcasecmp(chan+1, name) == 0;
	if(!has_cmd_perms){
		MOD_MSG(ctx, "check_whitelist", name, &whitelist_cb, &has_cmd_perms);
	}
	if(!has_cmd_perms) return;

	switch(cmd){
		case ALIAS_ADD: {
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

		case ALIAS_DEL: {
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

		case ALIAS_LIST: {
			char alias_buf[512];
			char* ptr = alias_buf;
			size_t sz = sizeof(alias_buf);

			const size_t total = sb_count(alias_keys);

			if(total == 0){
				inso_strcat(alias_buf, sizeof(alias_buf), "<none>.");
			} else {
				for(int i = 0; i < total; ++i){
					snprintf_chain(&ptr, &sz, "!%s%s", alias_keys[i], i == (total-1) ? "." : ", ");
				}
			}

			ctx->send_msg(chan, "%s: Current aliases: %s", name, alias_buf);
		} break;

        case ALIAS_SET_PERM: {
			if(!*arg++ || !isalnum(*arg)) goto usage_setperm;
            
			const char* space = strchr(arg, ' ');
			if(!space) goto usage_setperm;

			char* key = strndupa(arg, space - arg);
			for(char* k = key; *k; ++k) *k = tolower(*k);

            Alias* alias;
			bool found = false;
			for(int i = 0; i < sb_count(alias_keys); ++i){
				if(strcmp(key, alias_keys[i]) == 0){
					found = true;
                    alias = alias_vals + i;
				}
			}

			if(!found){
                ctx->send_msg(chan, "%s: No alias called '%s'.", name, key);
                return;
			}

            int perm = -1;
            const char* permstr = space+1;             
            if (strcasecmp(permstr, alias_permission_strs[AP_NORMAL]) == 0)
                perm = AP_NORMAL;
            else if (strcasecmp(permstr, alias_permission_strs[AP_WHITELISTED]) == 0)
                perm = AP_WHITELISTED;
            else if (strcasecmp(permstr, alias_permission_strs[AP_ADMINONLY]) == 0)
                perm = AP_ADMINONLY;
            
            if (perm == -1) {
                ctx->send_msg(chan, "%s: Not sure what permission level '%s' is.", name, permstr);
                return;
            }
                
            alias->permission = perm;
			ctx->send_msg(chan, "%s: Set permissions on %s to %s.", name, key, permstr);
        } break;
	}

	ctx->save_me();

	return;

usage_add:
	ctx->send_msg(chan, "%s: Usage: "CONTROL_CHAR"alias <key> <text>", name); return;
usage_del:
	ctx->send_msg(chan, "%s: Usage: "CONTROL_CHAR"unalias <key>", name); return;
usage_setperm:
	ctx->send_msg(chan, "%s: Usage: "CONTROL_CHAR"chaliasmod <key> [NORMAL|WLIST|ADMIN]", name); return;
}

static void alias_msg(const char* chan, const char* name, const char* msg){

	if(*msg != '!') return;

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
		} else if(*str == '%' && *(str + 1) == 'n'){
			if(arg && *arg && arg_len){
				memcpy(sb_add(msg_buf, arg_len), arg, arg_len);
			} else {
				memcpy(sb_add(msg_buf, name_len), name, name_len);
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

static bool alias_save(FILE* file){
	for(int i = 0; i < sb_count(alias_keys); ++i){
		fprintf(file, "%s\t%s\n", alias_keys[i], alias_vals[i]);
	}
	return true;
}

