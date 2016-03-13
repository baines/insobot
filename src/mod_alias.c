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
    char** keys;
    char* msg;
} Alias;

//TODO: aliases should be per-channel
static char** alias_keys;
static int* alias_val_offsets;

static Alias* alias_pool;

static Alias* alias_get_i(int ikey){
    int ival = alias_val_offsets[ikey];
    return alias_pool + ival;
}

// Adds a totally new alias, allocating one from the pool and setting up its initial values
static void alias_new(char* key, char* msg, int perm) {
    fprintf(stderr, "New alias: [%s] = [%s] (Access level %s)\n", key, msg, alias_permission_strs[perm]);
    Alias value = {
        .permission = perm,
        .keys       = NULL,
        .msg        = msg,
    };
    sb_push(alias_keys, key);
    sb_push(value.keys, key);
    sb_push(alias_pool, value);
    sb_push(alias_val_offsets, sb_count(alias_pool) - 1);
}

// Removes an alias from the value list, freeing up its spot in the pool
static void alias_delete(int ikey) {
    int ival = alias_val_offsets[ikey];
    Alias* value = alias_pool + ival;
    if (sb_count(value->keys) == 0) {
        sb_free(value->keys);
        free(value->msg);
    }
    // Else someone still has a reference to us
    else return;

    sb_erase(alias_pool, ival);
}

// Notifies an atlas that the key at alias_keys[ikey] is no longer pointing to it
// Possibly deleting the atlas if that was the last reference to it
static void alias_remove_reference(int ikey) {
    Alias* value = alias_get_i(ikey);
    //Iterate over alias's stored keys to check that this key points to it
    for (int ivalkey = 0; ivalkey < sb_count(value->keys); ++ivalkey) {
        if (strcmp(alias_keys[ikey], value->keys[ivalkey]) == 0) {
            // Remove this key from this alias's key list
            sb_erase(value->keys, ivalkey);
            break;
        }
    }
    if (sb_count(value->keys) == 0) {
        alias_delete(ikey);
    }
}

// Adds another key that maps to the same value as oldkey
static void alias_add_reference(int i_oldkey, char* newkey) {
    Alias* value = alias_get_i(i_oldkey);
    int ival = value - alias_pool;
    sb_push(value->keys, newkey);
    sb_push(alias_keys, newkey);
    sb_push(alias_val_offsets, ival);
}

// Updates the msg and/or permission flag of an alias.
// If update_other_references is false, and other keys point to this alias,
//   we'll make a new alias struct and point the key at that instead.
static void alias_update(int ikey, char* key, char* msg, int perm, bool update_other_references)
{
    if (strcmp(alias_keys[ikey], key) != 0) return;
    Alias* value = alias_get_i(ikey);

    if (update_other_references || sb_count(value->keys) == 0) {
        if (msg != NULL) { value->msg = msg; }
        value->permission = perm;
    }
    else {
        // Can't use the existing alias instance, since someone else points to us
        //   and we don't want them to update unless specifically asked to
        alias_remove_reference(ikey);

        Alias value = {
            .permission = perm,
            .keys       = NULL,
            .msg        = msg,
        };
        sb_push(alias_pool, value);
        alias_val_offsets[ikey] = sb_count(alias_pool) - 1;
    }
}

static void alias_add(char* key, char* msg, int perm)
{
    //TODO(chronister): I think this can be simplified down into alias_update
    //   by just creating the alias if it isn't found in that function
    // But I need to check use cases first
    bool found = false;
    for(int i = 0; i < sb_count(alias_keys); ++i){
        if(strcmp(key, alias_keys[i]) == 0){
            alias_update(i, key, msg, perm, false);
            found = true;
        }
    }

    if(!found){
        alias_new(strdup(key), msg, perm);
    }
}

