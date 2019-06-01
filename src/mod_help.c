#include "module.h"
#include "string.h"
#include "config.h"
#include "inso_utils.h"
#include "module_msgs.h"

static bool help_init (const IRCCoreCtx*);
static void help_cmd  (const char*, const char*, const char*, int);
static void help_pm   (const char*, const char*);

enum { CMD_HELP, CMD_BOTINFO, CMD_CMDS };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "help",
	.desc     = "Shows available commands",
	.flags    = IRC_MOD_GLOBAL,
	.on_init  = help_init,
	.on_cmd   = &help_cmd,
	.on_pm    = &help_pm,
	.commands = DEFINE_CMDS (
		[CMD_HELP]    = CMD("help"),
		[CMD_BOTINFO] = CMD(DEFAULT_BOT_NAME),
		[CMD_CMDS]    = CMD("commands")
	),
	.cmd_help = DEFINE_CMDS (
		[CMD_HELP]    = "[module|command] | Shows help for the given module/command (or lists all modules)",
		[CMD_BOTINFO] = "| Shows information about the bot",
		[CMD_CMDS]    = "| Sends a private message about the help system"
	),
	.help_url = "https://insobot.handmade.network/forums/t/2385"
};

static const IRCCoreCtx* ctx;

static bool help_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void get_mod_cmds(IRCModuleCtx* mod, char* buf, size_t sz){
	if(!mod->commands || !*mod->commands){
		inso_strcat(buf, sz, "<none>.");
		return;
	}

	*buf = 0;
	char cmd_buf[256];

	//XXX: only prints the first command alias, is that a good idea?
	for(const char** cmd = mod->commands; *cmd; ++cmd){

		const char* end = strchrnul(*cmd, ' ');

		memcpy(cmd_buf, *cmd, end - *cmd);
		cmd_buf[end - *cmd] = 0;

		inso_strcat(buf, sz, cmd_buf);
		inso_strcat(buf, sz, " ");
	}

}

static intptr_t help_alias_cb(intptr_t result, intptr_t arg){
	if(result) memcpy((AliasInfo*)arg, (AliasInfo*)result, sizeof(AliasInfo));
	return 0;
}

static void help_cmd(const char* chan, const char* name, const char* arg, int cmd){

	if(cmd == CMD_BOTINFO){
		ctx->send_msg(chan, "Hello %s, I'm %s based on code available at https://github.com/baines/insobot", name, ctx->get_username());
		return;
	}

	if(cmd == CMD_CMDS){
		ctx->send_msg(name, "You can find a list of commands at https://github.com/baines/insobot, or PM me with \"help\" for more info.");
		return;
	}

	// cmd == CMD_HELP

	IRCModuleCtx** mods = ctx->get_modules(false);

	if(*arg++){
		const size_t arg_len = strcspn(arg, " ");

		IRCModuleCtx* found_mod = NULL;
		for(IRCModuleCtx** m = mods; *m; ++m){
			if(strcasecmp((*m)->name, arg) == 0 && (*m)->commands){
				found_mod = *m;
				break;
			}
		}

		char cmd_buf[256];

		if(found_mod){
			get_mod_cmds(found_mod, cmd_buf, sizeof(cmd_buf));
			if(found_mod->help_url){
				ctx->send_msg(chan,
				              "[module] %s commands: [ %s]. [%s]. "
				              "Use " CONTROL_CHAR "help <command> for more info.",
				              found_mod->name, cmd_buf, found_mod->help_url);
			} else {
				ctx->send_msg(chan,
				              "[module] %s commands: [ %s]. "
				              "Use " CONTROL_CHAR "help <command> for more info.",
				              found_mod->name, cmd_buf);
			}
			return;
		}

		// TODO: skip control char of cmds?

		const char* found_cmd = NULL;
		size_t found_cmd_idx = 0;
		size_t found_len = 0;

		for(IRCModuleCtx** m = mods; *m; ++m){
			if(!(*m)->commands) continue;

			for(const char** cmd = (*m)->commands; *cmd; ++cmd){
				const char* str = strcasestr(*cmd, arg);
				if(!str) continue;

				size_t max_len = arg_len;

				if(str > *cmd){
					if(strchr(CONTROL_CHARS, str[-1])){
						--str;
						++max_len;
					} else if(str[-1] != ' '){
						continue;
					}
				}

				size_t len = strcspn(str, " ");
				if(len > max_len) continue;

				if(len > found_len){
					found_len = len;
					found_mod = *m;
					found_cmd = str;
					found_cmd_idx = cmd - (*m)->commands;
				}
			}
		}

		if(found_cmd){
			const char* help_text = found_mod->cmd_help ? found_mod->cmd_help[found_cmd_idx] : "No help available :(";
			ctx->send_msg(chan, "[mod_%s cmd]: %.*s %s", found_mod->name, (int)found_len, found_cmd, help_text);
			return;
		}

		if(*arg == '!') ++arg;

		AliasExistsMsg alias_msg = {
			.aliases = arg,
			.channel = (chan == name) ? NULL : chan
		};

		AliasInfo alias_info = {};
		MOD_MSG(ctx, "alias_info", &alias_msg, help_alias_cb, &alias_info);

		if(alias_info.content){
			const char* author = alias_info.author ?: "mod_alias";
			ctx->send_msg(chan, "[alias] !%s was created by %s, see " CONTROL_CHAR "help alias and " CONTROL_CHAR "ls(g)a", arg, author);
			return;
		}

		ctx->send_msg(chan, "Unknown module or command. Try " CONTROL_CHAR "help with no arguments to see available modules.");
	} else {
		char mod_names[256];
		*mod_names = 0;

		for(IRCModuleCtx** m = mods; *m; ++m){
			if((*m)->commands && (*m)->commands[0]){
				if(*mod_names){
					inso_strcat(mod_names, sizeof(mod_names), ", ");
				}
				inso_strcat(mod_names, sizeof(mod_names), (*m)->name);
			}
		}
		ctx->send_msg(chan, "%s: Use " CONTROL_CHAR "help <module|command> for more info. Available modules: %s.", name, mod_names);
	}
}

static void help_pm(const char* name, const char* msg){
	int len = inso_match_cmd(msg, irc_mod_ctx.commands[CMD_HELP], true);
	if(len > 0){
		help_cmd(name, name, msg + len, CMD_HELP);
	}
}
