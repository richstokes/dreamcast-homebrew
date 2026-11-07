ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = ridgeline.elf
# Ridgeline's own sources (src/) and the shared engine's
# (projects/common/engine).
SOURCES = main.c terrain.c world.c truck.c truckdraw.c camera.c hud.c audio.c ai.c fx.c menu.c demo.c soundtest.c autotest.c
ENGINE_SOURCES = input.c rand.c render.c prof.c mesh.c scenery.c sfx.c text.c
ENGINE_DIR = ../../common/engine
GEN = assets/generated
BLOBS = $(GEN)/textures.pak $(GEN)/sfx.pak $(GEN)/level.bin
SH4ZAM_HEADERS = $(wildcard third_party/sh4zam/include/sh4zam/*.h \
	third_party/sh4zam/include/sh4zam/inline/*.h \
	third_party/sh4zam/include/sh4zam/inline/sh4/*.h)
HEADERS = $(wildcard src/*.h ../../common/*.h $(ENGINE_DIR)/*.h $(GEN)/*.h) $(SH4ZAM_HEADERS)
# -ffast-math lets GCC use the SH-4's FSCA for sinf/cosf and plain FSQRT.
WARN = -std=gnu17 -O2 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(EXTRA_CFLAGS)
INCLUDES = -Isrc -I$(GEN) -Ibuild -I../../common -Ithird_party/sh4zam/include

# Variants share sources; each gets its own object directory.
# autotest: the CPU drives the player up the mountain and logs frame times.
# capture: autotest plus scripted screenshots (tools/capture.py).
# hwbench: capture timings on the console (tools/dc-hw.sh), quiet log.
VARIANT ?= release
VARIANT_FLAGS_release =
VARIANT_FLAGS_autotest = -DRL_AUTOTEST
VARIANT_FLAGS_capture = -DRL_AUTOTEST -DRL_CAPTURE
VARIANT_FLAGS_hwbench = -DRL_AUTOTEST -DRL_CAPTURE -DRL_HW
empty :=
space := $(empty) $(empty)
FLAGTAG = $(subst =,-,$(subst $(space),,$(subst -D,+,$(EXTRA_CFLAGS))))
OBJDIR = build/$(VARIANT)$(FLAGTAG)
OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(SOURCES)) $(patsubst %.c,$(OBJDIR)/eng_%.o,$(ENGINE_SOURCES)) \
	$(OBJDIR)/xform.o $(OBJDIR)/blobs.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(OBJDIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) $(VARIANT_FLAGS_$(VARIANT)) $(INCLUDES) -c $< -o $@

$(OBJDIR)/eng_%.o: $(ENGINE_DIR)/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) $(VARIANT_FLAGS_$(VARIANT)) $(INCLUDES) -c $< -o $@

$(OBJDIR)/xform.o: $(ENGINE_DIR)/xform.S
	@mkdir -p $(@D)
	kos-cc -c $< -o $@

$(OBJDIR)/blobs.o: src/blobs.S $(BLOBS)
	@mkdir -p $(@D)
	kos-cc -c $< -o $@

define link_elf
	kos-cc -o $@ $(OBJS) -lm
	$(KOS_OBJCOPY) --only-keep-debug $@ $@.debug
	$(KOS_STRIP) -g $@
	$(KOS_OBJCOPY) --add-gnu-debuglink=$@.debug $@
endef

LINK_STAMP = build/$(VARIANT).flags
$(LINK_STAMP): force
	@mkdir -p build
	@echo '$(FLAGTAG)' | cmp -s - $@ || { sleep 1; echo '$(FLAGTAG)' > $@; }

RL_GIT_SHA ?= $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD -- . 2>/dev/null || echo -dirty)
VERSION_H = build/version.h
$(VERSION_H): force
	@mkdir -p build
	@echo '#define RL_GIT_SHA "$(RL_GIT_SHA)"' | cmp -s - $@ || echo '#define RL_GIT_SHA "$(RL_GIT_SHA)"' > $@
$(OBJDIR)/main.o $(OBJDIR)/menu.o: $(VERSION_H)

ridgeline.elf: $(OBJS) $(LINK_STAMP)
	$(link_elf)

ridgeline-$(VARIANT).elf: $(OBJS) $(LINK_STAMP)
	$(link_elf)

clean:
	-rm -rf build ridgeline.elf ridgeline-*.elf ridgeline*.elf.debug ridgeline.cdi

# Asset generators (outputs are committed so a plain build needs no Python).
level:
	uv run tools/build_level.py --out $(GEN) --preview captures/level-preview.png

textures: level
	uv run tools/build_textures.py --output-dir $(GEN)

trucks:
	uv run tools/build_trucks.py

audio:
	uv run tools/build_audio.py

assets: level textures trucks audio

run: $(TARGET)
	./run-flycast.sh --skip-build

# The CPU drives the player from the meadow to the summit, logging frame
# times; passes when it prints its summary.
autotest:
	$(MAKE) VARIANT=autotest ridgeline-autotest.elf
	RL_ELF=ridgeline-autotest.elf RL_EXIT_ON_LOG='RL AUTOTEST: PASS' \
		RL_FAIL_ON_LOG='RL AUTOTEST: FAIL' ./run-flycast.sh --skip-build

DC_HW = ../../../tools/dc-hw.sh
HW_LOG ?= captures/hw/hwbench.log
hwbench:
	$(MAKE) VARIANT=hwbench ridgeline-hwbench.elf
	mkdir -p captures/hw
	$(DC_HW) run -c -o -T 420 -i 90 -u 'RL AUTOTEST: (PASS|FAIL)' -l $(HW_LOG) \
		ridgeline-hwbench.elf -- -m $(CURDIR)/captures/hw
	@/usr/bin/grep -a 'RL A' $(HW_LOG)

# The tyre effects on the console: 15 s of driving, then 25 s of wheelspin
# in the meadow's mud, frame and PVR render times every 5 s.
fxbench:
	$(MAKE) VARIANT=hwbench EXTRA_CFLAGS=-DRL_AT_FXBENCH ridgeline-hwbench.elf
	mkdir -p captures/hw
	$(DC_HW) run -c -o -T 240 -i 90 -u 'RL AUTOTEST: (PASS|FAIL)' -l captures/hw/fxbench.log \
		ridgeline-hwbench.elf -- -m $(CURDIR)/captures/hw
	@/usr/bin/grep -a 'RL A' captures/hw/fxbench.log

disc: $(TARGET)
	../../../tools/flycast/elf-to-cdi.sh $(TARGET) ridgeline.cdi

hw: $(TARGET)
	$(DC_HW) run -c -T 86400 ridgeline.elf -- -m $(CURDIR)

.PHONY: all clean run level textures trucks audio assets autotest hwbench fxbench disc hw force

endif
