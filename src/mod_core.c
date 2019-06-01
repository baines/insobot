#include <stdio.h>
#include <string.h>
#include <argz.h>
#include "module.h"
#include "stb_sb.h"
#include "inso_utils.h"

static void core_cmd     (const char*, const char*, const char*, int);
static bool core_save    (FILE*);
static void core_join    (const char* chan, const char* name);
static void core_part    (const char* chan, const char* name);
static bool core_meta    (const char*, const char*, int);
static bool core_init    (const IRCCoreCtx*);
static void core_quit    (void);
static void core_connect (const char*);
static void core_mod_msg (const char* sender, const IRCModMsg* msg);

enum { CMD_MODULES, CMD_MOD_ON, CMD_MOD_OFF, CMD_MOD_INFO, CMD_JOIN, CMD_LEAVE };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "core",
	.desc       = "Joins initial channels and manages permissons of other modules.",
	.priority   = 10000,
	.flags      = IRC_MOD_GLOBAL,
	.on_cmd     = &core_cmd,
	.on_save    = &core_save,
	.on_join    = &core_join,
	.on_part    = &core_part,
	.on_meta    = &core_meta,
	.on_init    = &core_init,
	.on_quit    = &core_quit,
	.on_connect = &core_connect,
	.on_mod_msg = &core_mod_msg,
	.commands = DEFINE_CMDS (
		[CMD_MODULES]  = CMD1("m")     CMD1("modules"),
		[CMD_MOD_ON]   = CMD1("mon")   CMD1("modon"),
		[CMD_MOD_OFF]  = CMD1("moff")  CMD1("modoff"),
		[CMD_MOD_INFO] = CMD1("minfo") CMD1("modinfo"),
		[CMD_JOIN]     = CMD1("join"),
		[CMD_LEAVE]    = CMD1("leave")
	),
	.cmd_help = DEFINE_CMDS (
		[CMD_MODULES]  = "| Displays which modules are enabled/disabled for the current channel.",
		[CMD_MOD_ON]   = "<mod> | Enables the module named <mod>.",
		[CMD_MOD_OFF]  = "<mod> | Disables the module named <mod>.",
		[CMD_MOD_INFO] = "<mod> | Shows the description for the <mod> module.",
		[CMD_JOIN]     = "<chan> | Instructs the bot to join <chan>",
		[CMD_LEAVE]    = "| Leaves the current channel"
	),
	.help_url = "https://insobot.handmade.network/forums/t/2385"
};

static const IRCCoreCtx* ctx;

struct chan {
	char*  name;
	char*  mod_list_argz;
	size_t mod_list_len;

	bool   should_join;
	bool   done_join;
};

static sb(struct chan) core_chans;

static void core_quit(void){
	sb_each(c, core_chans){
		free(c->name);
		free(c->mod_list_argz);
	}
	sb_free(core_chans);
}

static bool reload_file(void){
	FILE* f = fopen(ctx->get_datafile(), "r");
	if(!f) return false;

	core_quit();

	char* chan = NULL;
	char* modules = NULL;

	while(fscanf(f, "%ms %m[^\n]\n", &chan, &modules) == 2){
		bool join = true;

		if(chan[0] == ';'){
			join = false;
			memmove(chan, chan+1, strlen(chan));
		}

		struct chan c = {
			.name = strdup(chan),
			.should_join = join,
		};
		argz_create_sep(modules, ' ', &c.mod_list_argz, &c.mod_list_len);
		sb_push(core_chans, c);

		free(chan); chan = NULL;
		free(modules); modules = NULL;
	}

	free(chan);
	free(modules);

	fclose(f);

	return true;
}

static bool core_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return reload_file();
}

static struct chan* core_get_or_add(const char* chan_name){
	sb_each(c, core_chans){
		if(strcmp(c->name, chan_name) == 0){
			return c;
		}
	}

	struct chan new_chan = {
		.name = strdup(chan_name),
		.should_join = false,
	};

	IRCModuleCtx** modules = ctx->get_modules(GET_MODULES_CHAN_SPECIFIC);
	for(IRCModuleCtx** m = modules; *m; ++m){
		if((*m)->flags & IRC_MOD_DEFAULT){
			argz_add(&new_chan.mod_list_argz, &new_chan.mod_list_len, (*m)->name);
		}
	}

	sb_push(core_chans, new_chan);
	return &sb_last(core_chans);
}

static char* mod_find(struct chan* chan, const char* mod){
	char* p = NULL;
	while((p = argz_next(chan->mod_list_argz, chan->mod_list_len, p))){
		if(strcmp(p, mod) == 0){
			break;
		}
	}
	return p;
}

