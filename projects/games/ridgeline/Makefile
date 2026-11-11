ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = ridgeline.elf
GAME_PREFIX = RL
# Ridgeline's own sources (src/), then the shared engine's
# (projects/common/engine), in link order.
SOURCES = main.c level.c terrain.c world.c outland.c lamps.c air.c desert.c dunes.c water.c tod.c truck.c truckdraw.c camera.c hud.c audio.c ai.c fx.c menu.c demo.c records.c deer.c soundtest.c autotest.c birds.c film.c vmuscreen.c campfire.c glow.c \
	input.c rand.c render.c prof.c frame.c mesh.c scenery.c sfx.c text.c ui.c qa.c sky.c net.c online.c
# Shared with DreamKarts from projects/common: the network link (Broadband
# adapter, W5500, modem), the dialler and the VMU screens.
COMMON_SOURCES = netlink.c dialup.c vmulcd.c
LIBS = -lppp -lm
GEN = assets/generated
BLOBS = $(GEN)/textures.pak $(GEN)/sfx.pak $(GEN)/level_cascade.lvl $(GEN)/level_moab.lvl $(GEN)/level_moab.out $(GEN)/level_dunes.lvl $(GEN)/level_dunes.out
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
# film: scripted trailer scenes with no HUD (src/film.c, tools/film/).
VARIANT_FLAGS_film = -DRL_FILM

include ../../common/engine/game.mk

# Asset generators (outputs are committed so a plain build needs no Python).
# AREA=cascade, moab or dunes builds one area.
AREA ?= all
level:
	uv run tools/build_level.py --out $(GEN) --area $(AREA) --preview captures

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

# The best-times save on eight blank Flycast cards (never the real ones).
vmutest:
	rm -f ridgeline-autotest.elf
	$(MAKE) VARIANT=autotest EXTRA_CFLAGS=-DRL_AT_VMUTEST ridgeline-autotest.elf
	rm -rf build/vmutest-cards
	python3 -c 'import sys, pathlib; sys.path.insert(0, "../../dcvmu.com/client/tests"); \
		import isolated_cards; isolated_cards.blank_cards(pathlib.Path("build/vmutest-cards"))'
	RL_ELF=ridgeline-autotest.elf RL_EXIT_ON_LOG='RL AUTOTEST: PASS' RL_FAIL_ON_LOG='RL AUTOTEST: FAIL' \
		RL_FLYCAST_CONFIG="config:Dreamcast.VMUPath=$(CURDIR)/build/vmutest-cards" ./run-flycast.sh --skip-build
	rm -f ridgeline-autotest.elf

# Online play in Flycast against a server and bot on this Mac (RL_NET=modem
# dials in instead, RL_QA_LIVE=1 uses dcvmu.com): tools/online-test.sh.
online-test:
	tools/online-test.sh

DC_HW = ../../../tools/dc-hw.sh
HW_LOG ?= captures/hw/hwbench.log
# HW_OFF= leaves the console on after the run, so a bench that went quiet
# can be looked into with make hwreport (then dc-hw.sh power off).
HW_OFF ?= -o
hwbench:
	$(MAKE) VARIANT=hwbench ridgeline-hwbench.elf
	mkdir -p captures/hw
	$(DC_HW) run -c $(HW_OFF) -T 420 -i 90 -u 'RL AUTOTEST: (PASS|FAIL)' -l $(HW_LOG) \
		ridgeline-hwbench.elf -- -m $(CURDIR)/captures/hw
	@/usr/bin/grep -a 'RL A' $(HW_LOG)

# What a console bench was doing when it went quiet: read from its memory.
hwreport:
	python3 tools/hwreport.py ridgeline-hwbench.elf

# The tyre effects on the console: 15 s of driving, then 25 s of wheelspin
# in the meadow's mud, frame and PVR render times every 5 s.
fxbench:
	$(MAKE) VARIANT=hwbench EXTRA_CFLAGS=-DRL_AT_FXBENCH ridgeline-hwbench.elf
	mkdir -p captures/hw
	$(DC_HW) run -c -o -T 240 -i 90 -u 'RL AUTOTEST: (PASS|FAIL)' -l captures/hw/fxbench.log \
		ridgeline-hwbench.elf -- -m $(CURDIR)/captures/hw
	@/usr/bin/grep -a 'RL A' captures/hw/fxbench.log

# Trailer footage through OBS (its websocket password in OBS_WS_PASSWORD):
# every scene in src/film.c, or FILM_PICK=3,7 for some. Cut on the
# "RL FILM go/cut" lines in captures/film/runs/<take>.log.
FILM_PICK ?=
TAKE ?= take
film:
	rm -f ridgeline-film.elf
	$(MAKE) VARIANT=film $(if $(FILM_PICK),EXTRA_CFLAGS=-DRL_FILM_PICK=$(FILM_PICK)) ridgeline-film.elf
	uv run ../dreamkarts/tools/film/record.py ridgeline-film.elf $(TAKE) --prefix 'RL FILM' \
		--flycast tools/film/flycast-film.sh --footage "$(HOME)/Movies/Ridgeline Footage" --work captures/film \
		--stop-on 'RL FILM done' --stop-after 1

disc: $(TARGET)
	../../../tools/flycast/elf-to-cdi.sh $(TARGET) ridgeline.cdi

hw: $(TARGET)
	$(DC_HW) run -c -T 86400 ridgeline.elf -- -m $(CURDIR)

.PHONY: run level textures trucks audio assets autotest vmutest online-test hwbench hwreport fxbench film disc hw

endif
