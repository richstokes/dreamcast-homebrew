TARGET := chroma-circuit.elf
OBJS := chroma-circuit.o
TOOLCHAIN ?= $(if $(KOS_CC_BASE),$(KOS_CC_BASE)/bin,$(HOME)/.local/share/dreamcast/sh-elf/bin)
AS := $(TOOLCHAIN)/sh-elf-as
LD := $(TOOLCHAIN)/sh-elf-ld
READELF := $(TOOLCHAIN)/sh-elf-readelf
NM := $(TOOLCHAIN)/sh-elf-nm

ASFLAGS := --isa=sh4 --little
LDFLAGS := -m shlelf -T linker.ld --gc-sections -nostdlib

.PHONY: all clean run verify

all: $(TARGET)

chroma-circuit.o: chroma-circuit.s hyperfold.inc strange-form.inc product-form.inc elevated.inc navigator.inc aica-music.inc
	$(AS) $(ASFLAGS) -o $@ $<

$(TARGET): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

verify: $(TARGET)
	file $(TARGET)
	$(READELF) -h -l $(TARGET)
	$(NM) -n $(TARGET) | awk '$$3 == "_start" { print }'
	@test "$$($(NM) -n $(TARGET) | awk '$$3 == "_start" { print $$1 }')" = "8c010000"
	@test -z "$$($(NM) -u $(TARGET))"
	@echo "Verified _start at 0x8c010000 with no undefined symbols."

run: $(TARGET)
	./run-flycast.sh --skip-build

clean:
	-rm -f $(OBJS) $(TARGET)
