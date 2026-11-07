ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = ridgeline.elf
GAME_PREFIX = RL
# Ridgeline's own sources (src/), then the shared engine's
# (projects/common/engine), in link order.
SOURCES = main.c terrain.c world.c truck.c truckdraw.c camera.c hud.c audio.c ai.c fx.c menu.c demo.c soundtest.c autotest.c \
	input.c rand.c render.c prof.c frame.c mesh.c scenery.c sfx.c text.c ui.c
GEN = assets/generated
BLOBS = $(GEN)/textures.pak $(GEN)/sfx.pak $(GEN)/level.bin
VERSION_SOURCES = main.c menu.c

# Build variants (projects/common/engine/game.mk).
# autotest: the CPU drives the player up the mountain and logs frame times.
# capture: autotest plus scripted screenshots (tools/capture.py).
# hwbench: capture timings on the console (tools/dc-hw.sh), quiet log.
VARIANT ?= release
VARIANT_FLAGS_release =
VARIANT_FLAGS_autotest = -DRL_AUTOTEST
VARIANT_FLAGS_capture = -DRL_AUTOTEST -DRL_CAPTURE
VARIANT_FLAGS_hwbench = -DRL_AUTOTEST -DRL_CAPTURE -DRL_HW

include ../../common/engine/game.mk

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

.PHONY: run level textures trucks audio assets autotest hwbench fxbench disc hw

endif
