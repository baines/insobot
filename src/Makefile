CFLAGS   := -D_GNU_SOURCE -std=c99 -g -fsanitize=address
CXXFLAGS := -D_GNU_SOURCE -std=c++1y -g -Wl,--version-script=fix-dlclose.vs

module_src := $(wildcard mod_*.c)
module_out := $(patsubst %.c,../modules/%.so,$(module_src)) ../modules/mod_markov.so

all: ../insobot $(module_out)

../insobot: insobot.c config.h module.h
	$(CC) $(CFLAGS) -I/usr/include/libircclient $< -o $@ -lircclient -ldl

../modules/mod_markov.so: mod_markov.cpp module.h config.h
	$(CXX) $(CXXFLAGS) -fPIC -shared $< -o $@

../modules/%.so: %.c config.h module.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ -Wl,--as-needed -lcurl -lyajl
	
clean:
	rm -f ../insobot $(module_out) 
