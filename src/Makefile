CFLAGS := -D_GNU_SOURCE -Wall -Wno-pointer-sign -std=gnu99 -g -fsanitize=address

headers    := $(wildcard *.h)
module_src := $(wildcard mod_*.c)
module_out := $(patsubst %.c,../modules/%.so,$(module_src))

all: ../insobot $(module_out)

../insobot: insobot.c $(headers)
	$(CC) $(CFLAGS) -I/usr/include/libircclient $< -o $@ -lircclient -ldl -lrt -lpthread -lcurl

../modules:
	mkdir $@

../modules/%.so: %.c $(headers) | ../modules
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ -Wl,--as-needed -lcurl -lyajl -lz -lcairo
	
clean:
	rm -f ../insobot $(module_out) 
