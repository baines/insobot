CFLAGS   := -D_GNU_SOURCE -Wall -Wno-pointer-sign -std=gnu99 -g

module_src := $(wildcard mod_*.c)
module_out := $(patsubst %.c,../modules/%.so,$(module_src))

all: ../insobot $(module_out)

../insobot: insobot.c config.h module.h stb_sb.h
	$(CC) $(CFLAGS) -I/usr/include/libircclient $< -o $@ -lircclient -ldl -lrt -lpthread -lcurl

../modules:
	mkdir $@

../modules/%.so: %.c config.h module.h utils.h stb_sb.h | ../modules
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ -Wl,--as-needed -lcurl -lyajl -lz
	
clean:
	rm -f ../insobot $(module_out) 
