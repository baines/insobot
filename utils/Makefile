STUFF := schedule_api mod_core_upgrade

all: $(STUFF)

$(STUFF): %: %.c
	gcc -g -D_GNU_SOURCE -std=c99 $< -o $@ -lyajl

clean:
	$(RM) $(STUFF)

.PHONY: clean
