#include "module.h"
#include "stb_sb.h"
#include <string.h>
#include <ctype.h>
#include "utils.h"

static void alias_msg      (const char*, const char*, const char*);
static void alias_cmd      (const char*, const char*, const char*, int);
static bool alias_save     (FILE*);
static bool alias_init     (const IRCCoreCtx*);
static void alias_modified (void);

enum { ALIAS_ADD, ALIAS_ADD_GLOBAL, ALIAS_DEL, ALIAS_DEL_GLOBAL, ALIAS_LIST, ALIAS_SET_PERM };

const IRCModuleCtx irc_mod_ctx = {
	.name        = "alias",
	.desc        = "Allows defining simple responses to !commands",
	.flags       = IRC_MOD_DEFAULT,
	.on_save     = &alias_save,
	.on_modified = &alias_modified,
	.on_msg      = &alias_msg,
	.on_cmd      = &alias_cmd,
	.on_init     = &alias_init,
	.commands    = DEFINE_CMDS (
		[ALIAS_ADD]        = CONTROL_CHAR "alias",
		[ALIAS_ADD_GLOBAL] = CONTROL_CHAR "galias",
		[ALIAS_DEL]        = CONTROL_CHAR "unalias "    CONTROL_CHAR "delalias "  CONTROL_CHAR "rmalias ",
		[ALIAS_DEL_GLOBAL] = CONTROL_CHAR "gunalias "   CONTROL_CHAR "gdelalias " CONTROL_CHAR "grmalias ",
		[ALIAS_LIST]       = CONTROL_CHAR "lsalias "    CONTROL_CHAR "lsa "       CONTROL_CHAR "listalias "   CONTROL_CHAR "listaliases",
		[ALIAS_SET_PERM]   = CONTROL_CHAR "chaliasmod " CONTROL_CHAR "chamod "    CONTROL_CHAR "aliasaccess " CONTROL_CHAR "setaliasaccess"
	)
};

static const IRCCoreCtx* ctx;

enum {
	AP_NORMAL = 0,
	AP_WHITELISTED,
	AP_ADMINONLY,

	AP_COUNT,
};

char* alias_permission_strs[] = {
	"NORMAL",
	"WLIST",
	"ADMIN",
};

typedef struct {
	int permission;
	bool me_action;
	char* msg;
} Alias;

static char*** alias_keys;
static Alias*  alias_vals;

static void alias_load(){
	int save_format_ver = 0;
	char** keys = NULL;
	Alias val = { .permission = AP_NORMAL };

	FILE* f = fopen(ctx->get_datafile(), "rb");
	
	if(fscanf(f, "VERSION %d\n", &save_format_ver) == 1){
		if(save_format_ver != 2){
			fprintf(stderr, "Unknown save format version %d! Can't load any aliases.\n", save_format_ver);
			return;
		}

		while(!feof(f)){
			bool parse_keys = true;
			char* token;

			while(parse_keys){
				if(fscanf(f, "%ms", &token) != 1){
					break;
				}

				if(isupper(*token)){
					int perm_idx = -1;
					for(int i = 0; i < AP_COUNT; ++i){
						if(strcmp(token, alias_permission_strs[i]) == 0){
							perm_idx = i;
							break;
						}
					}

					if(perm_idx >= 0){
						val.permission = perm_idx;
					} else {
						fprintf(stderr, "Unknown permission in file %s!\n", token);
					}

					free(token);
					parse_keys = false;
				} else {
					sb_push(keys, token);
				}
			}

			if(fscanf(f, " %m[^\n]", &token) == 1){
				val.msg = token;
				val.me_action = (strstr(token, "/me") == token);
				sb_push(alias_keys, keys);
				sb_push(alias_vals, val);
				fprintf(stderr, "Loaded alias [%s] = [%s]\n", *keys, val.msg);
			}

			keys = NULL;
		}
	} else {
		// This is probably the original format which didn't have the VERSION header, convert it
		char* key;
		while(fscanf(f, "%ms %m[^\n]", &key, &val.msg) == 2){
			fprintf(stderr, "Loaded old style alias [%s] = [%s]\n", key, val.msg);
			sb_push(keys, key);
			sb_push(alias_keys, keys);
			sb_push(alias_vals, val);
			keys = NULL;
		}
	}

	fclose(f);
}

