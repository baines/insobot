#ifndef INSOBOT_MODULE_MSGS_H
#define INSOBOT_MODULE_MSGS_H

// Explanation

// MOD_MSG is a macro defined in module.h for passing messages between modules.
// It has the following syntax:
// 
//   MOD_MSG(IRCCoreCtx ctx, char* msg_id, arg, callback, user_defined);
//
// The callback parameter should have the following signature:
//
//   void callback(intptr_t result, intptr_t user_defined)
//
// The user_defined parameter is the same as given in the macro, and can be
// used to store the result somewhere without using a global.
//
// This macro is just a wrapper around ctx->send_mod_msg that adds casting.
// You can use that instead if you want.

// Before this macro / func returns, the callback you passed in will be called
// immediately by any module that chooses to respond to the given msg id
// (potentially multiple times). In theory multiple different modules could
// respond, but currently it's only 1:1.
//
// If there is no module that handles the given message, or if a module that
// does handle it is not loaded, then the callback will not be called at all.
// Code written using this system shouldn't assume it'll always get a response;
// it should handle the case where the callback is not called.


// Message List

// [L] means the arg is a space-separated List of strings in a single char*
// [M] means the callback will be called Multiple times with different results
// [F] means the result is malloc'd and should be Freed with free()
//     * if [F] is not present, then the result probably becomes invalid
//       after the callback returns, so copy it if you need to.
//
// Custom argument / return types will be defined in this file.

//    module     |        msg id          | arg type  | ret type  | desc
// --------------+------------------------+-----------+-----------+------
// mod_alias     | "alias_exists"         | char*[2]  | bool      | returns true if any of the given aliases exist. arg[0] is the alias list, arg[1] is the channel.
// mod_hmh       | "hmh_is_live"          | unused    | bool      | returns true if HMH is scheduled to be airing currently
// mod_karma     | "karma_get"            | char*     | int       | returns the total karma for the given name
// mod_markov    | "markov_gen"           | unused    | char* [F] | returns a malloc'd random markov sentence
// mod_notes     | "note_get_stream_start"| char* [L] | time_t    | returns the stream start time if a note is present for any of the channels given
// mod_schedule  | "sched_get" [M]        | char*     | SchedMsg* | returns all the known schedules for a given user name
// mod_schedule  | "sched_set" (TODO)     | SchedMsg* | bool      | sets the given schedule, or if start/end are 0, delete it. if sched_id < 0, use next available.
// mod_twitch    | "twitch_get_user_date" | char*     | time_t    | returns the account creation date of the given twitch user
// mod_twitch    | "twitch_is_live"       | char* [L] | bool      | returns true if any of the given channels are currently live
// mod_whitelist | "check_admin"          | char*     | bool      | returns true if the given name is an admin
// mod_whitelist | "check_whitelist"      | char*     | bool      | returns true if the given name is whitelisted
// mod_twitch    | "display_name"         | char*     | char*     | returns the display name of the given name (correct capitilzation or unicode variant) (needs tags support)


// Types

typedef struct {
	const char* user;
	int sched_id;
	time_t start;
	time_t end;
	const char* title;
	uint8_t repeat;
} SchedMsg;

#endif
