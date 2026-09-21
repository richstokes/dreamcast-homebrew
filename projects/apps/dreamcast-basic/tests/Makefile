# Host-side build of the portable interpreter and editor, for tests.
CC ?= cc
# Contraction is off so transcripts match across compilers and CPUs.
CFLAGS ?= -std=gnu11 -O1 -g -Wall -Wextra -ffp-contract=off \
          -fsanitize=address,undefined
SOURCES = host.c ../basic.c ../screen.c ../av.c ../app.c
HEADERS = $(wildcard ../*.h)

all: host-basic

host-basic: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -lm

check: host-basic
	./run-tests.sh

clean:
	-rm -rf host-basic host-basic.dSYM out

.PHONY: all check clean
