TARGET = drift-los-angeles.elf
OBJS = drift-los-angeles.o assets/generated/texture_assets.o assets/generated/audio_assets.o assets/generated/music_asset.o
BLENDER ?= blender

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

.PHONY: all clean run showcase textures audio music model
