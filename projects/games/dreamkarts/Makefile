ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = dreamkarts.elf
SOURCES = main.c input.c track.c kart.c render.c world.c models.c fx.c hud.c title.c audio.c
GEN = assets/generated
BLOBS = $(GEN)/textures.pak $(GEN)/sfx.pak $(GEN)/music.adpcm
GEN_HEADERS = $(wildcard $(GEN)/*.h)
SH4ZAM_HEADERS = $(wildcard third_party/sh4zam/include/sh4zam/*.h \
	third_party/sh4zam/include/sh4zam/inline/*.h \
	third_party/sh4zam/include/sh4zam/inline/sh4/*.h)
HEADERS = $(wildcard src/*.h) $(GEN_HEADERS) $(SH4ZAM_HEADERS)
WARN = -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(EXTRA_CFLAGS)
INCLUDES = -Isrc -I$(GEN) -Ithird_party/sh4zam/include
BLENDER ?= blender

# Build variants share sources; each gets its own object directory.
# autotest: CPU drives the player kart for a full race and logs frame times.
VARIANT ?= release
VARIANT_FLAGS_release =
VARIANT_FLAGS_autotest = -DDK_AUTOTEST
VARIANT_FLAGS_capture = -DDK_AUTOTEST -DDK_CAPTURE
OBJDIR = build/$(VARIANT)
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
	kos-cc -o $@ $(OBJS) -lm

dreamkarts-$(VARIANT).elf: $(OBJS)
	kos-cc -o $@ $(OBJS) -lm

clean:
	-rm -rf build dreamkarts.elf dreamkarts-*.elf

# Asset generators (outputs are committed so a plain build needs no Python).
textures:
	uv run tools/build_textures.py --output-dir $(GEN)

models:
	uv run tools/build_models.py --out $(GEN)/models.h

audio:
	uv run tools/build_audio.py --output-dir $(GEN)

assets: textures models audio

run: $(TARGET)
	./run-flycast.sh --skip-build

# Full scripted race with the CPU driving the player; fails on a panic and
# passes when the autotest prints its summary.
autotest:
	$(MAKE) VARIANT=autotest dreamkarts-autotest.elf
	DK_ELF=dreamkarts-autotest.elf DK_EXIT_ON_LOG='DK AUTOTEST: PASS' \
		DK_FAIL_ON_LOG='DK AUTOTEST: FAIL' ./run-flycast.sh --skip-build

.PHONY: all clean run textures models audio assets autotest

endif
