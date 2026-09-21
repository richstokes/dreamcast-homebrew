TARGET = dreamcast-browser.elf
OBJS = dreamcast-browser.o net.o html.o images.o render.o osk.o address.o \
       bookmarks.o storage.o romdisk.o

KOS_BUILD_SUBARCHS = pristine
KOS_ROMDISK_DIR = romdisk
KOS_CSTD = -std=gnu17

LDLIBS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lstb_image -lm -lpthread

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET) romdisk.img

dreamcast-browser.o net.o html.o images.o render.o osk.o address.o bookmarks.o \
    storage.o: browser.h
dreamcast-browser.o storage.o: storage.h

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) $(LDLIBS)

run: $(TARGET)
	./run-flycast.sh --skip-build

update-cacert:
	./scripts/update-cacert.sh

# Parser, keyboard, address-bar, and bookmark tests on the host compiler.
host-test:
	$(MAKE) -C tests

.PHONY: all clean run update-cacert host-test

# A normal build must never package temporary integration credentials.
.PHONY: release-check
ifeq ($(filter -DBROWSER_FORM_SELF_TEST,$(CPPFLAGS)),)
release-check:
	@test ! -f romdisk/test-credentials.txt || { echo "Remove romdisk/test-credentials.txt before a normal build."; exit 1; }
else
release-check:
endif
$(OBJS): | release-check
