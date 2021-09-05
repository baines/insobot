srcs   := $(wildcard src/*.c)
html   := $(wildcard src/*.html)
objs   := $(srcs:src/%.c=build/%.o) $(html:src/%.html=build/%.html.o)
CFLAGS := -std=gnu99 -D_GNU_SOURCE -g -Wall -Wextra -Werror -Wno-missing-field-initializers

all: cgi-bin/ibstat

cgi-bin/ibstat: $(objs) | cgi-bin
	$(CC) $^ -o $@ -lrt -lm -lcrypt

cgi-bin build:
	mkdir $@

build/%.o: src/%.c | build
	$(CC) -c $(CFLAGS) $< -o $@

build/%.html.o: src/%.html | build
	(cd $(<D) && ld -r -b binary -z noexecstack $(<F) -o ../$@)

clean:
	$(RM) $(objs) cgi-bin/ibstat

.PHONY: clean
