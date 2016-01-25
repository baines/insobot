#ifndef INSOBOT_MODULE_H
#define INSOBOT_MODULE_H
#include <stdint.h>
#include <stdbool.h>

#define WARN_FMT(x)   __attribute__ ((format (printf, x, x+1))) 
#define WARN_SENTINEL __attribute__ ((sentinel))

// used for on_meta callback, which determines if the callback will be called or not.
enum  {
	IRC_CB_MOD_LOADED,
	IRC_CB_MSG,
	IRC_CB_CONNECT,
	IRC_CB_JOIN,
	IRC_CB_PART,
};

// used for flags field of IRCModuleCtx
enum {
	IRC_MOD_GLOBAL = 1, // not a module that can be enabled / disabled per channel
};

// used for inter-module communication messages
typedef struct IRCModMsg_ {
	const char* cmd;
	intptr_t    arg;
	void        (*callback)(intptr_t result, intptr_t arg);
	intptr_t    cb_arg;
} IRCModMsg;

struct IRCCoreCtx_;
typedef struct IRCCoreCtx_ IRCCoreCtx;

// defined by a module to provide info & callbacks to the core.
typedef struct IRCModuleCtx_ {

	const char*        name;
	const char*        desc;

	// bigger number = called first
	unsigned int       priority;

	unsigned int       flags;

	// irc callbacks
	void (*on_connect) (void);
	void (*on_msg)     (const char* chan, const char* name, const char* msg);
	void (*on_join)    (const char* chan, const char* name);
	void (*on_part)    (const char* chan, const char* name);

	bool (*on_init)    (const IRCCoreCtx* ctx);

	// called to request the module saves any data it needs
	void (*on_save)    (void);

	// called before other callbacks to allow per-channel modules
	bool (*on_meta)    (const char* modname, const char* chan, int callback_id);

	// simple inter-module communication callback
	void (*on_mod_msg) (const char* sender, const IRCModMsg* msg);

} IRCModuleCtx;

// passed to modules to provide functions for them to use.
struct IRCCoreCtx_ {

	const char*    (*get_username) (void);
	const char*    (*get_datafile) (void);
	IRCModuleCtx** (*get_modules)  (void);
	void           (*join)         (const char* chan);
	void           (*part)         (const char* chan);
	void           (*send_msg)     (const char* chan, const char* fmt, ...) WARN_FMT(2);
	void           (*send_mod_msg) (IRCModMsg* msg);
	int            (*check_cmds)   (const char* msg, ...) WARN_SENTINEL;
	//XXX: clang is dumb and ignores the sentinel attibute on function pointers?
};

#endif