static bool alias_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	FILE* f = fopen(ctx->get_datafile(), "rb");

    bool got_alias = false;
	do {
        size_t fptr = ftell(f);
        char *keystr, *permstr, *msg;
        int perm = -1;
        got_alias = false; // Guilty until proven innocent

        bool got_keys = fscanf(f, " [%m[^]]]", &keystr) == 1;
        if (got_keys) {
            int c;
            while(1) {
                c = fgetc(f);
                if (c == ' ')       continue;
                else if (c == ']')  continue;
                else if (c == ':')  break;
                else break;
            }
            
            if (c == ':') {
                got_alias = fscanf(f, "%ms %m[^\n]", &permstr, &msg) == 2;
                
                if      (strcmp(permstr, alias_permission_strs[AP_NORMAL]) == 0)        perm = AP_NORMAL;
                else if (strcmp(permstr, alias_permission_strs[AP_WHITELISTED]) == 0)   perm = AP_WHITELISTED;
                else if (strcmp(permstr, alias_permission_strs[AP_ADMINONLY]) == 0)     perm = AP_ADMINONLY;

                free(permstr);

                if (perm == -1) { 
                    got_alias = false; 
                    free(keystr);
                    free(msg);
                }
            }
            else {
                free(keystr);
            }
        }

        char** keys = NULL;

        if (got_alias) {
            char* keystart = keystr;
            for (char* keycur = keystr + 1; *(keycur-1); ++keycur) {
                if ((isspace(*keycur) || *keycur == 0) && !isspace(*(keycur - 1)))
                    sb_push(keys, strndup(keystart, keycur - keystart));
                else if (!isspace(*keycur) && isspace(*(keycur - 1)))
                    keystart = keycur;
            }
        }

        if (!got_alias) {
            // Might be an old-style alias declaration
            fseek(f, fptr, SEEK_SET);
            got_alias = fscanf(f, "%ms %m[^\n]", &keystr, &msg) == 2;
            sb_push(keys, keystr);
            perm = AP_NORMAL;
        }

        if (got_alias && keys != NULL) {
            alias_add(keys[0], msg, perm);
            int ikey = sb_count(keys) - 1;
            for (int i = 1; i < sb_count(keys); ++i) {
                alias_add_reference(ikey, keys[i]);
            }
        }
	} while(got_alias);
		
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

            if (strncmp(space+1, "->", 2) == 0) {
                // Aliasing new key to existing command
                char* otherkey;
                sscanf(space + 3, " %ms", &otherkey);

                bool found = false;
                for(int i = 0; i < sb_count(alias_keys); ++i){
                    if(strcmp(otherkey, alias_keys[i]) == 0){
                        found = true;
                        alias_add_reference(i, strdup(key));
                        ctx->send_msg(chan, "%s: Alias %s set.", name, key);
                    }
                }
                if (!found) {
                    ctx->send_msg(chan, "%s: Can't alias %s as %s is not defined.", name, key, otherkey);
                    free(otherkey);
                }
            }
            else {
                alias_add(strdup(key), strdup(space+1), AP_NORMAL);
                ctx->send_msg(chan, "%s: Alias %s set.", name, key);
            }

		} break;

		case ALIAS_DEL: {
			if(!*arg++ || !isalnum(*arg)) goto usage_del;

			bool found = false;
			for(int i = 0; i < sb_count(alias_keys); ++i){
				if(strcasecmp(arg, alias_keys[i]) == 0){
					found = true;

                    alias_remove_reference(i);
                    sb_erase(alias_keys, i);
                    sb_erase(alias_val_offsets, i);

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
                    alias = alias_get_i(i);
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

    Alias* value = alias_get_i(index);
	bool has_cmd_perms = (value->permission == AP_NORMAL) || strcasecmp(chan+1, name) == 0;
	if(!has_cmd_perms){
        if (value->permission == AP_WHITELISTED)
            MOD_MSG(ctx, "check_whitelist", name, &whitelist_cb, &has_cmd_perms);
        else if (value->permission == AP_ADMINONLY)
            MOD_MSG(ctx, "check_admin", name, &whitelist_cb, &has_cmd_perms);
        else {
            // Some kind of weird unknown permission type. Assume normal access.
            has_cmd_perms = true;
        }
	}
	if(!has_cmd_perms) return;

	for(const char* str = value->msg; *str; ++str){
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
	for(int i = 0; i < sb_count(alias_pool); ++i){
        Alias* alias = alias_pool + i;
        fputc('[', file);
        for (int ikey = 0; ikey < sb_count(alias->keys); ++ikey) {
            fputs(alias->keys[ikey], file); 
            fputc(' ', file);
        }
        fputc(']', file);
        bool perm_valid = (alias->permission >= 0 && alias->permission <= AP_COUNT);
        if (perm_valid) {
            fputc(':', file);
            fputs(alias_permission_strs[alias->permission], file);
        }
        fprintf(file, " %s\n", alias->msg);
	}
	return true;
}

