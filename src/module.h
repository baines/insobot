#ifndef INSOBOT_MODULE_H
#define INSOBOT_MODULE_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "config.h"

typedef struct IRCCoreCtx_ IRCCoreCtx;
typedef struct IRCModMsg_ IRCModMsg;

// defined by a module to provide info & callbacks to the core.
typedef struct IRCModuleCtx_ {

	const char*        name;
	const char*        desc;
	const char**       commands; // null-terminated, use DEFINE_CMDS macro
	unsigned int       priority; // bigger number = higher priority
	unsigned int       flags;
	
	bool (*on_init)    (const IRCCoreCtx* ctx);
	void (*on_quit)    (void); //TODO

	// IRC event callbacks
	void (*on_connect) (const char* serv);
	void (*on_msg)     (const char* chan, const char* name, const char* msg);
	void (*on_join)    (const char* chan, const char* name);
	void (*on_part)    (const char* chan, const char* name);
	void (*on_nick)    (const char* prev_nick, const char* new_nick);

	// called when a command listed by the module was found in a message, before on_msg
	void (*on_cmd)     (const char* chan, const char* name, const char* arg, int cmd);

	// called to request the module saves any data it needs, return true to complete the save
	bool (*on_save)    (FILE* file);

	// called when the module's data file is modified externally
	void (*on_modified)(void);

	// called before other callbacks to allow per-channel modules
	bool (*on_meta)    (const char* modname, const char* chan, int callback_id);

	// simple inter-module communication callback
	void (*on_mod_msg) (const char* sender, const IRCModMsg* msg);

	// called atleast once every ~250ms
	void (*on_tick)    (void);

	// called if something was written to stdin
	void (*on_stdin)   (const char* text);

	// called when a message is sent
	void (*on_msg_out) (const char* chan, const char* msg);
} IRCModuleCtx;

// passed to modules to provide functions for them to use.
struct IRCCoreCtx_ {

	const char*    (*get_username) (void);
	const char*    (*get_datafile) (void);
	IRCModuleCtx** (*get_modules)  (bool channel_mods_only); // null terminated
	const char**   (*get_channels) (void); // null terminated
	void           (*join)         (const char* chan);
	void           (*part)         (const char* chan);
	void           (*send_msg)     (const char* chan, const char* fmt, ...) __attribute__ ((format (printf, 2, 3)));
	void           (*send_raw)     (const char* raw);
	void           (*send_mod_msg) (IRCModMsg* msg);
	void           (*save_me)      (void);
	void           (*log)          (const char* fmt, ...) __attribute__ ((format (printf, 1, 2)));
};

// used for on_meta callback, which determines if the callback will be called or not.
enum  {
	IRC_CB_MSG,
	IRC_CB_CMD,
	IRC_CB_JOIN,
	IRC_CB_PART,
};

// used for flags field of IRCModuleCtx
enum {
	IRC_MOD_GLOBAL  = 1, // not a module that can be enabled / disabled per channel
	IRC_MOD_DEFAULT = 2, // enabled by default when joining new channels
};

// used for inter-module communication messages
typedef struct IRCModMsg_ {
	const char* cmd;
	intptr_t    arg;
	void        (*callback)(intptr_t result, intptr_t arg);
	intptr_t    cb_arg;
} IRCModMsg;

#define MOD_MSG(ctx, cmd, arg, cb, cb_arg) (ctx)->send_mod_msg(\
	&(IRCModMsg){ (cmd), (intptr_t)(arg), (cb), (intptr_t)(cb_arg) }\
)

#define DEFINE_CMDS(...) (const char*[]) {\
	__VA_ARGS__,\
	0\
}

#endif
