TARGET = drift-los-angeles.elf
OBJS = drift-los-angeles.o assets/generated/texture_assets.o assets/generated/audio_assets.o assets/generated/music_asset.o
BLENDER ?= blender
QA_PREVIEW_DIR = assets/generated/previews/visual-qa
QA_SHOTS = $(QA_PREVIEW_DIR)/downtown.png $(QA_PREVIEW_DIR)/coast.png $(QA_PREVIEW_DIR)/arts.png $(QA_PREVIEW_DIR)/neon.png
QA_SEGMENT ?= 0
QA_CAPTURE_LOCAL ?= 8.0f

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lm

drift-los-angeles.o: assets/generated/texture_assets.h assets/generated/audio_assets.h assets/generated/music_asset.h model_data.h
assets/generated/texture_assets.o: assets/generated/texture_assets.h
assets/generated/audio_assets.o: assets/generated/audio_assets.h
assets/generated/music_asset.o: assets/generated/music_asset.S assets/generated/music_asset.mulaw

textures:
	uv run tools/build_textures.py --source-dir assets/source --output-dir assets/generated

audio: music
	python3 tools/build_audio_assets.py --idle-source assets/source/audio/v8-steady-idle.wav --load-source assets/source/audio/v8-steady-high.wav --tire-source assets/source/audio/tire-screech.wav --output-dir assets/generated --preview assets/generated/previews/v8-engine-rpm-sweep.wav

music:
	python3 tools/build_music_asset.py --source assets/source/audio/music/pure_raceway_bpm160.ogg --output-dir assets/generated

model:
	$(BLENDER) --background --python tools/build_car_blender.py -- --header model_data.h

run: $(TARGET)
	./run-flycast.sh --skip-build

showcase:
	$(MAKE) clean
	$(MAKE) CFLAGS=-DDRIFT_LA_SHOWCASE
	./run-flycast.sh --skip-build

qa:
	uv run tools/visual_qa.py --strict

qa-build:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O2 -Wall -Wextra -Wpedantic -Werror -DDRIFT_LA_SHOWCASE -DDRIFT_LA_VISUAL_QA'

qa-run: qa-build
	./run-flycast.sh --skip-build

qa-capture-build:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O2 -Wall -Wextra -Wpedantic -Werror -DDRIFT_LA_SHOWCASE -DDRIFT_LA_VISUAL_QA -DDRIFT_LA_CAPTURE_SEGMENT=$(QA_SEGMENT) -DDRIFT_LA_CAPTURE_LOCAL=$(QA_CAPTURE_LOCAL)'

qa-capture-run: qa-capture-build
	./run-flycast.sh --skip-build

qa-visual:
	uv run tools/visual_qa.py --strict --json $(QA_PREVIEW_DIR)/report.json \
		--contact-sheet $(QA_PREVIEW_DIR)/contact-sheet.png $(QA_SHOTS)

.PHONY: all clean run showcase qa qa-build qa-run qa-capture-build qa-capture-run qa-visual textures audio music model
