TARGET = pico8-dreamcast.elf
KOS_BUILD_SUBARCHS = pristine
KOS_ROMDISK_DIR = romdisk
CORE = third_party/fake-08
LUA = $(CORE)/libs/z8lua
CORE_SRC = $(filter-out $(CORE)/source/main.cpp $(CORE)/source/hostCommonFunctions.cpp $(CORE)/source/logger.cpp,$(wildcard $(CORE)/source/*.cpp))
LUA_SRC = $(filter-out $(LUA)/ltests.c $(LUA)/liolib.c $(LUA)/loslib.c $(LUA)/loadlib.c $(LUA)/lbitlib.c,$(wildcard $(LUA)/*.c))
CPP_SRC = main.cpp host.cpp logger.cpp $(CORE_SRC) $(CORE)/libs/lodepng/lodepng.cpp
OBJS = $(patsubst %.cpp,build/%.o,$(CPP_SRC)) $(patsubst %.c,build/%.o,$(LUA_SRC)) romdisk.o
CXXFLAGS = -std=gnu++17 -O3 -flto -fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections -MMD -MP
CPPFLAGS = -I$(CORE)/source -I$(LUA) -I$(CORE)/libs/lodepng -DVER_STR=\"Dreamcast\" -DLOAD_PACK_INS=0

all: $(TARGET)
include $(KOS_BASE)/Makefile.rules

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# Z8lua uses C++ fixed-point types, including in its .c files.
build/%.o: %.c
	@mkdir -p $(dir $@)
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -x c++ -c $< -o $@

build/romdisk-files: FORCE
	@mkdir -p build
	@find romdisk -type f | LC_ALL=C sort > build/romdisk-files.new
	@cmp -s build/romdisk-files.new $@ || cp build/romdisk-files.new $@

romdisk.img: $(shell find romdisk -type f) build/romdisk-files

$(TARGET): $(OBJS)
	kos-c++ -O3 -flto -Wl,--gc-sections -o $@ $(OBJS) -lm

build/main-smoke.o: main.cpp platform.h
	@mkdir -p build
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -DDC_SMOKE_TEST -c $< -o $@

pico8-smoke.elf: $(filter-out build/main.o,$(OBJS)) build/main-smoke.o
	kos-c++ -O3 -flto -Wl,--gc-sections -o $@ $^ -lm

run: $(TARGET)
	./run-flycast.sh --skip-build

check:
	uv run tests/check_samples.py

smoke-test:
	uv run tests/run_smoke.py

clean:
	$(RM) $(OBJS) build/main-smoke.o pico8-smoke.elf $(TARGET) romdisk.img romdisk_tmp.c romdisk_tmp.o

-include $(OBJS:.o=.d)
.PHONY: all run clean check smoke-test FORCE
