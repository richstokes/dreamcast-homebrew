# Host-side unit tests for the browser's platform-independent modules.
# These build with the machine's own C compiler and need no KallistiOS.

HOST_CC ?= cc
CFLAGS = -std=gnu17 -O1 -g -Wall -Wextra -Werror -DBROWSER_QUIET -I..
SOURCES = run_tests.c stubs.c html_test.c ui_test.c ../html.c ../osk.c \
          ../address.c ../bookmarks.c
TARGET = host-tests

all: test

$(TARGET): $(SOURCES) test.h ../browser.h
	$(HOST_CC) $(CFLAGS) -o $@ $(SOURCES)

test: $(TARGET)
	./$(TARGET)

clean:
	-rm -f $(TARGET)
	-rm -rf $(TARGET).dSYM

.PHONY: all test clean
