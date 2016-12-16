CFLAGS  := -D_GNU_SOURCE -Wall -ffunction-sections -fdata-sections -Wno-pointer-sign -std=gnu99 -g -fsanitize=address
LDFLAGS := -Wl,--as-needed -Wl,--gc-sections

headers  := $(wildcard *.h)
module_c := $(wildcard mod_*.c)
module_o = $(patsubst %.c,../modules/%.so,$(module_c))

common_h := $(wildcard inso_*.h)
common_o := $(patsubst %.h,../lib/%.o,$(common_h))

lz     != pkg-config --libs zlib  2>/dev/null
lyajl  != pkg-config --libs yajl  2>/dev/null
lcairo != pkg-config --libs cairo 2>/dev/null

ifndef lyajl
    $(error libyajl2 is required)
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
