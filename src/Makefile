CFLAGS  := -D_GNU_SOURCE -Wall -ffunction-sections -fdata-sections -Wno-pointer-sign -std=gnu99 -g
LDFLAGS := -Wl,--as-needed -Wl,--gc-sections

headers    := $(wildcard *.h)
module_src := $(wildcard mod_*.c)
module_out  = $(patsubst %.c,../modules/%.so,$(module_src))

lyajl  != pkg-config --libs yajl  2>/dev/null
lcairo != pkg-config --libs cairo 2>/dev/null
lz     != pkg-config --libs zlib  2>/dev/null

ifndef lyajl
  module_src := $(filter-out mod_quotes.c mod_twitch.c mod_imgmacro.c mod_linkinfo.c,$(module_src))
endif
ifndef lcairo
  module_src := $(filter-out mod_imgmacro.c,$(module_src))
endif
ifndef lz
  module_src := $(filter-out mod_markov.c,$(module_src))
endif

all: ../insobot $(module_out)

../insobot: insobot.c $(headers)
	$(CC) $(CFLAGS) -I/usr/include/libircclient $< -o $@ -lircclient -ldl -lrt -lpthread -lcurl

../modules:
	mkdir $@

../inso_%.o: inso_%.h
	$(CC) $(CFLAGS) -x c -fPIC -DINSO_IMPL -c $^ -o $@

../inso_%.a: $(patsubst %.h,../%.o,$(wildcard inso_*.h))
	ar rcs $@ $^

../modules/%.so: %.c ../inso_common.a | $(headers) ../modules
	$(CC) $(CFLAGS) -fPIC $^ -shared -o $@ $(LDFLAGS) -lcurl $(lyajl) $(lcairo) $(lz)
	
clean:
	$(RM) ../insobot $(module_out) 
