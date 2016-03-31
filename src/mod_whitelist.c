#include "module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "stb_sb.h" 

static bool whitelist_init    (const IRCCoreCtx*);
static void whitelist_cmd     (const char*, const char*, const char*, int);
static bool whitelist_save    (FILE*);
static void whitelist_mod_msg (const char*, const IRCModMsg*);

enum { WL_CHECK_SELF, WL_CHECK_OTHER, WL_ADD, WL_DEL };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "whitelist",
	.desc       = "Allow known users to access commands",
	.flags      = IRC_MOD_GLOBAL,
	.on_init    = &whitelist_init,
	.on_cmd     = &whitelist_cmd,
	.on_mod_msg = &whitelist_mod_msg,
	.on_save    = &whitelist_save,
	.commands   = DEFINE_CMDS (
		[WL_CHECK_SELF]  = CONTROL_CHAR"wl    "CONTROL_CHAR"wlcheckme "CONTROL_CHAR"amiwhitelisted",
		[WL_CHECK_OTHER] = CONTROL_CHAR"iswl  "CONTROL_CHAR"wlcheck",
		[WL_ADD]         = CONTROL_CHAR"wladd "CONTROL_CHAR"wl+",
		[WL_DEL]         = CONTROL_CHAR"wldel "CONTROL_CHAR"wl-"
	)
};

static const IRCCoreCtx* ctx;

enum {
	ROLE_PLEBIAN     = 0,
	ROLE_WHITELISTED = (1 << 0),
	ROLE_ADMIN       = (1 << 1) | ROLE_WHITELISTED,
};

typedef struct WLEntry_ {
	char* name;
	int role;
} WLEntry;

static WLEntry* wlist;

static void whitelist_load(void){
	FILE* f = fopen(ctx->get_datafile(), "r");

	char role[32], name[64];

	while(fscanf(f, "%31s %63s", role, name) == 2){
		WLEntry wl = { .role = ROLE_PLEBIAN };

		if(strcasecmp(role, "ADMIN") == 0){
			wl.role = ROLE_ADMIN;
		} else if(strcasecmp(role, "WLIST") == 0){
			wl.role = ROLE_WHITELISTED;
		}
		
		if(wl.role != ROLE_PLEBIAN){
			wl.name = strdup(name);
			sb_push(wlist, wl);
		}
	}

	fclose(f);
}

static inline bool role_check(const char* name, int role){
	for(WLEntry* wle = wlist; wle < sb_end(wlist); ++wle){
		if(strcasecmp(name, wle->name) == 0 && (wle->role & role) == role){
			return true;
		}
	}
	return false;
}

static inline bool wlist_check(const char* name){
	return role_check(name, ROLE_WHITELISTED);
}

static inline bool admin_check(const char* name){
	return role_check(name, ROLE_ADMIN);
}

static bool whitelist_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	whitelist_load();
	return true;
}

static void whitelist_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool is_admin = strcasecmp(chan + 1, name) == 0 || admin_check(name);

	switch(cmd){
		case WL_CHECK_SELF: {
			const char* opt = wlist_check(name) ? "are" : "are not";
			ctx->send_msg(chan, "%s: You %s whitelisted.", name, opt);
		} break;

		case WL_CHECK_OTHER: {
			if(!is_admin || !*arg++) break;

			const char* opt = wlist_check(arg) ? "is" : "is not";
			ctx->send_msg(chan, "%s: %s %s whitelisted.", name, arg, opt);
		} break;

		case WL_ADD: {
			if(!is_admin) break;

			if(!*arg++){
				ctx->send_msg(chan, "%s: Whitelist who exactly?", name);
				break;
			}

			bool found = false;
			for(WLEntry* wle = wlist; wle < sb_end(wlist); ++wle){
				if((wle->role & ROLE_WHITELISTED) && strcasecmp(wle->name, arg) == 0){
					ctx->send_msg(chan, "%s: They're already whitelisted.", name);
					found = true;
					break;
				}
			}

			if(!found){
				ctx->send_msg(chan, "%s: Whitelisted %s.", name, arg);
				WLEntry wle = { .name = strdup(arg), .role = ROLE_WHITELISTED };
				sb_push(wlist, wle);
				ctx->save_me();
			}
		} break;

		case WL_DEL: {
			if(!is_admin) break;

			if(!*arg++){
				ctx->send_msg(chan, "%s: Unwhitelist who exactly?", name);
				break;
			}

			bool found = false;
			for(WLEntry* wle = wlist; wle < sb_end(wlist); ++wle){
				if(wle->role == ROLE_WHITELISTED && strcasecmp(wle->name, arg) == 0){
					ctx->send_msg(chan, "%s: Unwhitelisted %s.", name, arg);
					free(wle->name);
					sb_erase(wlist, wle - wlist);
					ctx->save_me();
					found = true;
					break;
				}
			}

			if(!found){
				ctx->send_msg(chan, "%s: They're already not whitelisted.", name);
			}
		} break;
	}
}

static bool whitelist_save(FILE* file){
	for(WLEntry* wle = wlist; wle < sb_end(wlist); ++wle){
		if(wle->role == ROLE_ADMIN){
			fprintf(file, "ADMIN %s\n", wle->name);
		} else if(wle->role == ROLE_WHITELISTED){
			fprintf(file, "WLIST %s\n", wle->name);
		}
	}
	return true;
}

static void whitelist_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "check_whitelist") == 0){
		msg->callback(wlist_check((const char*)msg->arg), msg->cb_arg);
	}

	if(strcmp(msg->cmd, "check_admin") == 0){
		msg->callback(admin_check((const char*)msg->arg), msg->cb_arg);
	}
}
