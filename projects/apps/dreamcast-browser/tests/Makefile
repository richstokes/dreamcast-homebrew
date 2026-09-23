# Host-side unit tests for the browser's platform-independent modules.
# These build with the machine's own C compiler and need no KallistiOS.

HOST_CC ?= cc
PYTHON ?= python3
CFLAGS = -std=gnu17 -O1 -g -Wall -Wextra -Werror -DBROWSER_QUIET -I..
SOURCES = run_tests.c stubs.c html_test.c ui_test.c ../html.c ../osk.c \
          ../address.c ../bookmarks.c
TARGET = host-tests
IMAGE_TARGET = image-tests

all: test

$(TARGET): $(SOURCES) test.h ../browser.h
	$(HOST_CC) $(CFLAGS) -o $@ $(SOURCES)

$(IMAGE_TARGET): image_test.c ../images.c ../browser.h test.h \
                include/kos/timer.h include/stb_image/stb_image.h
	$(HOST_CC) $(CFLAGS) -Iinclude -o $@ image_test.c

test: $(TARGET) $(IMAGE_TARGET)
	./$(TARGET)
	./$(IMAGE_TARGET)
	$(PYTHON) profile_test.py
	$(PYTHON) serve_fixtures.py --check

clean:
	-rm -f $(TARGET)
	-rm -rf $(TARGET).dSYM
	-rm -f $(IMAGE_TARGET)
	-rm -rf $(IMAGE_TARGET).dSYM

.PHONY: all test clean
