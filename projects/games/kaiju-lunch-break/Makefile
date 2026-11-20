ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = kaiju-lunch-break.elf
GAME_PREFIX = KL
# The game's sources (src/), then the shared engine's, in link order.
SOURCES = main.c city.c cel.c kaiju.c aim.c life.c fx.c score.c hud.c audio.c save.c autotest.c film.c \
	input.c render.c prof.c frame.c mesh.c scenery.c sfx.c text.c ui.c qa.c sky.c cardpick.c
LIBS = -lm
GEN = assets/generated
BLOBS = $(GEN)/textures.pak $(GEN)/sfx.pak
VERSION_SOURCES = main.c hud.c

# autotest: the CPU plays a lunch break and logs frame times.
# capture: autotest plus scripted screenshots (tools/capture.py).
VARIANT ?= release
VARIANT_FLAGS_release =
VARIANT_FLAGS_autotest = -DKL_AUTOTEST
VARIANT_FLAGS_capture = -DKL_AUTOTEST -DKL_CAPTURE
# film: scripted trailer scenes (src/film.c).
VARIANT_FLAGS_film = -DKL_FILM

# Under dcload there is normally no disc in the drive, so the game leaves
# the GD-ROM alone and plays no music; MUSIC=1 (make hw MUSIC=1) plays the
# CD-DA tracks of whatever Kaiju Lunch Break disc is in the tray.
ifeq ($(MUSIC),1)
override EXTRA_CFLAGS += -DKL_DCLOAD_MUSIC
endif

# The game is a disc: its music is CD-DA tracks (cdda/*.wav, fetched and
# cut by make music), which run-flycast.sh and make disc press onto the
# image with the ELF. A plain make builds just the ELF.
include ../../common/engine/game.mk

# Asset generators (outputs are committed so a plain build needs no Python).
textures:
	uv run tools/build_textures.py

models:
	uv run tools/build_models.py

audio:
	uv run tools/build_audio.py

# The music: the mp3 in assets/music/tracks.txt downloaded (once) and cut to
# a CD-DA WAV in cdda/, plus music_tracks.h.
music:
	python3 tools/build_music.py

assets: textures models audio music

run: $(TARGET) music
	./run-flycast.sh --skip-build

autotest:
	$(MAKE) VARIANT=autotest kaiju-lunch-break-autotest.elf
	KL_ELF=kaiju-lunch-break-autotest.elf KL_EXIT_ON_LOG='KL AUTOTEST: PASS' \
		KL_FAIL_ON_LOG='KL AUTOTEST: FAIL' ./run-flycast.sh --skip-build

# The save's round trip on blank Flycast cards (never the real ones).
vmutest:
	rm -f kaiju-lunch-break-autotest.elf
	$(MAKE) VARIANT=autotest EXTRA_CFLAGS=-DKL_AT_VMUTEST kaiju-lunch-break-autotest.elf
	rm -rf build/vmutest-cards
	python3 -c 'import sys, pathlib; sys.path.insert(0, "../../dcvmu.com/client/tests"); \
		import isolated_cards; isolated_cards.blank_cards(pathlib.Path("build/vmutest-cards"))'
	KL_ELF=kaiju-lunch-break-autotest.elf KL_EXIT_ON_LOG='KL AUTOTEST: PASS' KL_FAIL_ON_LOG='KL AUTOTEST: FAIL' \
		KL_FLYCAST_CONFIG="config:Dreamcast.VMUPath=$(CURDIR)/build/vmutest-cards" ./run-flycast.sh --skip-build
	rm -f kaiju-lunch-break-autotest.elf

capture:
	$(MAKE) VARIANT=capture kaiju-lunch-break-capture.elf
	uv run tools/capture.py --out captures

# Trailer footage through OBS (its websocket password in OBS_WS_PASSWORD):
# every scene in src/film.c, or FILM_PICK=3,7 for some. Cut on the
# "KL FILM go/cut" lines in captures/film/runs/<take>.log.
FILM_PICK ?=
TAKE ?= take
film:
	rm -f kaiju-lunch-break-film.elf
	$(MAKE) VARIANT=film $(if $(FILM_PICK),EXTRA_CFLAGS=-DKL_FILM_PICK=$(FILM_PICK)) kaiju-lunch-break-film.elf
	uv run ../dreamkarts/tools/film/record.py kaiju-lunch-break-film.elf $(TAKE) --prefix 'KL FILM' \
		--flycast ../ridgeline/tools/film/flycast-film.sh --footage "$(HOME)/Movies/Kaiju Lunch Break Footage" --work captures/film \
		--stop-on 'KL FILM done' --stop-after 1

disc: $(TARGET) music
	../../../tools/flycast/elf-to-cdi.sh --cdda cdda $(TARGET) kaiju-lunch-break.cdi

DC_HW = ../../../tools/dc-hw.sh
hw: $(TARGET)
	$(DC_HW) run -c -T 86400 kaiju-lunch-break.elf -- -m $(CURDIR)

# No music unless MUSIC=1 and a Kaiju Lunch Break disc is in the drive.
.PHONY: run textures models audio music assets autotest vmutest capture film disc hw

endif
