CFLAGS := -D_GNU_SOURCE -std=c99 -g -fsanitize=address

module_src := $(wildcard mod_*.c)
module_out := $(patsubst %.c,../modules/%.so,$(module_src))

all: ../insobot $(module_out)

../insobot: insobot.c config.h
	$(CC) $(CFLAGS) -I/usr/include/libircclient $< -o $@ -lircclient -ldl
	
../modules/%.so: %.c config.h module.h
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@
	
clean:
	rm -f ../insobot $(module_out) 
