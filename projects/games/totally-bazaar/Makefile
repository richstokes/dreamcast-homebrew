ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = totally-bazaar.elf
SELFTEST_TARGET = totally-bazaar-selftest.elf
SOURCES = main.c rules.c ai.c gfx.c audio.c selftest.c
GENERATED = assets/generated/texture_assets.c assets/generated/sound_assets.c
ASSET_OBJS = build/release/texture_assets.o build/release/sound_assets.o \
	build/release/blobs.o
OBJS = $(patsubst %.c,build/release/%.o,$(SOURCES)) $(ASSET_OBJS)
SELFTEST_OBJS = $(patsubst %.c,build/selftest/%.o,$(SOURCES)) $(ASSET_OBJS)
AUTOPLAY_OBJS = $(patsubst %.c,build/autoplay/%.o,$(SOURCES)) $(ASSET_OBJS)
UITEST_OBJS = $(patsubst %.c,build/uitest/%.o,$(SOURCES)) $(ASSET_OBJS)
AUTOPLAY_PLAYERS ?= 4
AUTOPLAY_GAMES ?= 2
BLOBS = assets/generated/textures.pak assets/generated/sfx.pak \
	assets/generated/bazaar_music.adpcm
SH4ZAM_HEADERS = $(wildcard third_party/sh4zam/include/sh4zam/*.h \
	third_party/sh4zam/include/sh4zam/inline/*.h \
	third_party/sh4zam/include/sh4zam/inline/sh4/*.h)
HEADERS = $(wildcard src/*.h) assets/generated/texture_assets.h \
	assets/generated/sound_assets.h $(SH4ZAM_HEADERS)
WARN = -O2 -Wall -Wextra -Wno-unused-parameter $(EXTRA_CFLAGS)
BLENDER ?= blender

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

build/release/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) -Isrc -Iassets/generated -Ithird_party/sh4zam/include -c $< -o $@

build/selftest/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) -DBAZAAR_SELFTEST -Isrc -Iassets/generated -Ithird_party/sh4zam/include -c $< -o $@

build/autoplay/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) -DBAZAAR_AUTOPLAY=$(AUTOPLAY_PLAYERS) -DBAZAAR_AUTOPLAY_GAMES=$(AUTOPLAY_GAMES) -Isrc -Iassets/generated -Ithird_party/sh4zam/include -c $< -o $@

build/uitest/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) -DBAZAAR_UITEST $(UITEST_FLAGS) -Isrc -Iassets/generated -Ithird_party/sh4zam/include -c $< -o $@

build/release/texture_assets.o: assets/generated/texture_assets.c assets/generated/texture_assets.h
	@mkdir -p $(@D)
	kos-cc -O2 -Iassets/generated -c $< -o $@

build/release/sound_assets.o: assets/generated/sound_assets.c assets/generated/sound_assets.h
	@mkdir -p $(@D)
	kos-cc -O2 -Iassets/generated -c $< -o $@

build/release/blobs.o: src/blobs.S $(BLOBS)
	@mkdir -p $(@D)
	kos-cc -c $< -o $@

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lm

$(SELFTEST_TARGET): $(SELFTEST_OBJS)
	kos-cc -o $@ $(SELFTEST_OBJS) -lm

totally-bazaar-autoplay.elf: $(AUTOPLAY_OBJS)
	kos-cc -o $@ $(AUTOPLAY_OBJS) -lm

totally-bazaar-uitest.elf: $(UITEST_OBJS)
	kos-cc -o $@ $(UITEST_OBJS) -lm

clean:
	-rm -rf build $(TARGET) $(SELFTEST_TARGET) totally-bazaar-autoplay.elf totally-bazaar-uitest.elf totally-bazaar-diag.elf \
		totally-bazaar-hwbench.elf totally-bazaar-hwsoak.elf

# Regenerate art (Blender renders, then Pillow composition) and audio.
art:
	$(BLENDER) --background --factory-startup --python tools/render_art_blender.py -- --output-dir assets/source/renders
	uv run tools/build_textures.py --source-dir assets/source --output-dir assets/generated

textures:
	uv run tools/build_textures.py --source-dir assets/source --output-dir assets/generated

audio:
	uv run tools/build_audio.py --output-dir assets/generated

run: $(TARGET)
	./run-flycast.sh --skip-build

selftest: $(SELFTEST_TARGET)
	BAZAAR_ELF=$(SELFTEST_TARGET) BAZAAR_EXIT_ON_LOG='BAZAAR SELFTEST: ALL TESTS PASS' \
		BAZAAR_FAIL_ON_LOG='BAZAAR SELFTEST: FAILED' ./run-flycast.sh --skip-build

# Hardware diagnostics: a 10-second CPU/render heartbeat on the serial log.
diag:
	rm -rf build/release $(TARGET)
	$(MAKE) EXTRA_CFLAGS=-DBAZAAR_DIAG $(TARGET)
	mv $(TARGET) totally-bazaar-diag.elf
	rm -rf build/release

# Real hardware through tools/dc-hw.sh (Shelly plug power cycle + dcload-ip).
# Builds for hardware never log per move: under dcload-ip each printf is a
# network call made with interrupts off, and a lost packet freezes the console.
DC_HW = ../../../tools/dc-hw.sh
HW_LOG ?= hw-console.log
HW_SECONDS ?= 150
SOAK_SECONDS ?= 1800

define hw_variant
	rm -rf build/release $(TARGET)
	$(MAKE) EXTRA_CFLAGS="$(2)" $(TARGET)
	mv $(TARGET) $(1)
	rm -rf build/release
endef

# Play the release build on the console (power-cycles it first).
hw: $(TARGET)
	$(DC_HW) run -c -T 86400 -l $(HW_LOG) $(TARGET)

# Four CPU merchants with the 10-second CPU/render heartbeat.
hwbench:
	$(call hw_variant,totally-bazaar-hwbench.elf,-DBAZAAR_AUTOPLAY=4 -DBAZAAR_AUTOPLAY_GAMES=50 -DBAZAAR_DIAG -DBAZAAR_HW)
	$(DC_HW) run -c -T $(HW_SECONDS) -u 'WATCHDOG|STALL' -l $(HW_LOG) \
		totally-bazaar-hwbench.elf || [ $$? -eq 124 ]
	@! /usr/bin/grep -aE 'WATCHDOG|STALL' $(HW_LOG)
	@/usr/bin/grep -a 'BAZAAR HB' $(HW_LOG) | tail -n 8

# Long two-player CPU session to catch hangs; fails on a watchdog report.
hwsoak:
	$(call hw_variant,totally-bazaar-hwsoak.elf,-DBAZAAR_AUTOPLAY=2 -DBAZAAR_AUTOPLAY_GAMES=1000 -DBAZAAR_DIAG -DBAZAAR_HW -DBAZAAR_HB_SECONDS=60.0f)
	$(DC_HW) run -c -T $(SOAK_SECONDS) -u 'WATCHDOG|STALL' -l $(HW_LOG) \
		totally-bazaar-hwsoak.elf || [ $$? -eq 124 ]
	@! /usr/bin/grep -aE 'WATCHDOG|STALL' $(HW_LOG)
	@echo "Soak passed: $$(/usr/bin/grep -ac 'scored' $(HW_LOG)) player-round scores, no stalls."

# CPU-only games at full speed; stops after AUTOPLAY_GAMES complete games.
autoplay: totally-bazaar-autoplay.elf
	BAZAAR_ELF=totally-bazaar-autoplay.elf BAZAAR_EXIT_ON_LOG='BAZAAR AUTOPLAY: ALL GAMES COMPLETE' \
		BAZAAR_FAIL_ON_LOG='KOS panic' ./run-flycast.sh --skip-build

# Scripted hot-seat session that drives the human controls end to end.
uitest: totally-bazaar-uitest.elf
	BAZAAR_ELF=totally-bazaar-uitest.elf BAZAAR_EXIT_ON_LOG='BAZAAR UITEST: ALL PASS' \
		BAZAAR_FAIL_ON_LOG='BAZAAR UITEST: FAIL' ./run-flycast.sh --skip-build

.PHONY: all clean run selftest autoplay uitest diag hw hwbench hwsoak art textures audio

endif
