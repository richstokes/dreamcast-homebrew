TARGET = gravity-wave.elf
OBJS = gravity-wave.o assets/generated/texture_assets.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lm

gravity-wave.o: assets/generated/texture_assets.h model_data.h
assets/generated/texture_assets.o: assets/generated/texture_assets.h

textures:
	uv run tools/build_textures.py --source-dir assets/source --output-dir assets/generated

run: $(TARGET)
	./run-flycast.sh --skip-build

.PHONY: all clean run textures
