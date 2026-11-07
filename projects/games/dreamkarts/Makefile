ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = dreamkarts.elf
GAME_PREFIX = DK
# input.c, render.c, mesh.c, text.c, ui.c, sfx.c, prof.c, frame.c, qa.c
# and sky.c are the shared engine's (projects/common/engine); the rest are
# src/. The list's
# order is the link order, which sets the code and data layout the
# console's timings were measured on, so keep it.
SOURCES = main.c input.c track.c kart.c render.c world.c models.c mesh.c fx.c hud.c text.c theme.c ui.c title.c audio.c sfx.c rumble.c dolphins.c biplane.c \
	net.c netlink.c online.c save.c vmuscreen.c spectate.c film.c items.c gp.c chat.c perfhud.c flappy.c roam.c \
	world_mesh.c world_road.c world_props.c world_beach.c world_coral.c world_harbour.c world_temple.c world_skyway.c \
	world_canyon.c world_tsudoken.c world_apple.c world_lot.c world_sf.c tide.c cablecar.c birds.c skylife.c autotest.c prof.c frame.c qa.c sky.c track_tex.c
# Shared with other projects, from projects/common.
COMMON_SOURCES = dialup.c
GEN = assets/generated
BLOBS = $(GEN)/textures.pak $(GEN)/sfx.pak
LIBS = -lppp -lm
EXTRA_INCLUDES = -I../..
VERSION_SOURCES = title.c main.c
BLENDER ?= blender

# Build variants (projects/common/engine/game.mk).
# autotest: CPU drives the player kart for a full race and logs frame times.
VARIANT ?= release
VARIANT_FLAGS_release =
VARIANT_FLAGS_autotest = -DDK_AUTOTEST
VARIANT_FLAGS_capture = -DDK_AUTOTEST -DDK_CAPTURE
VARIANT_FLAGS_hwbench = -DDK_AUTOTEST -DDK_CAPTURE -DDK_HW
# film: trailer footage (src/film.c), with -DDK_FILM_SPECTATE or -DDK_FILM_SHOW.
VARIANT_FLAGS_film = -DDK_FILM
# Under dcload there is normally no disc in the drive, so the game leaves
# the GD-ROM alone and plays no music; MUSIC=1 (make hw MUSIC=1) plays the
# CD-DA tracks of whatever DreamKarts disc is in the tray.
ifeq ($(MUSIC),1)
override EXTRA_CFLAGS += -DDK_DCLOAD_MUSIC
endif

# The game is a disc: its music is CD-DA tracks (cdda/*.wav, fetched and cut
# by make music), which run-flycast.sh and the release packaging press onto
# the image with the ELF. A plain make builds just the ELF, so a toolchain
# container without ffmpeg or the network can still compile it.
include ../../common/engine/game.mk

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

# Apple Valley's props: Blender models every one (race control, the
# bleachers, pit canopies, containers, clubhouse, trailer, starter's stand,
# tyre stacks, light pole, fence, boulders, Joshua trees, creosote and the
# brown mountain) and bakes them as strips; captures/apple_valley_props.blend
# to look at, captures/apple-valley-props/*.png for a render of each.
apple-valley-props:
	mkdir -p captures
	$(BLENDER) --background --python tools/build_apple_valley_props.py -- --header $(GEN)/apple_valley_props.h \
		--blend captures/apple_valley_props.blend --preview captures/apple-valley-props

# Bay City Hills' props: Blender models every one (the Golden Gate Bridge
# and its headlands, Alcatraz, Coit Tower, the pyramid, a cable car, a bay
# ferry, street lamps, date palms, a Monterey cypress, a fishing boat, the
# sea lions' raft and the wharf's sign arch) and bakes them as strips;
# captures/sf_props.blend to look at, captures/sf-props/*.png for a render
# of each.
sf-props:
	mkdir -p captures
	$(BLENDER) --background --python tools/build_sf_props.py -- --header $(GEN)/sf_props.h \
		--blend captures/sf_props.blend --preview captures/sf-props

# Spiral Canyon's final-lap biplane: Blender models it and bakes the body
# and propeller meshes; captures/biplane.blend and captures/biplane/*.png
# to look at.
biplane:
	mkdir -p captures
	$(BLENDER) --background --python tools/build_biplane.py -- --header $(GEN)/biplane_mesh.h \
		--blend captures/biplane.blend --preview captures/biplane

audio:
	uv run tools/build_audio.py --output-dir $(GEN)

# The music: the mp3s in assets/music/tracks.txt downloaded (once) from
# dcvmu.com and cut to CD-DA WAVs in cdda/, plus music_tracks.h.
music:
	uv run tools/build_music.py

vmu:
	uv run tools/build_vmu.py --output-dir $(GEN)

assets: textures models audio music vmu canyon-rock tsudoken-props apple-valley-props sf-props biplane

run: $(TARGET) music
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

# A self-booting CDI, as the release publishes it (tools/mkdcdisc).
disc: $(TARGET) music
	../../../tools/flycast/elf-to-cdi.sh --cdda cdda $(TARGET) dreamkarts.cdi

# Play on the console (power-cycles it first); dcload maps this directory
# at /pc. No music unless MUSIC=1 and a DreamKarts disc is in the drive.
hw: $(TARGET)
	$(DC_HW) run -c -T 86400 dreamkarts.elf -- -m $(CURDIR)

.PHONY: canyon-rock tsudoken-props apple-valley-props sf-props biplane run textures models audio music vmu assets autotest hwbench hw disc

endif
