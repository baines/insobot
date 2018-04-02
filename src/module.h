#ifndef INSOBOT_MODULE_H
#define INSOBOT_MODULE_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include "config.h"

typedef struct IRCCoreCtx_ IRCCoreCtx;
typedef struct IRCModMsg_ IRCModMsg;

// defined by a module to provide info & callbacks to the core.
typedef struct IRCModuleCtx_ {

	const char*        name;
	const char*        desc;
	const char**       commands; // null-terminated, use DEFINE_CMDS macro
	intptr_t           priority; // bigger number = higher priority
	uintptr_t          flags;

	bool (*on_init)    (const IRCCoreCtx* ctx);
	void (*on_quit)    (void);

	// IRC event callbacks
	void (*on_connect) (const char* serv);
	void (*on_msg)     (const char* chan, const char* name, const char* msg);
	void (*on_action)  (const char* chan, const char* name, const char* msg);
	void (*on_pm)      (const char* name, const char* msg);
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
	void (*on_tick)    (time_t now);

	// called if something was written to stdin
	void (*on_stdin)   (const char* text);

	// called when a message is sent
	void (*on_msg_out) (const char* chan, const char* msg);

	// called on receipt of an inter-process message
	void (*on_ipc)     (int sender_id, const uint8_t* data, size_t data_len);

	// called before a message is sent out, to allow filtering.
	// chan will be NULL for a raw message.
	// set msg[0] to '\0' to prevent the message being sent at all.
	void (*on_filter)  (size_t msg_id, const char* chan, char* msg, size_t msg_len);

	// called on an unknown IRC event
	void (*on_unknown) (const char* event, const char* origin, const char** params, size_t num_params);

	// help / usage for commands (use DEFINE_CMDS)
	const char** cmd_help;

	// link to online guide / documentation for this module
	const char* help_url;

} IRCModuleCtx;

// incremented when new functions are added to IRCCoreCtx
#define INSO_CORE_API_VERSION 3

// API version history:
// 1: Initial version.
// 2: send_msg and send_raw now return an ID for the message.
//    This will be passed to the filter function of IRCModuleCtx.
// 3: Added gen_event function

// passed to modules to provide functions for them to use.
struct IRCCoreCtx_ {

	const uintptr_t api_version;

	intptr_t       (*get_info)     (int id); // see IRC_INFO enum below
	const char*    (*get_username) (void);
	const char*    (*get_datafile) (void);
	IRCModuleCtx** (*get_modules)  (bool channel_mods_only); // null terminated
	const char**   (*get_channels) (void); // null terminated
	const char**   (*get_nicks)    (const char* chan, int* count_out);
	void           (*join)         (const char* chan);
	void           (*part)         (const char* chan);
	size_t         (*send_msg)     (const char* chan, const char* fmt, ...) __attribute__ ((format (printf, 2, 3)));
	size_t         (*send_raw)     (const char* raw);
	void           (*send_ipc)     (int target, const void* data, size_t data_len); // target 0 == broadcast
	void           (*send_mod_msg) (IRCModMsg* msg);
	void           (*save_me)      (void);
	void           (*log)          (const char* fmt, ...) __attribute__ ((format (printf, 1, 2)));
	void           (*strip_colors) (char* msg);
	bool           (*responded)    (void); // true if send_msg was called for the current msg already
	bool           (*get_tag)      (size_t index, const char** k, const char** v); // IRCv3 tag iteration (if available)

	// === Since API v3 ===
	// Queues a synthetic IRC event that will trigger modules' callbacks as if it were an event from the IRC Server.
	// The variadic args should be the same as for the corresponding on_ callback in IRCModuleCtx.
	// Supported callbacks are in the enum below.
	void           (*gen_event)    (int which, ...);
};

enum {
	IRC_INFO_CAN_PARSE_TAGS, // bool
};

// used for on_meta callback & gen_event.
enum  {
	IRC_CB_MSG,
	IRC_CB_CMD, // on_meta only
	IRC_CB_JOIN,
	IRC_CB_PART,
	IRC_CB_ACTION,
	IRC_CB_NICK,
	IRC_CB_PM,
};

// used for the flags field of IRCModuleCtx
enum {
	IRC_MOD_GLOBAL  = 1, // not a module that can be enabled / disabled per channel
	IRC_MOD_DEFAULT = 2, // enabled by default when joining new channels
};

// used for inter-module communication messages
struct IRCModMsg_ {
	const char* cmd;
	intptr_t    arg;
	intptr_t    (*callback)(intptr_t result, intptr_t arg);
	intptr_t    cb_arg;
};

#define MOD_MSG(ctx, cmd, arg, cb, cb_arg) (ctx)->send_mod_msg(\
	&(IRCModMsg){ (cmd), (intptr_t)(arg), (intptr_t(*)())(cb), (intptr_t)(cb_arg) }\
)

#define DEFINE_CMDS(...) (const char*[]) {\
	__VA_ARGS__,\
	0\
}

#define CMD1(x) CONTROL_CHAR x " "
#define CMD2(x) CONTROL_CHAR_2 x " "
#define CMD(x) CMD1(x) CMD2(x)

#endif
