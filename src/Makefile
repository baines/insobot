CFLAGS  := -D_GNU_SOURCE -Wall -ffunction-sections -fdata-sections -std=gnu99 \
 -g -Werror=implicit-function-declaration -Wno-pointer-sign -funwind-tables
LDFLAGS := -Wl,--as-needed -Wl,--gc-sections

headers  := $(wildcard *.h)
module_c := $(wildcard mod_*.c)
module_o = $(patsubst %.c,../modules/%.so,$(module_c))

common_h := $(wildcard inso_*.h)
common_o := $(patsubst %.h,../lib/%.o,$(common_h))

lz     := $(shell pkg-config --libs zlib  2>/dev/null)
lyajl  := $(shell pkg-config --libs yajl  2>/dev/null)
lcairo := $(shell pkg-config --libs cairo 2>/dev/null)

# Use this to only build certain modules
# MODULES := meta admin chans whitelist markov linkinfo
# And put it (and other stuff) in here that you don't want overridden by a git pull
-include config.mk

ifdef MODULES
    module_c := $(filter $(patsubst %,mod_%.c,$(MODULES)),$(module_c))
endif

ifndef lyajl
    $(error libyajl2 is required for JSON parsing & generation. Please install it)
endif

ifndef lz
    $(warning warning: zlib is missing. mod_markov will not be built.)
    module_c := $(filter-out mod_markov.c,$(module_c))
endif

ifndef lcairo
    $(warning warning: libcairo is missing. mod_imgmacro will not be built.)
    module_c := $(filter-out mod_imgmacro.c,$(module_c))
endif

all: ../insobot $(module_o)

../insobot: insobot.c $(headers)
	$(CC) $(CFLAGS) -I/usr/include/libircclient $< -o $@ -lircclient -ldl -lrt -lpthread -lcurl

../modules ../lib:
	mkdir $@

../modules/%.so: %.c ../lib/inso_common.a | $(headers) ../modules
	$(CC) $(CFLAGS) -fPIC $^ -shared -o $@ $(LDFLAGS) -lcurl $(lyajl) $(lz) $(lcairo)

../lib/inso_%.o: inso_%.h | ../lib
	$(CC) $(CFLAGS) -x c -fPIC -DINSO_IMPL -c $^ -o $@

../lib/inso_common.a: $(common_o)
	ar rcs $@ $^

clean:
	$(RM) $(module_o) $(common_o) ../lib/inso_common.a ../insobot
