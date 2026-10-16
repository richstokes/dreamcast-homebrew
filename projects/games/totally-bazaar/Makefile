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
HEADERS = $(wildcard src/*.h) assets/generated/texture_assets.h \
	assets/generated/sound_assets.h
WARN = -O2 -Wall -Wextra -Wno-unused-parameter
BLENDER ?= blender

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

build/release/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) -Isrc -Iassets/generated -c $< -o $@

build/selftest/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) -DBAZAAR_SELFTEST -Isrc -Iassets/generated -c $< -o $@

build/autoplay/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) -DBAZAAR_AUTOPLAY=$(AUTOPLAY_PLAYERS) -DBAZAAR_AUTOPLAY_GAMES=$(AUTOPLAY_GAMES) -Isrc -Iassets/generated -c $< -o $@

build/uitest/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(WARN) -DBAZAAR_UITEST $(UITEST_FLAGS) -Isrc -Iassets/generated -c $< -o $@

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
	-rm -rf build $(TARGET) $(SELFTEST_TARGET) totally-bazaar-autoplay.elf totally-bazaar-uitest.elf

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

# CPU-only games at full speed; stops after AUTOPLAY_GAMES complete games.
autoplay: totally-bazaar-autoplay.elf
	BAZAAR_ELF=totally-bazaar-autoplay.elf BAZAAR_EXIT_ON_LOG='BAZAAR AUTOPLAY: ALL GAMES COMPLETE' \
		BAZAAR_FAIL_ON_LOG='KOS panic' ./run-flycast.sh --skip-build

# Scripted hot-seat session that drives the human controls end to end.
uitest: totally-bazaar-uitest.elf
	BAZAAR_ELF=totally-bazaar-uitest.elf BAZAAR_EXIT_ON_LOG='BAZAAR UITEST: ALL PASS' \
		BAZAAR_FAIL_ON_LOG='BAZAAR UITEST: FAIL' ./run-flycast.sh --skip-build

.PHONY: all clean run selftest autoplay uitest art textures audio
