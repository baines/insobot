CFLAGS   := -D_GNU_SOURCE -Wall -Wno-pointer-sign -std=c99 -g -fsanitize=address
CXXFLAGS := -D_GNU_SOURCE -Wall -std=c++1y -g -Wl,--version-script=fix-dlclose.vs

module_src := $(wildcard mod_*.c)
module_out := $(patsubst %.c,../modules/%.so,$(module_src)) ../modules/mod_markov.so

all: ../insobot $(module_out)

../insobot: insobot.c config.h module.h
	$(CC) $(CFLAGS) -I/usr/include/libircclient $< -o $@ -lircclient -ldl

../modules:
	mkdir $@

../modules/mod_markov.so: mod_markov.cpp module.h config.h ../modules
	$(CXX) $(CXXFLAGS) -fPIC -shared $< -o $@

../modules/%.so: %.c config.h module.h utils.h ../modules
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ -Wl,--as-needed -lcurl -lyajl
	
clean:
	rm -f ../insobot $(module_out) 
