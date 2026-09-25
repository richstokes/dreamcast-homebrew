ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = drift-los-angeles.elf
OBJS = drift-los-angeles.o assets/generated/texture_assets.o assets/generated/audio_assets.o assets/generated/music_asset.o
BLENDER ?= blender
QA_PREVIEW_DIR = assets/generated/previews/visual-qa
QA_LOG ?= $(QA_PREVIEW_DIR)/benchmark.log
SOUNDTRACK_DIR = assets/generated/previews/soundtrack
QA_SHOTS = $(QA_PREVIEW_DIR)/downtown.png $(QA_PREVIEW_DIR)/coast.png $(QA_PREVIEW_DIR)/arts.png $(QA_PREVIEW_DIR)/neon.png
QA_SEGMENT ?= 0
QA_CAPTURE_LOCAL ?= 8.0f
STRICT_CFLAGS = -O2 -Wall -Wextra -Wpedantic -Werror
SH4ZAM_HEADERS = $(wildcard third_party/sh4zam/include/sh4zam/*.h \
    third_party/sh4zam/include/sh4zam/inline/*.h \
    third_party/sh4zam/include/sh4zam/inline/sh4/*.h)

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET) tools/render_math_qa.o render-math-qa.elf tools/render_visibility_qa.o render-visibility-qa.elf

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lm

drift-los-angeles.o: assets/generated/texture_assets.h assets/generated/audio_assets.h assets/generated/music_asset.h model_data.h pedestrian_data.h vehicle_data.h prop_data.h render_math.h render_visibility.h $(SH4ZAM_HEADERS)
assets/generated/texture_assets.o: assets/generated/texture_assets.h
assets/generated/audio_assets.o: assets/generated/audio_assets.h
assets/generated/music_asset.o: assets/generated/music_asset.S assets/generated/music_asset.adpcm

architecture:
	uv run tools/build_architecture.py --source-dir assets/source --output-dir assets/source

effects:
	uv run tools/build_effect_textures.py --output-dir assets/source

textures: architecture effects
	uv run tools/build_textures.py --source-dir assets/source --output-dir assets/generated

audio: music
	python3 tools/build_audio_assets.py --idle-source assets/source/audio/v8-steady-idle.wav --load-source assets/source/audio/v8-steady-high.wav --tire-source assets/source/audio/tire-screech.wav --output-dir assets/generated --preview assets/generated/previews/v8-engine-rpm-sweep.wav

music:
	uv run tools/compose_soundtrack.py --output-dir $(SOUNDTRACK_DIR)
	uv run tools/build_music_asset.py --manifest $(SOUNDTRACK_DIR)/tracks.json --output-dir assets/generated

model:
	$(BLENDER) --background --python tools/build_car_blender.py -- --header model_data.h

pedestrians:
	$(eval PED_PREVIEW_TMP := $(shell mktemp -d -t dla-pedestrians))
	uv run tools/build_pedestrians.py --atlas assets/source/pedestrian-atlas.png --header pedestrian_data.h --preview-dir $(PED_PREVIEW_TMP)
	$(BLENDER) --background --python tools/preview_pedestrians_blender.py -- --input-dir $(PED_PREVIEW_TMP) --output assets/generated/previews/pedestrians.png

vehicles:
	$(eval VEH_PREVIEW_TMP := $(shell mktemp -d -t dla-vehicles))
	uv run tools/build_vehicles.py --atlas assets/source/vehicle-atlas.png --header vehicle_data.h --preview-dir $(VEH_PREVIEW_TMP)
	$(BLENDER) --background --python tools/preview_pedestrians_blender.py -- --input-dir $(VEH_PREVIEW_TMP) --output assets/generated/previews/vehicles.png

props:
	$(eval PROP_PREVIEW_TMP := $(shell mktemp -d -t dla-props))
	uv run tools/build_props.py --atlas assets/source/prop-atlas.png --header prop_data.h --preview-dir $(PROP_PREVIEW_TMP)
	$(BLENDER) --background --python tools/preview_pedestrians_blender.py -- --input-dir $(PROP_PREVIEW_TMP) --output assets/generated/previews/props.png

run: $(TARGET)
	./run-flycast.sh --skip-build

showcase:
	$(MAKE) clean
	$(MAKE) CFLAGS=-DDRIFT_LA_SHOWCASE
	./run-flycast.sh --skip-build

qa:
	uv run tools/visual_qa.py --strict

qa-tools:
	uv run tools/test_graphics_qa.py
	uv run tools/test_pvr_texture.py
	uv run tools/test_compare_geometry_logs.py

tools/render_math_qa.o: tools/render_math_qa.c render_math.h $(SH4ZAM_HEADERS)

render-math-qa.elf: tools/render_math_qa.o
	kos-cc -o $@ $< -lm

math-qa-build:
	-rm -f tools/render_math_qa.o render-math-qa.elf
	$(MAKE) render-math-qa.elf CFLAGS='$(STRICT_CFLAGS)'

tools/render_visibility_qa.o: tools/render_visibility_qa.c render_visibility.h

render-visibility-qa.elf: tools/render_visibility_qa.o
	kos-cc -o $@ $< -lm

visibility-qa-build:
	-rm -f tools/render_visibility_qa.o render-visibility-qa.elf
	$(MAKE) render-visibility-qa.elf CFLAGS='$(STRICT_CFLAGS)'

qa-build:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(STRICT_CFLAGS) -DDRIFT_LA_SHOWCASE -DDRIFT_LA_VISUAL_QA'

qa-run: qa-build
	./run-flycast.sh --skip-build

geometry-qa-build:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(STRICT_CFLAGS) -DDRIFT_LA_SHOWCASE -DDRIFT_LA_VISUAL_QA -DDRIFT_LA_GEOMETRY_QA'

geometry-qa-run: geometry-qa-build
	DRIFT_LA_EXIT_ON_LOG='Drift Los Angeles geometry QA: complete;' \
		./run-flycast.sh --skip-build --input keyboard

qa-benchmark: qa-build
	mkdir -p "$(dir $(QA_LOG))"
	bash -o pipefail -c 'DRIFT_LA_EXIT_ON_LOG="Drift Los Angeles benchmark:" ./run-flycast.sh --skip-build --input keyboard 2>&1 | tee "$(QA_LOG)"'
	uv run tools/analyze_render_log.py "$(QA_LOG)" --json "$(QA_LOG).json"

qa-capture-build:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(STRICT_CFLAGS) -DDRIFT_LA_SHOWCASE -DDRIFT_LA_VISUAL_QA -DDRIFT_LA_CAPTURE_SEGMENT=$(QA_SEGMENT) -DDRIFT_LA_CAPTURE_LOCAL=$(QA_CAPTURE_LOCAL)'

qa-capture-run: qa-capture-build
	./run-flycast.sh --skip-build

qa-visual:
	uv run tools/visual_qa.py --strict --json $(QA_PREVIEW_DIR)/report.json \
		--contact-sheet $(QA_PREVIEW_DIR)/contact-sheet.png $(QA_SHOTS)

physics-qa-build:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(STRICT_CFLAGS) -DDRIFT_LA_AUTOTEST -DDRIFT_LA_PHYSICS_QA'

physics-qa-run: physics-qa-build
	DRIFT_LA_EXIT_ON_LOG='physics QA: ALL TESTS PASS.' \
	DRIFT_LA_FAIL_ON_LOG='physics QA: FAILED.' \
		./run-flycast.sh --skip-build --input keyboard

collision-qa-build:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(STRICT_CFLAGS) -DDRIFT_LA_AUTOTEST -DDRIFT_LA_COLLISION_QA'

collision-qa-run: collision-qa-build
	DRIFT_LA_EXIT_ON_LOG='collision QA: ALL TESTS PASS.' \
	DRIFT_LA_FAIL_ON_LOG='collision QA: FAILED.' \
		./run-flycast.sh --skip-build --input keyboard

.PHONY: all clean run showcase qa qa-tools math-qa-build visibility-qa-build qa-build qa-run geometry-qa-build geometry-qa-run qa-benchmark qa-capture-build qa-capture-run qa-visual physics-qa-build physics-qa-run collision-qa-build collision-qa-run architecture effects textures audio music model pedestrians vehicles props

endif
