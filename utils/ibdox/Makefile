SRC := $(wildcard *.c)
RES := $(wildcard *.htm) $(wildcard *.txt)
OBJ := $(SRC:.c=.o)
RSO := $(addsuffix .o,$(RES))

.SUFFIXES:

ibdox: $(OBJ) $(RSO)
	gcc $^ -o $@ -ldl

dist/index.html: ibdox
	./$< ../../modules > $@

run: dist/index.html

$(OBJ): %.o: %.c
	gcc -c -std=gnu99 -D_GNU_SOURCE $< -o $@

$(RSO): %.o: %
	ld -r -b binary -z noexecstack $< -o $@

clean:
	$(RM) $(OBJ) $(RSO) ibdox dist/index.html

.PHONY: clean run
