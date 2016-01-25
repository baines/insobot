#include "module.h"

static void quotes_msg   (const char*, const char*, const char*);
static void quotes_save  (void);

static IRCModuleCtx mod_ctx = {
	.name     = "quotes",
	.desc     = "Saves per-channel quotes",
	.on_msg   = &quotes_msg,
	.on_save  = &quotes_save,
};

/*
   \q    <num>
   \qadd <text>
   \qdel <num>
   \qfix <num> <text>
   \qs   <text>

	// figure out gist api for storage?
*/

static void quotes_msg(const char* chan, const char* name, const char* msg){

}

static void quotes_save(void){

}
