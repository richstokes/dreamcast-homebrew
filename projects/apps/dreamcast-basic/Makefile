TARGET = dreamcast-basic.elf
OBJS = dreamcast-basic.o app.o basic.o screen.o av.o romdisk.o
HEADERS = app.h av.h basic.h font8x16.h platform.h screen.h

KOS_BUILD_SUBARCHS = pristine
# The bundled example programs are served from /rd.
KOS_ROMDISK_DIR = examples

# Test builds mirror program output to the serial console. AUTORUN=STARTREK
# loads and runs one example at boot; AUTOKEYS=SRS_LRS_ types keys (_ is Enter,
# @5 is F5, @S is a space; see plat_key).
ifdef AUTORUN
KOS_CFLAGS += -DBASIC_AUTORUN=$(AUTORUN)
endif
ifdef AUTOKEYS
KOS_CFLAGS += -DBASIC_AUTOKEYS=$(AUTOKEYS)
endif

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET) romdisk.img

$(filter-out romdisk.o,$(OBJS)): $(HEADERS)

romdisk.img: $(wildcard examples/*)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lm

run: $(TARGET)
	./run-flycast.sh --skip-build

check:
	$(MAKE) -C tests check

.PHONY: all clean run check