static void core_cmd(const char* chan, const char* name, const char* arg, int cmd){

	bool is_wlist = strcasecmp(chan+1, name) == 0 || inso_is_wlist(ctx, name);
	bool is_admin = is_wlist || inso_is_admin(ctx, name);

	if(!is_wlist)
		return;

	IRCModuleCtx** modules = ctx->get_modules(GET_MODULES_CHAN_SPECIFIC);
	struct chan*   info    = core_get_or_add(chan);

	switch(cmd){
		case CMD_MODULES: {
			char buff[1024];
			char *b = buff;
			size_t buflen = sizeof(buff);
			snprintf_chain(&b, &buflen, "Modules for %s: ", chan);

			for(; *modules; ++modules){
				const char* box = mod_find(info, (*modules)->name) ? "☑" : "☐";
				snprintf_chain(&b, &buflen, "%s %s, ", box, (*modules)->name);
			}

			ctx->send_msg(chan, "%s", buff);
		} break;

		case CMD_MOD_ON: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Which module?", name);
				break;
			}

			bool found = false;
			for(; *modules; ++modules){
				if(strcmp((*modules)->name, arg) == 0){
					if(mod_find(info, arg)){
						ctx->send_msg(chan, "%s: That module is already enabled here!", name);
					} else {
						argz_add(&info->mod_list_argz, &info->mod_list_len, arg);
						ctx->send_msg(chan, "%s: Enabled module %s.", name, arg);
						ctx->save_me();
					}

					found = true;
					break;
				}
			}

			if(!found){
				ctx->send_msg(chan, "%s: I haven't heard of that module...", name);
			}
		} break;

		case CMD_MOD_OFF: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Which module?", name);
				break;
			}

			bool found = false;
			for(; *modules; ++modules){
				if(strcmp((*modules)->name, arg) == 0){
					char* mod = mod_find(info, (*modules)->name);
					if(mod){
						argz_delete(&info->mod_list_argz, &info->mod_list_len, mod);
						ctx->send_msg(chan, "%s: Disabled module %s.", name, (*modules)->name);
						ctx->save_me();
					} else {
						ctx->send_msg(chan, "%s: That module is already disabled here!", name);
					}

					found = true;
					break;
				}
			}

			if(!found){
				ctx->send_msg(chan, "%s: I haven't heard of that module...", name);
			}
		} break;

		case CMD_MOD_INFO: {
			if(!*arg++){
				ctx->send_msg(chan, "%s: Which module?", name);
				break;
			}

			modules = ctx->get_modules(GET_MODULES_ALL);

			bool found = false;
			for(; *modules; ++modules){
				if(strcmp((*modules)->name, arg) == 0){
					ctx->send_msg(chan, "%s: %s: %s", name, (*modules)->name, (*modules)->desc);
					found = true;
					break;
				}
			}

			if(!found){
				ctx->send_msg(chan, "%s: I haven't heard of that module...", name);
			}
		} break;

		case CMD_JOIN: {
			if(!is_admin) break;

			if(!*arg++){
				ctx->send_msg(chan, "%s: Join where exactly?", name);
			} else {
				ctx->join(arg);
				ctx->send_msg(chan, "%s: Joining %s.", name, arg);
			}
		} break;

		case CMD_LEAVE: {
			if(!is_admin) break;
			ctx->send_msg(chan, "Goodbye, %s.", name);
			ctx->part(chan);
		} break;
	}
}

static bool core_meta(const char* modname, const char* chan, int callback_id){
	struct chan* info = core_get_or_add(chan);
	return mod_find(info, modname);
}

static bool core_save(FILE* file){
	sb_each(c, core_chans){
		if(!c->should_join){
			fputc(';', file);
		}

		fprintf(file, "%s\t", c->name);

		char* p = NULL;
		while((p = argz_next(c->mod_list_argz, c->mod_list_len, p))){
			fprintf(file, "%s ", p);
		}

		fputc('\n', file);
	}

	return true;
}

static void core_connect(const char* serv){
	sb_each(c, core_chans){
		c->done_join = false;
	}

	if(strcasecmp(serv, "irc.chat.twitch.tv") == 0 || getenv("IRC_IS_TWITCH")){
		ctx->send_raw("CAP REQ :twitch.tv/membership");

		if(ctx->get_info(IRC_INFO_CAN_PARSE_TAGS)){
			ctx->send_raw("CAP REQ :twitch.tv/tags");
		}
	}

	char* nspass = getenv("IRC_NICKSERV_PASS");
	if(nspass){
		ctx->send_msg("nickserv", "IDENTIFY %s", nspass);
	}

	{
		const char* c;
		if((c = getenv("IRC_CHAN"))){
			for(;;){
				size_t n = strcspn(c, ",\t ");
				core_get_or_add(strndupa(c, n))->should_join = true;
				if(!c[n]) break;
				c += n + 1;
			}
		} else if((c = getenv("IRC_ADMIN"))){
			core_get_or_add(c)->should_join = true;
		}
	}

	sb_each(c, core_chans){
		if(c->should_join){
			ctx->join(c->name);
			c->done_join = true;
			break;
		}
	}
}

static void core_join(const char* chan, const char* name){
	if(strcmp(name, ctx->get_username()) != 0) return;

	struct chan* info = core_get_or_add(chan);
	info->should_join = info->done_join = true;

	bool found = false;
	sb_each(c, core_chans){
		if(c->should_join && !c->done_join){
			ctx->join(c->name);
			found = c->done_join = true;
			break;
		}
	}

	if(!found){
		ctx->save_me();
	}
}

static void core_part(const char* chan, const char* name){
	if(strcmp(name, ctx->get_username()) != 0) return;

	struct chan* info = core_get_or_add(chan);
	info->should_join = false;
	ctx->save_me();
}

static void core_mod_msg(const char* sender, const IRCModMsg* msg){
	const char* admin = getenv("IRC_ADMIN");

	if(admin && (strcmp(msg->cmd, "check_whitelist") == 0 ||
			strcmp(msg->cmd, "check_admin") == 0) && strcmp((char*)msg->arg, admin) == 0){
		msg->callback(true, msg->cb_arg);
	}

	else if(strcmp(msg->cmd, "check_chan_enabled") == 0){
		struct chan* info = core_get_or_add((char*)msg->arg);
		bool enabled = mod_find(info, sender);
		msg->callback(enabled, msg->cb_arg);
	}
}