static bool alias_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	alias_load();
	return true;
}

static void alias_modified(void){
	for(int i = 0; i < sb_count(alias_keys); ++i){
		for(int j = 0; j < sb_count(alias_keys[i]); ++j){
			free(alias_keys[i][j]);
		}
		sb_free(alias_keys[i]);
		free(alias_vals[i].msg);
	}
	sb_free(alias_vals);

	alias_load();
}

static bool alias_valid_1st_char(char c){
	// this needs to exclude atleast irc channel prefixes: # & + ~ . !
	return isalnum(c);
}

enum { ALIAS_NOT_FOUND = 0, ALIAS_FOUND_CHAN = 1, ALIAS_FOUND_GLOBAL = 2 };

static int alias_find(const char* chan, const char* key, int* idx, int* sub_idx){

	if(chan){
		char full_key[strlen(chan) + strlen(key) + 2];
		snprintf(full_key, sizeof(full_key), "%s,%s", chan, key);

		for(int i = 0; i < sb_count(alias_keys); ++i){
			for(int j = 0; j < sb_count(alias_keys[i]); ++j){
				if(strcasecmp(full_key, alias_keys[i][j]) == 0){
					if(idx) *idx = i;
					if(sub_idx) *sub_idx = j;
					return ALIAS_FOUND_CHAN;
				}
			}
		}
	}
	
	for(int i = 0; i < sb_count(alias_keys); ++i){
		for(int j = 0; j < sb_count(alias_keys[i]); ++j){
			if(strcasecmp(key, alias_keys[i][j]) == 0){
				if(idx) *idx = i;
				if(sub_idx) *sub_idx = j;
				return ALIAS_FOUND_GLOBAL;
			}
		}
	}

	return ALIAS_NOT_FOUND;
}

static void alias_add(const char* chan, const char* key, const char* msg, int perm){
	Alias* alias;
	int idx;

	int required = chan ? ALIAS_FOUND_CHAN : ALIAS_FOUND_GLOBAL;

	if(alias_find(chan, key, &idx, NULL) == required){
		alias = alias_vals + idx;
		free(alias->msg);
	} else {

		char* full_key;
		if(chan){
			asprintf(&full_key, "%s,%s", chan, key);
		} else {
			full_key = strdup(key);
		}

		char** keys = NULL;
		Alias a = {};

		sb_push(keys, full_key);
		sb_push(alias_keys, keys);
		sb_push(alias_vals, a);

		alias = &sb_last(alias_vals);
	}

	alias->msg        = strdup(msg);
	alias->permission = perm;
	alias->me_action  = (strstr(msg, "/me") == msg);
}

static void alias_del(int idx, int sub_idx){
	sb_erase(alias_keys[idx], sub_idx);
	if(sb_count(alias_keys[idx]) == 0){
		sb_erase(alias_keys, idx);
		sb_erase(alias_vals, idx);
	}
}

static void whitelist_cb(intptr_t result, intptr_t arg){
	if(result) *(bool*)arg = true;
}

