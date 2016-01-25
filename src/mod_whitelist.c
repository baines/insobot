#include "module.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static bool whitelist_init    (const IRCCoreCtx*);
static void whitelist_msg     (const char*, const char*, const char*);
static void whitelist_save    (void);
static void whitelist_mod_msg (const char*, const IRCModMsg*);

IRCModuleCtx irc_mod_ctx = {
	.name       = "whitelist",
	.desc       = "Allow known users to access commands",
	.flags      = IRC_MOD_GLOBAL, //FIXME should whitelist be per channel?
	.on_init    = &whitelist_init,
	.on_msg     = &whitelist_msg,
	.on_mod_msg = &whitelist_mod_msg,
	.on_save    = &whitelist_save,
};

static const IRCCoreCtx* ctx;

//TODO: read from file?
static const char* wlist[] = {
	"insofaras",
	"test",
	NULL,
};

static bool wl_check(const char* name){
	for(const char** str = wlist; *str; ++str){
		if(strcmp(name, *str) == 0){
			return true;
		}
	}
	return false;
}

static bool whitelist_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void whitelist_msg(const char* chan, const char* name, const char* msg){

	//TODO: add / del
	enum { WL_CHECK, WL_ADD, WL_DEL };
	int i = ctx->check_cmds(msg, "\\amiwhitelisted", "\\wladd", "\\wldel", NULL);

	switch(i){
		case WL_CHECK: {
			const char* opt = wl_check(name) ? "are" : "are not";
			ctx->send_msg(chan, "%s: You %s whitelisted.", name, opt);
		} break;
	}
}

static void whitelist_save(void){
	//TODO: write to file
}

static void whitelist_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "check_whitelist") == 0){
		const char* name = (const char*)msg->arg;

		for(const char** str = wlist; *str; ++str){
			if(strcmp(name, *str) == 0){
				printf("check_whitelist: %s found.\n", name);
				msg->callback(true, msg->cb_arg);
				break;
			}
		}
	}
}