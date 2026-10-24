ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = dreamkarts.elf
SOURCES = main.c input.c track.c kart.c render.c world.c models.c fx.c hud.c title.c audio.c rumble.c dolphins.c \
	net.c netlink.c online.c save.c vmuscreen.c spectate.c film.c items.c gp.c chat.c perfhud.c
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
INCLUDES = -Isrc -I$(GEN) -Ibuild -Ithird_party/sh4zam/include
BLENDER ?= blender

# Build variants share sources; each gets its own object directory.
# autotest: CPU drives the player kart for a full race and logs frame times.
VARIANT ?= release
VARIANT_FLAGS_release =
VARIANT_FLAGS_autotest = -DDK_AUTOTEST
VARIANT_FLAGS_capture = -DDK_AUTOTEST -DDK_CAPTURE
VARIANT_FLAGS_hwbench = -DDK_AUTOTEST -DDK_CAPTURE -DDK_HW
# film: trailer footage (src/film.c), with -DDK_FILM_SPECTATE or -DDK_FILM_SHOW.
VARIANT_FLAGS_film = -DDK_FILM
# Extra -D flags get their own object directory so builds never mix.
empty :=
space := $(empty) $(empty)
FLAGTAG = $(subst =,-,$(subst $(space),,$(subst -D,+,$(EXTRA_CFLAGS))))
OBJDIR = build/$(VARIANT)$(FLAGTAG)
ASM_SOURCES = xform.S
OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(SOURCES)) $(patsubst %.S,$(OBJDIR)/%.o,$(ASM_SOURCES)) $(OBJDIR)/blobs.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(OBJDIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) $(VARIANT_FLAGS_$(VARIANT)) $(INCLUDES) -c $< -o $@

$(OBJDIR)/xform.o: src/xform.S
	@mkdir -p $(@D)
	kos-cc -c $< -o $@

$(OBJDIR)/blobs.o: src/blobs.S $(BLOBS)
	@mkdir -p $(@D)
	kos-cc -c $< -o $@

# The music playlist alone is 9 MB, and with debug info the ELF passed 16 MB,
# which Flycast will not boot. Keep the debug info beside it (for
# sh-elf-addr2line on panic addresses) and strip it from the ELF.
define link_elf
	kos-cc -o $@ $(OBJS) -lppp -lm
	$(KOS_OBJCOPY) --only-keep-debug $@ $@.debug
	$(KOS_STRIP) -g $@
	$(KOS_OBJCOPY) --add-gnu-debuglink=$@.debug $@
endef

# A build with other EXTRA_CFLAGS has its own objects, but the ELF it links
# is named the same, so switching back would leave the newer ELF in place:
# the stamp changes with the flags and forces the link.
LINK_STAMP = build/$(VARIANT).flags
$(LINK_STAMP): force
	@mkdir -p build
	@echo '$(FLAGTAG)' | cmp -s - $@ || echo '$(FLAGTAG)' > $@

# The commit the game was built from, shown on the title screen so bug
# reports can name it. Short SHA plus "-dirty" when this directory has
# uncommitted changes; DK_GIT_SHA=... overrides it (CI, tarballs). Only
# rewritten when it changes so a new commit relinks title.o and main.o
# rather than everything.
DK_GIT_SHA ?= $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD -- . 2>/dev/null || echo -dirty)
VERSION_H = build/version.h
$(VERSION_H): force
	@mkdir -p build
	@echo '#define DK_GIT_SHA "$(DK_GIT_SHA)"' | cmp -s - $@ || echo '#define DK_GIT_SHA "$(DK_GIT_SHA)"' > $@
$(OBJDIR)/title.o $(OBJDIR)/main.o: $(VERSION_H)

dreamkarts.elf: $(OBJS) $(LINK_STAMP)
	$(link_elf)

dreamkarts-$(VARIANT).elf: $(OBJS) $(LINK_STAMP)
	$(link_elf)

clean:
	-rm -rf build dreamkarts.elf dreamkarts-*.elf dreamkarts*.elf.debug

# Asset generators (outputs are committed so a plain build needs no Python).
textures:
	uv run tools/build_textures.py --output-dir $(GEN)

models:
	uv run tools/build_models.py --out $(GEN)/models.h

# Spiral Canyon's rock: a -DDK_DUMP_ROCK build prints the rock's height
# field, tunnel bores and road clearance, and Blender cuts one mesh from
# them (captures/canyon_rock.blend to look at, the header to draw).
canyon-rock:
	$(MAKE) VARIANT=autotest EXTRA_CFLAGS="-DDK_AT_TRACK=6 -DDK_DUMP_ROCK" dreamkarts-autotest.elf
	mkdir -p captures
	DK_ELF=dreamkarts-autotest.elf DK_EXIT_ON_LOG='DKR END' DK_FAIL_ON_LOG='DK AUTOTEST: FAIL' \
		./run-flycast.sh --skip-build > captures/canyon-rock-dump.log 2>&1
	$(BLENDER) --background --python tools/build_canyon_rock.py -- --dump captures/canyon-rock-dump.log \
		--header $(GEN)/canyon_rock.h --blend captures/canyon_rock.blend

# Tsudoken Drift's props: Blender models every one (Mt Fuji, the cherry
# trees, torii, lanterns, the pagoda, grandstand, tower, pits, teahouse and
# bridge) and bakes them as strips; captures/tsudoken_props.blend to look
# at, captures/tsudoken-props/*.png for a render of each.
tsudoken-props:
	mkdir -p captures
	$(BLENDER) --background --python tools/build_tsudoken_props.py -- --header $(GEN)/tsudoken_props.h \
		--blend captures/tsudoken_props.blend --preview captures/tsudoken-props

audio:
	uv run tools/build_audio.py --output-dir $(GEN)

vmu:
	uv run tools/build_vmu.py --output-dir $(GEN)

assets: textures models audio vmu canyon-rock tsudoken-props

run: $(TARGET)
	./run-flycast.sh --skip-build

# Full scripted race with the CPU driving the player; fails on a panic and
# passes when the autotest prints its summary.
autotest:
	$(MAKE) VARIANT=autotest dreamkarts-autotest.elf
	DK_ELF=dreamkarts-autotest.elf DK_EXIT_ON_LOG='DK AUTOTEST: PASS' \
		DK_FAIL_ON_LOG='DK AUTOTEST: FAIL' ./run-flycast.sh --skip-build

# Every online scenario (race, custom kart, practice, spectate, old
# protocol, server restart) against a local lobby server, each in its own
# Flycast with the bots it needs; DK_QA_LIVE=1 runs the first four against
# dcvmu.com. QA="race restart" picks scenarios.
online-qa:
	tools/online-qa.sh $(QA)

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

.PHONY: canyon-rock tsudoken-props all clean run textures models audio vmu assets autotest hwbench hw force

endif
