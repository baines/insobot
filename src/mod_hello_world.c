#include "module.h"

static bool hello_init(const IRCCoreCtx*);
static void hello_cmd (const char*, const char*, const char*, int);

enum { HELLO_SAY_IT };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "helloworld",
	.desc       = "Minimal example module.",
	.on_cmd     = &hello_cmd,
	.on_init    = &hello_init,
	.commands   = DEFINE_CMDS (
		[HELLO_SAY_IT] = CMD("helloworld")
	),
	.cmd_help   = DEFINE_CMDS (
		[HELLO_SAY_IT] = "<alternative noun> | Says hello world, or hello <alternative noun> if given."
	)
};

static const IRCCoreCtx* ctx;

static bool hello_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void hello_cmd(const char* chan, const char* name, const char* arg, int cmd){
	switch(cmd){
		case HELLO_SAY_IT: {
			// if there was nothing after !helloworld, *arg will be '\0' (evaluating to false in the if)
			// otherwise it'll be ' ', evaluating to true. use postfix ++ to move past the space in this case.
			if(*arg++){
				ctx->send_msg(chan, "Hello, %s!", arg);
			} else {
				ctx->send_msg(chan, "Hello world!");
			}
		} break;
	}
}
