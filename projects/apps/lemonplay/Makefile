TARGET = lemonplay.elf
SRCS = src/main.c src/gfx.c src/input.c src/netlink.c src/http.c src/upnp.c \
       src/player.c src/mpeg.c src/bench.c src/settings.c

KOS_BUILD_SUBARCHS = pristine
KOS_CSTD = -std=gnu17

# VARIANT builds a separately named ELF from its own objects, e.g.
#   make VARIANT=qa EXTRA_CFLAGS='-DLP_SERVER=\"http://192.168.1.5:8200/description.xml\"'
VARIANT ?=
EXTRA_CFLAGS ?=
VARIANT_FLAGS_qa = -DLP_QA -DLP_START_SECONDS=300
VARIANT_FLAGS_bench = -DLP_BENCH -DLP_START_SECONDS=300
VARIANT_FLAGS_decodebench = -DLP_DECODE_BENCH
VARIANT_FLAGS_qaend = -DLP_QA -DLP_QA_END
BUILD = build/$(if $(VARIANT),$(VARIANT),release)
ELF = $(if $(VARIANT),lemonplay-$(VARIANT).elf,$(TARGET))
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o)

CFLAGS_LP = -Isrc -Ithird_party/pl_mpeg -Wall -Wno-unused-function $(VARIANT_FLAGS_$(VARIANT)) $(EXTRA_CFLAGS)

all: $(ELF)

include $(KOS_BASE)/Makefile.rules

# Rebuild when the flags change, not only the sources.
FLAGS_STAMP = $(BUILD)/flags
$(FLAGS_STAMP): FORCE | $(BUILD)
	@echo '$(CFLAGS_LP)' | cmp -s - $@ || echo '$(CFLAGS_LP)' > $@

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) $(FLAGS_STAMP) | $(BUILD)
	kos-cc $(KOS_CFLAGS) $(CFLAGS_LP) -c $< -o $@

# pl_mpeg's own warnings are not ours to fix.
$(BUILD)/mpeg.o: src/mpeg.c src/mpeg.h third_party/pl_mpeg/pl_mpeg.h $(FLAGS_STAMP) | $(BUILD)
	kos-cc $(KOS_CFLAGS) $(CFLAGS_LP) -w -c $< -o $@

$(BUILD):
	mkdir -p $@

$(ELF): $(OBJS) $(KOS_BASE)/lib/$(KOS_ARCH)/libkallisti.a
	kos-cc -o $@ $(OBJS) -lm

clean:
	-rm -rf build lemonplay*.elf

run: $(ELF)
	./run-flycast.sh --skip-build

# Real Dreamcast through tools/dc-hw.sh (dcload-ip hands the adapter over).
DC_HW = ../../../tools/dc-hw.sh
hw: $(TARGET)
	$(DC_HW) run -c $(TARGET)

.PHONY: all clean run hw FORCE
