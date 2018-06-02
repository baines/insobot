CFLAGS  := @CFLAGS
LDFLAGS := -Wl,--as-needed -Wl,--gc-sections

headers  := $(wildcard *.h)
module_c := $(wildcard mod_*.c)
module_o  = $(patsubst %.c,../modules/%.so,$(module_c))

common_h := $(wildcard inso_*.h)
common_o := $(patsubst %.h,../lib/%.o,$(common_h))

# Use this to only build certain modules; here is a reasonable default set.
MODULES := core alias filter haiku help imgmacro info karma linkinfo\
 markov poll psa twitch whitelist

# You can define it (and other stuff) in src/config.mk to avoid makefile changes
#  in git from overwriting your settings.
# Put "undefine MODULES" in there if you want all modules (not recommended)
-include config.mk

ifdef MODULES
    module_c := $(filter $(patsubst %,mod_%.c,$(MODULES)),$(module_c))
endif

# check for libraries

lz     := $(shell pkg-config --libs zlib  2>/dev/null)
lyajl  := $(shell pkg-config --libs yajl  2>/dev/null)
lcairo := $(shell pkg-config --libs cairo 2>/dev/null)

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

LIBS := ../lib/inso_common.a -lcurl -lm $(lyajl) $(lz) $(lcairo)

# main rules

all: ../insobot $(module_o)

../insobot: insobot.c $(headers)
	$(CC) $(CFLAGS) -I/usr/include/libircclient $< -o $@ -lircclient -ldl -lrt -lpthread -lcurl

../modules ../lib:
	mkdir $@

../modules/%.so: %.c ../lib/inso_common.a $(headers) | ../modules
	$(CC) $(CFLAGS) -fPIC $< -shared -o $@ $(LDFLAGS) $(LIBS)

../lib/inso_%.o: inso_%.h | ../lib
	$(CC) $(CFLAGS) -x c -fPIC -DINSO_IMPL -c $^ -o $@

../lib/inso_common.a: $(common_o)
	ar rcs $@ $^

# twc stuff

../modules/mod_twitter.so: ../lib/libtwc.a
../modules/mod_twitter.so: LIBS += ../lib/libtwc.a
../modules/mod_twitter.so: CFLAGS += -I../lib

../lib/libtwc.a: ../lib/makefile.twc
	$(MAKE) -C $(<D) -f $(<F)

# misc

clean:
	$(RM) ../modules/*.so $(common_o) ../lib/inso_common.a ../insobot

.PHONY: all clean