static void alias_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool is_admin = strcasecmp(chan+1, name) == 0;
	bool is_wlist = is_admin;

	if(!is_wlist) MOD_MSG(ctx, "check_whitelist", name, &whitelist_cb, &is_wlist);
	if(!is_admin) MOD_MSG(ctx, "check_admin", name, &whitelist_cb, &is_admin);

	if(!is_wlist) return;

	switch(cmd){
		case ALIAS_ADD: {
			if(!*arg++ || !alias_valid_1st_char(*arg)) goto usage_add;

			const char* space = strchr(arg, ' ');
			if(!space) goto usage_add;

			char* key = strndupa(arg, space - arg);
			for(char* k = key; *k; ++k) *k = tolower(*k);

			if(strncmp(space+1, "->", 2) == 0){
				// Aliasing new key to existing command

				char* otherkey;
				if(sscanf(space + 3, " %ms", &otherkey) != 1){
					ctx->send_msg(chan, "%s: Alias it to what, exactly?", name);
					break;
				}

				int idx, sub_idx;
				if(alias_find(chan, key, &idx, &sub_idx) == ALIAS_FOUND_CHAN){
					if(alias_vals[idx].permission == AP_ADMINONLY && !is_admin){
						ctx->send_msg("%s: You don't have permission to change %s.\n", name, key);
						break;
					} else {
						alias_del(idx, sub_idx);
					}
				}

				int otheridx;
				if(alias_find(chan, otherkey, &otheridx, NULL)){
					char* chan_key;
					asprintf(&chan_key, "%s,%s", chan, key);
					sb_push(alias_keys[otheridx], chan_key);
					ctx->send_msg(chan, "%s: Alias %s set.", name, key);
				} else {
					ctx->send_msg(chan, "%s: Can't alias %s as %s is not defined.", name, key, otherkey);
					free(otherkey);
				}
			} else {
				alias_add(chan, key, space+1, AP_NORMAL);
				ctx->send_msg(chan, "%s: Alias %s set.", name, key);
			}

		} break;

		case ALIAS_ADD_GLOBAL: {
			//TODO: implement the -> thing for global aliases? should only be able to alias global to global i think
			if(!*arg++ || !alias_valid_1st_char(*arg)) goto usage_add;

			const char* space = strchr(arg, ' ');
			if(!space) goto usage_add;

			char* key = strndupa(arg, space - arg);
			for(char* k = key; *k; ++k) *k = tolower(*k);

			alias_add(NULL, key, space+1, AP_NORMAL);
			ctx->send_msg(chan, "%s: Global alias %s set.", name, key);
		} break;

		case ALIAS_DEL: {
			if(!*arg++ || !alias_valid_1st_char(*arg)) goto usage_del;

			int idx, sub_idx;
			int found = alias_find(chan, arg, &idx, &sub_idx);

			if(found == ALIAS_FOUND_CHAN){
				if(alias_vals[idx].permission == AP_ADMINONLY && !is_admin){
					ctx->send_msg("%s: You don't have permission to delete %s.\n", name, arg);
					break;
				} else {
					alias_del(idx, sub_idx);
					ctx->send_msg(chan, "%s: Removed alias %s.\n", name, arg);
				}
			} else if(found == ALIAS_FOUND_GLOBAL){
				//TODO: create a blank alias for this channel to disable the global one only here.
				ctx->send_msg(chan, "%s: That's a global alias, poke insofaras to implement hiding them per channel, or use " CONTROL_CHAR "gdelalias to remove it everywhere." , name);
			} else {
				ctx->send_msg(chan, "%s: That alias doesn't exist.", name);
			}
		} break;

		case ALIAS_DEL_GLOBAL: {
			if(!*arg++ || !alias_valid_1st_char(*arg)) goto usage_del;

			int idx, sub_idx;
			if(alias_find(NULL, arg, &idx, &sub_idx)){
				if(alias_vals[idx].permission == AP_ADMINONLY && !is_admin){
					ctx->send_msg("%s: You don't have permission to change %s.\n", name, arg);
					break;
				} else {
					alias_del(idx, sub_idx);
					ctx->send_msg(chan, "%s: Removed global alias %s.\n", name, arg);
				}
			} else {
				ctx->send_msg(chan, "%s: That global alias doesn't exist.", name);
			}
		} break;

		case ALIAS_LIST: {
			//XXX: this is probably over-complicated

			const char** aliases_to_print = NULL;
			const size_t total = sb_count(alias_keys);

			for(int i = 0; i < total; ++i){
				const size_t subtotal = sb_count(alias_keys[i]);

				for(int j = 0; j < subtotal; ++j){
					const char* key = alias_keys[i][j];

					// channel specific alias, only print if the channel matches
					if(!alias_valid_1st_char(*key)){
						char* endptr = strchr(key, ',');
						if(endptr && strncmp(chan, key, endptr - key) == 0){
							key = endptr + 1;
						} else {
							key = NULL;
						}
					}

					if(key){
						bool already_printed = false;
						for(int k = 0; k < sb_count(aliases_to_print); ++k){
							if(strcmp(aliases_to_print[k], key) == 0){
								already_printed = true;
								break;
							}
						}

						if(!already_printed){
							sb_push(aliases_to_print, key);
						}
					}
				}
			}

			char alias_buf[512];
			char* ptr = alias_buf;
			size_t sz = sizeof(alias_buf);
		
			if(sb_count(aliases_to_print) == 0){
				strcpy(alias_buf, "<none>.");
			} else {
				for(int i = 0; i < sb_count(aliases_to_print); ++i){
					const bool last = i == sb_count(aliases_to_print) - 1;
					snprintf_chain(&ptr, &sz, "!%s%s", aliases_to_print[i], last ? "." : ", ");
				}
			}

			sb_free(aliases_to_print);

			ctx->send_msg(chan, "%s: Current aliases: %s", name, alias_buf);
		} break;

		//FIXME: potential issues:
		// * changing permissions of global alias through local alias ptr
		
		case ALIAS_SET_PERM: {
			if(!*arg++ || !alias_valid_1st_char(*arg)) goto usage_setperm;

			const char* space = strchr(arg, ' ');
			if(!space) goto usage_setperm;

			char* key = strndupa(arg, space - arg);
			for(char* k = key; *k; ++k) *k = tolower(*k);

			int idx;
			if(!alias_find(chan, key, &idx, NULL)){
				ctx->send_msg(chan, "%s: No alias called '%s'.", name, key);
				break;
			}

			bool perm_set = false;
			const char* permstr = space+1;
			for(int i = 0; i < AP_COUNT; ++i){
				if(strcasecmp(permstr, alias_permission_strs[i]) == 0){
					if(i == AP_ADMINONLY && !is_admin){
						ctx->send_msg(chan, "%s: You don't have permission to set that permission... Yeah.", name);
					} else {
						alias_vals[idx].permission = i;
						ctx->send_msg(chan, "%s: Set permissions on %s to %s.", name, key, permstr);
						perm_set = true;
					}
					break;
				}
			}

			if(!perm_set){
				ctx->send_msg(chan, "%s: Not sure what permission level '%s' is.", name, permstr);
			}
		} break;
	}

	ctx->save_me();

	return;

