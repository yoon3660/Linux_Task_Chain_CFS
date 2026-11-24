CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result
all: chain_stage chain_run
chain_stage: chain_stage.c chain.h
	$(CC) $(CFLAGS) -o $@ chain_stage.c
chain_run: chain_run.c chain.h
	$(CC) $(CFLAGS) -o $@ chain_run.c
clean:
	rm -f chain_stage chain_run
.PHONY: all clean
