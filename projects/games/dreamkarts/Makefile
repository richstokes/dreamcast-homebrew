ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = dreamkarts.elf
SOURCES = main.c input.c track.c kart.c render.c world.c models.c fx.c hud.c title.c audio.c rumble.c dolphins.c \
	net.c netlink.c online.c save.c vmuscreen.c spectate.c
GEN = assets/generated
BLOBS = $(GEN)/textures.pak $(GEN)/sfx.pak $(GEN)/music.adpcm
GEN_HEADERS = $(wildcard $(GEN)/*.h)
SH4ZAM_HEADERS = $(wildcard third_party/sh4zam/include/sh4zam/*.h \
	third_party/sh4zam/include/sh4zam/inline/*.h \
	third_party/sh4zam/include/sh4zam/inline/sh4/*.h)
HEADERS = $(wildcard src/*.h) $(GEN_HEADERS) $(SH4ZAM_HEADERS)
# -ffast-math lets GCC use the SH-4's FSCA for sinf/cosf (KOS passes -mfsca
# but libm calls remain without it) and plain FSQRT for sqrtf.
WARN = -std=gnu17 -O2 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(EXTRA_CFLAGS)
INCLUDES = -Isrc -I$(GEN) -Ithird_party/sh4zam/include
BLENDER ?= blender

# Build variants share sources; each gets its own object directory.
# autotest: CPU drives the player kart for a full race and logs frame times.
VARIANT ?= release
VARIANT_FLAGS_release =
VARIANT_FLAGS_autotest = -DDK_AUTOTEST
VARIANT_FLAGS_capture = -DDK_AUTOTEST -DDK_CAPTURE
VARIANT_FLAGS_hwbench = -DDK_AUTOTEST -DDK_CAPTURE -DDK_HW
# Extra -D flags get their own object directory so builds never mix.
empty :=
space := $(empty) $(empty)
FLAGTAG = $(subst =,-,$(subst $(space),,$(subst -D,+,$(EXTRA_CFLAGS))))
OBJDIR = build/$(VARIANT)$(FLAGTAG)
OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(SOURCES)) $(OBJDIR)/blobs.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(OBJDIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) $(VARIANT_FLAGS_$(VARIANT)) $(INCLUDES) -c $< -o $@

$(OBJDIR)/blobs.o: src/blobs.S $(BLOBS)
	@mkdir -p $(@D)
	kos-cc -c $< -o $@

dreamkarts.elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lppp -lm

dreamkarts-$(VARIANT).elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lppp -lm

clean:
	-rm -rf build dreamkarts.elf dreamkarts-*.elf

# Asset generators (outputs are committed so a plain build needs no Python).
textures:
	uv run tools/build_textures.py --output-dir $(GEN)

models:
	uv run tools/build_models.py --out $(GEN)/models.h

audio:
	uv run tools/build_audio.py --output-dir $(GEN)

vmu:
	uv run tools/build_vmu.py --output-dir $(GEN)

assets: textures models audio vmu

run: $(TARGET)
	./run-flycast.sh --skip-build

# Full scripted race with the CPU driving the player; fails on a panic and
# passes when the autotest prints its summary.
autotest:
	$(MAKE) VARIANT=autotest dreamkarts-autotest.elf
	DK_ELF=dreamkarts-autotest.elf DK_EXIT_ON_LOG='DK AUTOTEST: PASS' \
		DK_FAIL_ON_LOG='DK AUTOTEST: FAIL' ./run-flycast.sh --skip-build

# Real Dreamcast through tools/dc-hw.sh (Shelly plug power cycle +
# dcload-ip): the scripted title/menu/race benchmark with 10 s timing lines,
# screenshots written to captures/hw/ over dcload's /pc/ share (as PNG).
DC_HW = ../../../tools/dc-hw.sh
HW_LOG ?= captures/hw/hwbench.log

hwbench:
	$(MAKE) VARIANT=hwbench dreamkarts-hwbench.elf
	mkdir -p captures/hw
	$(DC_HW) run -c -o -T 420 -i 90 -u 'DK AUTOTEST: (PASS|FAIL)' -l $(HW_LOG) \
		dreamkarts-hwbench.elf -- -m $(CURDIR)/captures/hw
	uv run --with pillow python -c "import glob,PIL.Image as I; [I.open(f).save(f[:-3]+'png') for f in glob.glob('captures/hw/*.ppm')]"
	@/usr/bin/grep -a 'DK AUTOTEST' $(HW_LOG)

# Play the release build on the console (power-cycles it first).
hw: $(TARGET)
	$(DC_HW) run -c -T 86400 dreamkarts.elf

.PHONY: all clean run textures models audio vmu assets autotest hwbench hw

endif
