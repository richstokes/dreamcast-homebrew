ifneq ($(SH4ZAM_UPDATED),1)
include ../../../tools/sh4zam.mk
else

TARGET = pico8-dreamcast.elf
KOS_BUILD_SUBARCHS = pristine
KOS_ROMDISK_DIR = romdisk
CORE = third_party/fake-08
LUA = $(CORE)/libs/z8lua
CORE_SRC = $(filter-out $(CORE)/source/main.cpp $(CORE)/source/hostCommonFunctions.cpp $(CORE)/source/logger.cpp,$(wildcard $(CORE)/source/*.cpp))
LUA_SRC = $(filter-out $(LUA)/ltests.c $(LUA)/liolib.c $(LUA)/loslib.c $(LUA)/loadlib.c $(LUA)/lbitlib.c,$(wildcard $(LUA)/*.c))
CPP_SRC = main.cpp host.cpp logger.cpp lua_alloc.cpp download.cpp download_screen.cpp $(CORE_SRC) $(CORE)/libs/lodepng/lodepng.cpp
OBJS = $(patsubst %.cpp,build/%.o,$(CPP_SRC)) $(patsubst %.c,build/%.o,$(LUA_SRC)) romdisk.o
# -O2 runs the Lua VM and rasterizers faster on hardware than -O3: the
# SH-4 has an 8 KiB instruction cache. The audio mixer is a tight loop and
# stays at -O3, which also keeps its regression hashes (see tests/kernels.cpp).
OPT ?= -O2
AUDIO_OBJS = $(addprefix build/$(CORE)/source/,Audio.o synth.o filter.o)
$(AUDIO_OBJS): OPT = -O3
CXXFLAGS = -std=gnu++17 $(OPT) -flto -fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections -MMD -MP
CPPFLAGS = -I$(CORE)/source -I$(LUA) -I$(CORE)/libs/lodepng -DVER_STR=\"Dreamcast\" -DLOAD_PACK_INS=0

all: $(TARGET)
LIBS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lpthread -lppp -lm
# Bound individual PNG allocations, including deflate expansion.
CPPFLAGS += -DLODEPNG_MAX_ALLOC=1048576
CPPFLAGS += -I. -Ithird_party/sh4zam/include
# Z8lua is compiled as C++, which would make every error and every
# coroutine.yield a C++ throw. FAKE-08 runs each cart's main loop in a
# coroutine that yields once per frame, and SH-4 unwinding costs milliseconds.
CPPFLAGS += -DLUA_USE_LONGJMP
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

romdisk/SH4ZAM-LICENSE.txt: FORCE
	@cmp -s third_party/sh4zam/LICENSE $@ || cp third_party/sh4zam/LICENSE $@

romdisk.img: $(shell find romdisk -type f) build/romdisk-files

# Pad the ROM disk to a multiple of 16 KiB, the SH-4's direct-mapped operand
# cache, so data linked after it keeps its cache sets when bundled files
# change. Unpadded, a 9 KiB test-cart change made the audio mixer up to 60%
# slower on hardware.
romdisk.o: build/romdisk-padded
build/romdisk-padded: romdisk.img
	@mkdir -p build
	@size=$$(wc -c < romdisk.img); \
	 head -c $$(( (16384 - size % 16384) % 16384 )) /dev/zero >> romdisk.img
	@touch $@

$(TARGET): $(OBJS)
	kos-c++ $(OPT) -flto -Wl,--gc-sections -o $@ $(OBJS) $(LIBS)
	kos-objcopy --only-keep-debug $@ build/$@.debug
	kos-objcopy --strip-debug $@

# The player for hardware runs under dcload-ip (tools/dc-hw.sh): no boot-time
# networking, so no Download & Play.
build/main-hw.o: main.cpp platform.h
	@mkdir -p build
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -DDC_DCLOAD -c $< -o $@

pico8-hw.elf: $(filter-out build/main.o,$(OBJS)) build/main-hw.o
	kos-c++ $(OPT) -flto -Wl,--gc-sections -o $@ $^ $(LIBS)
	kos-objcopy --only-keep-debug $@ build/$@.debug
	kos-objcopy --strip-debug $@

DC_HW = ../../../tools/dc-hw.sh
HW_LOG ?= build/hw-console.log

# Play on the console. dc-tool-ip stays attached: a printf with nobody
# listening would freeze the console.
hw: pico8-hw.elf
	$(DC_HW) run -c -T 86400 -l $(HW_LOG) pico8-hw.elf

# The smoke suite and its per-cart timings on the console.
hwbench: pico8-smoke.elf
	$(DC_HW) run -c -o -T 600 -i 90 -u 'PICO8_TEST: (PASS|FAIL) ALL' -l $(HW_LOG) pico8-smoke.elf
	grep -q 'PICO8_TEST: PASS ALL' $(HW_LOG)

build/main-smoke.o: main.cpp platform.h
	@mkdir -p build
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -DDC_SMOKE_TEST -c $< -o $@

pico8-smoke.elf: $(filter-out build/main.o,$(OBJS)) build/main-smoke.o build/tests/kernels.o
	kos-c++ $(OPT) -flto -Wl,--gc-sections -o $@ $^ $(LIBS)
	kos-objcopy --only-keep-debug $@ build/$@.debug
	kos-objcopy --strip-debug $@

build/main-profile.o: main.cpp platform.h tests/profile.h
	@mkdir -p build
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -DDC_SMOKE_TEST -DDC_PROFILE -c $< -o $@

pico8-profile.elf: $(filter-out build/main.o,$(OBJS)) build/main-profile.o build/tests/profile.o build/tests/kernels.o
	kos-c++ $(OPT) -flto -Wl,--gc-sections -o $@ $^ $(LIBS)
	kos-objcopy --only-keep-debug $@ build/$@.debug
	kos-objcopy --strip-debug $@

build/main-kernels.o: main.cpp platform.h tests/kernels.h
	@mkdir -p build
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -DDC_KERNEL_TEST -c $< -o $@

pico8-kernels.elf: $(filter-out build/main.o,$(OBJS)) build/main-kernels.o build/tests/kernels.o
	kos-c++ $(OPT) -flto -Wl,--gc-sections -o $@ $^ $(LIBS)
	kos-objcopy --only-keep-debug $@ build/$@.debug
	kos-objcopy --strip-debug $@

build/main-net.o: main.cpp platform.h download.h
	@mkdir -p build
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -DDC_NETWORK_TEST -c $< -o $@

build/download-screen-net.o: download_screen.cpp platform.h download.h
	kos-c++ $(CXXFLAGS) $(CPPFLAGS) -DDC_NETWORK_TEST -c $< -o $@

# Test roots and URLs are generated only under build/, never the release ROM.
build/network-romdisk.o: build/network-romdisk.stamp
	$(KOS_GENROMFS) -f build/network-romdisk.img -d build/network-romdisk
	$(KOS_BASE)/utils/bin2c/bin2c build/network-romdisk.img build/network-romdisk.c romdisk
	kos-cc -o build/network-romdisk-tmp.o -c build/network-romdisk.c
	$(KOS_CC) -o $@ -r build/network-romdisk-tmp.o $(KOS_LIB_PATHS) -Wl,--whole-archive -lromdiskbase -nostdlib

pico8-network-test.elf: $(filter-out build/main.o build/download_screen.o romdisk.o,$(OBJS)) build/main-net.o build/download-screen-net.o build/tests/network_smoke.o build/network-romdisk.o
	kos-c++ $(OPT) -flto -Wl,--gc-sections -o $@ $^ $(LIBS)
	kos-objcopy --only-keep-debug $@ build/$@.debug
	kos-objcopy --strip-debug $@

run: $(TARGET)
	./run-flycast.sh --skip-build

check:
	uv run tests/check_samples.py

smoke-test:
	uv run tests/run_smoke.py

network-test:
	uv run tests/run_network.py

clean:
	$(RM) build/main-profile.o build/tests/profile.o pico8-profile.elf
	$(RM) build/main-kernels.o build/tests/kernels.o pico8-kernels.elf
	$(RM) build/main-hw.o pico8-hw.elf
	$(RM) build/romdisk-padded $(OBJS) build/main-smoke.o build/main-net.o build/download-screen-net.o build/tests/network_smoke.o build/network-romdisk.o pico8-network-test.elf pico8-smoke.elf $(TARGET) romdisk.img romdisk_tmp.c romdisk_tmp.o

-include $(OBJS:.o=.d)
-include build/main-hw.d build/main-smoke.d build/main-net.d build/download-screen-net.d build/tests/network_smoke.d
-include build/main-profile.d build/tests/profile.d
-include build/main-kernels.d build/tests/kernels.d
.PHONY: all run hw hwbench clean check smoke-test network-test FORCE

endif
