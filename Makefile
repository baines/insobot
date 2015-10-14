CFLAGS := -I/usr/include/libircclient -g

all: ircbot

ircbot: irc_glue.o irc_markov.o
	$(CXX) $(CFLAGS) $^ -o $@ -lircclient
	
irc_glue.o: irc_glue.c config.h
	$(CC) $(CFLAGS) -c $< -o $@
	
irc_markov.o: irc_markov.cpp config.h
	$(CXX) $(CFLAGS) -std=c++1y -c $< -o $@
	
clean:
	rm -f irc_glue.o irc_markov.o ircbot
