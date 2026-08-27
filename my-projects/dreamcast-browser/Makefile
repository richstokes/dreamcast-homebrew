TARGET = dreamcast-browser.elf
OBJS = dreamcast-browser.o net.o html.o images.o render.o romdisk.o

KOS_BUILD_SUBARCHS = pristine
KOS_ROMDISK_DIR = romdisk
KOS_CSTD = -std=gnu17

LDLIBS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lstb_image -lm -lpthread

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET) romdisk.img

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) $(LDLIBS)

run: $(TARGET)
	./run-flycast.sh --skip-build

update-cacert:
	./scripts/update-cacert.sh

.PHONY: all clean run update-cacert
