#include <libircclient.h>
#include <string.h>
#include <stdio.h>
#include <err.h>
#include <signal.h>
#include "config.h"

static const char *user, *pass, *serv, *chan, *port;

static const char* env_else(const char* env, const char* def){
	const char* c = getenv(env);
	return c ? c : def;
}

#define IRC_CALLBACK(x) static void irc_##x ( \
	irc_session_t* session, \
	const char*    event,   \
	const char*    origin,  \
	const char**   params,  \
	unsigned int   count    \
)

#define IRC_EV_CALLBACK(x) static void irc_##x ( \
	irc_session_t* session, \
	unsigned int   event,   \
	const char*    origin,  \
	const char**   params,  \
	unsigned int   count    \
)

IRC_CALLBACK(on_connect) {

	char *channels = strdup(chan),
	     *state = NULL,
	     *c = strtok_r(channels, ", ", &state);
	
	irc_send_raw(session, "CAP REQ :twitch.tv/membership");
	
	do {
		printf("Joining %s\n", c);
		irc_cmd_join(session, c, NULL);
	} while((c = strtok_r(NULL, ", ", &state)));
	
	free(channels);
}

IRC_CALLBACK(on_chat_msg) {
	if(count < 2) return;

	const char* channel = params[0];
	const char* msg = params[1];
	
	if(!msg) return;
	
	markov_recv(session, channel, origin, msg);
}

IRC_CALLBACK(on_join) {
	markov_add_name(origin);
}

IRC_CALLBACK(on_part) {
	markov_del_name(origin);
}

IRC_EV_CALLBACK(on_numeric) {
	if(event == LIBIRC_RFC_RPL_NAMREPLY && count >= 4 && params[3]){
		char *names = strdup(params[3]), 
		     *state = NULL,
		     *n     = strtok_r(names, " ", &state);
		
		do {
			markov_add_name(n);
		} while((n = strtok_r(NULL, " ", &state)));
		
		free(names);
	} else {
		printf(". . . Numeric [%u]\n", event);
	}
}

static irc_session_t* irc_ctx = NULL;

static void handle_sig(int n){
	if(irc_ctx){
		irc_cmd_quit(irc_ctx, "goodbye");
		irc_disconnect(irc_ctx);
	}
}

int main(void){

	srand(time(0));
	signal(SIGINT, &handle_sig);
	
	user = env_else("IRC_USER", BOT_NAME);
	pass = env_else("IRC_PASS", BOT_PASS);
	serv = env_else("IRC_SERV", "irc.nonexistent.domain");
	chan = env_else("IRC_CHAN", "#nowhere");
	port = env_else("IRC_PORT", "6667");
			
	irc_callbacks_t callbacks = {
		.event_connect = irc_on_connect,
		.event_channel = irc_on_chat_msg,
		.event_join    = irc_on_join,
		.event_part    = irc_on_part,
		.event_numeric = irc_on_numeric
	};
	
	irc_ctx = irc_create_session(&callbacks);
	irc_option_set(irc_ctx, LIBIRC_OPTION_STRIPNICKS);

	if(irc_connect(irc_ctx, serv, atoi(port), pass, user, user, user) != 0){
		errx(1, "couldn't connect");
	}
	
	markov_read("chains.txt");
	
	irc_run(irc_ctx);
	irc_destroy_session(irc_ctx);
	
	markov_write("chains.txt");
	
	return 0;
}