usage_add:
	ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "(g)alias <key> <text>", name); return;
usage_del:
	ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "(g)unalias <key>", name); return;
usage_setperm:
	ctx->send_msg(chan, "%s: Usage: " CONTROL_CHAR "chaliasmod <key> [NORMAL|WLIST|ADMIN]", name); return;
}

static void alias_msg(const char* chan, const char* name, const char* msg){

	if(*msg != '!' || !alias_valid_1st_char(msg[1])) return;

	const char* key = strndupa(msg+1, strchrnul(msg, ' ') - (msg+1));
	int idx, sub_idx;
	if(!alias_find(chan, key, &idx, &sub_idx)){
		return;
	}

	size_t alias_len = strlen(alias_keys[idx][sub_idx]);
	const char* arg = msg + alias_len + 1;

	while(*arg == ' ') ++arg;

	size_t arg_len = strlen(arg);
	size_t name_len = strlen(name);
	char* msg_buf = NULL;

	Alias* value = alias_vals + idx;
	bool has_cmd_perms = (value->permission == AP_NORMAL) || strcasecmp(chan+1, name) == 0;
	if(!has_cmd_perms){
		if (value->permission == AP_WHITELISTED){
			MOD_MSG(ctx, "check_whitelist", name, &whitelist_cb, &has_cmd_perms);
		} else if (value->permission == AP_ADMINONLY){
			MOD_MSG(ctx, "check_admin", name, &whitelist_cb, &has_cmd_perms);
		} else {
			// Some kind of weird unknown permission type. Assume normal access.
			has_cmd_perms = true;
		}
	}
	if(!has_cmd_perms) return;

	for(const char* str = value->msg + (value->me_action ? 3 : 0); *str; ++str){
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

	if(value->me_action){
		ctx->send_msg(chan, "\001ACTION %s\001", msg_buf);
	} else {
		ctx->send_msg(chan, "%s", msg_buf);
	}

	sb_free(msg_buf);
}

static bool alias_save(FILE* file){
	fputs("VERSION 2\n", file);
	for(int i = 0; i < sb_count(alias_keys); ++i){
		for(int j = 0; j < sb_count(alias_keys[i]); ++j){
			fprintf(file, "%s ", alias_keys[i][j]);
		}

		int perms = alias_vals[i].permission;
		if(perms < 0 || perms >= AP_COUNT) perms = AP_NORMAL;
		fputs(alias_permission_strs[perms], file);

		fprintf(file, " %s\n", alias_vals[i].msg);
	}
	return true;
}

