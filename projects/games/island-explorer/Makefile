ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = island-explorer.elf
OBJS = main.o world.o render.o dolphins.o audio.o assets.o
SH4ZAM_HEADERS = $(wildcard third_party/sh4zam/include/sh4zam/*.h \
    third_party/sh4zam/include/sh4zam/inline/*.h \
    third_party/sh4zam/include/sh4zam/inline/sh4/*.h)
CFLAGS += -std=gnu17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -MMD -MP
CPPFLAGS += -Ithird_party/sh4zam/include
KOS_BUILD_SUBARCHS = pristine
all: $(TARGET)
include $(KOS_BASE)/Makefile.rules
$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lm
main.o world.o render.o dolphins.o audio.o: engine.h
dolphins.o render.o: dolphins.h
render.o: assets/generated/asset_ids.h render_math.h $(SH4ZAM_HEADERS)
assets.o: third_party/sh4zam/LICENSE
assets.o: assets/generated/coast0.bin assets/generated/coast1.bin assets/generated/textures.bin assets/generated/sky.bin assets/generated/ambience.pcm assets/generated/font.bin assets/generated/foam0.bin assets/generated/foam1.bin
assets/generated/asset_ids.h assets/generated/coast0.bin:
	@echo 'Run uv run tools/extract_stage.py --gdi /path/to/Sonic-Adventure.gdi first.'
	@false
assets/generated/ambience.pcm: tools/build_atmosphere.py
	uv run tools/build_atmosphere.py
# Serialize the two outputs even with parallel make and a missing asset cache.
assets/generated/font.bin: assets/generated/ambience.pcm
	@test -f $@ || uv run tools/build_atmosphere.py
clean:
	-rm -f $(OBJS) $(TARGET) main_test.o island-explorer-test.elf *.d
	-rm -f build/main_bench.o build/main_reference.o build/render_bench.o build/render_reference.o build/render_math_qa.o build/*.d
	-rm -f island-explorer-bench.elf island-explorer-reference.elf render-math-qa.elf
	-rm -f build/main_dolphins.o island-explorer-dolphins.elf
run: $(TARGET)
	./run-flycast.sh --skip-build
assets:
	uv run tools/extract_stage.py $(if $(GDI),--gdi "$(GDI)",)
	uv run tools/build_atmosphere.py
check-import:
	uv run --with numpy --with pillow python -m unittest discover -s tests -v
.PHONY: all clean run assets check-import

check-math: render-math-qa.elf
	uv run tools/run_checks.py --math
check-runtime: island-explorer-test.elf
	uv run tools/run_checks.py --traversal
benchmark: island-explorer-bench.elf island-explorer-reference.elf
	uv run tools/run_checks.py --benchmark
check-dolphins: island-explorer-dolphins.elf
	uv run tools/run_checks.py --dolphins
.PHONY: check-math check-runtime check-dolphins benchmark

build/main_dolphins.o: main.c engine.h dolphins.h tests/dolphin_preview.h tests/dolphin_checks.h
	@mkdir -p build
	kos-cc $(CFLAGS) $(CPPFLAGS) -DIE_DOLPHIN_PREVIEW -c $< -o $@
island-explorer-dolphins.elf: build/main_dolphins.o world.o render.o dolphins.o audio.o assets.o
	kos-cc -o $@ $^ -lm

main_test.o: main.c engine.h tests/runtime_checks.h
	kos-cc $(CFLAGS) $(CPPFLAGS) -DIE_AUTOTEST -c main.c -o $@
island-explorer-test.elf: main_test.o world.o render.o dolphins.o audio.o assets.o
	kos-cc -o $@ $^ -lm

build/main_bench.o: main.c engine.h tests/render_benchmark.h
	@mkdir -p build
	kos-cc $(CFLAGS) $(CPPFLAGS) -DIE_BENCHMARK -c $< -o $@
build/main_reference.o: main.c engine.h tests/render_benchmark.h
	@mkdir -p build
	kos-cc $(CFLAGS) $(CPPFLAGS) -DIE_BENCHMARK -DIE_REFERENCE_RENDER -c $< -o $@
build/render_bench.o: render.c engine.h render_math.h assets/generated/asset_ids.h $(SH4ZAM_HEADERS)
	@mkdir -p build
	kos-cc $(CFLAGS) $(CPPFLAGS) -DIE_BENCHMARK -c $< -o $@
build/render_reference.o: render.c engine.h render_math.h assets/generated/asset_ids.h $(SH4ZAM_HEADERS)
	@mkdir -p build
	kos-cc $(CFLAGS) $(CPPFLAGS) -DIE_BENCHMARK -DIE_REFERENCE_RENDER -c $< -o $@
island-explorer-bench.elf: build/main_bench.o build/render_bench.o world.o dolphins.o audio.o assets.o
	kos-cc -o $@ $^ -lm
island-explorer-reference.elf: build/main_reference.o build/render_reference.o world.o dolphins.o audio.o assets.o
	kos-cc -o $@ $^ -lm

build/render_math_qa.o: tests/render_math_qa.c engine.h render_math.h $(SH4ZAM_HEADERS)
	@mkdir -p build
	kos-cc $(CFLAGS) $(CPPFLAGS) -c $< -o $@
render-math-qa.elf: build/render_math_qa.o
	kos-cc -o $@ $^ -lm

-include $(wildcard *.d build/*.d)
endif
