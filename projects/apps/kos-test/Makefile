TARGET = kos-test.elf
SRCS = src/main.c src/buildinfo.c src/gfx.c src/report.c src/util.c src/vmutest.c src/sysinfo.c \
       src/inputtest.c src/selftest.c src/cputest.c src/bench.c src/gpubench.c \
       src/gputest.c src/iobench.c src/monitor.c src/sound.c src/nettest.c
COMMON_SRCS = ../../common/dialup.c

KOS_BUILD_SUBARCHS = pristine
KOS_CSTD = -std=gnu17

# VARIANT builds a separately named ELF from its own objects:
#   autotest  walks every screen with no input and prints KT AUTOTEST DONE
#   tour      the same for the GPU, monitor, sound and network pages only
#   nettest   the network test alone
#   auto      self-tests, CPU suite and benchmarks with no input, then the
#             results as one KT RESULTS block and back to dcload (make auto)
VARIANT ?=
EXTRA_CFLAGS ?=
# SDK names another KOS tree the objects and ELF are kept apart for
# (SDK=stock: see auto-stock below). AUTO_VMU=1 adds the VMU test to auto.
SDK ?=
AUTO_VMU ?=
VARIANT_FLAGS_autotest = -DKT_AUTOTEST
VARIANT_FLAGS_tour = -DKT_AUTOTEST -DKT_TOUR
VARIANT_FLAGS_nettest = -DKT_AUTOTEST -DKT_TOUR -DKT_TOUR_NET
VARIANT_FLAGS_auto = -DKT_AUTOTEST -DKT_AUTO $(if $(AUTO_VMU),-DKT_AUTO_VMU)
BUILD = build/$(if $(VARIANT),$(VARIANT),release)$(if $(SDK),-$(SDK))
ELF = $(if $(VARIANT),kos-test-$(VARIANT),kos-test)$(if $(SDK),-$(SDK)).elf
OBJS = $(SRCS:src/%.c=$(BUILD)/%.o) $(BUILD)/cpuasm.o $(COMMON_SRCS:../../common/%.c=$(BUILD)/%.o) \
       $(BUILD)/romdisk.o

CFLAGS_KT = -Isrc -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            $(VARIANT_FLAGS_$(VARIANT)) $(EXTRA_CFLAGS)

all: $(ELF)

include $(KOS_BASE)/Makefile.rules

# Rebuild when the flags change, not only the sources.
FLAGS_STAMP = $(BUILD)/flags
$(FLAGS_STAMP): FORCE | $(BUILD)
	@echo '$(CFLAGS_KT)' | cmp -s - $@ || echo '$(CFLAGS_KT)' > $@

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) ../../common/vmusave.h ../../common/softreset.h $(FLAGS_STAMP) | $(BUILD)
	kos-cc $(KOS_CFLAGS) $(CFLAGS_KT) -c $< -o $@

# The commits this ELF reports with its results. KOS_PATCHES hashes the
# SDK's local changes (tools/kos/patches), so patched and stock builds differ.
KOS_COMMIT := $(shell git -C "$(KOS_BASE)" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)
KOS_PATCHES := $(shell d=$$(git -C "$(KOS_BASE)" diff HEAD 2>/dev/null); \
                 if [ -z "$$d" ]; then echo none; else printf '%s' "$$d" | sha1sum | cut -c1-8; fi)
# A dirty tree adds a hash of this project's sources, so records from
# uncommitted builds still say which benchmark code made them.
TEST_COMMIT := $(shell c=$$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown); \
                 [ -n "$$(git status --porcelain -- . 2>/dev/null)" ] && \
                 c=$$c-dirty-$$(find src tools Makefile -type f | LC_ALL=C sort | xargs cat | sha1sum | cut -c1-8); \
                 echo $$c)

BUILDINFO = $(KOS_COMMIT) $(KOS_PATCHES) $(TEST_COMMIT)
BUILDINFO_STAMP = $(BUILD)/buildinfo
$(BUILDINFO_STAMP): FORCE | $(BUILD)
	@echo '$(BUILDINFO)' | cmp -s - $@ || echo '$(BUILDINFO)' > $@

# Rebuilt with any other object, so its build date stays current.
$(BUILD)/buildinfo.o: src/buildinfo.c src/kt.h $(BUILDINFO_STAMP) \
                      $(filter-out $(BUILD)/buildinfo.o,$(OBJS)) | $(BUILD)
	kos-cc $(KOS_CFLAGS) $(CFLAGS_KT) -DKT_KOS_COMMIT='"$(KOS_COMMIT)"' \
	    -DKT_KOS_PATCHES='"$(KOS_PATCHES)"' -DKT_TEST_COMMIT='"$(TEST_COMMIT)"' -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	kos-cc $(KOS_CFLAGS) -c $< -o $@

$(BUILD)/%.o: ../../common/%.c ../../common/%.h $(FLAGS_STAMP) | $(BUILD)
	kos-cc $(KOS_CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

# The filesystem benchmarks' romdisk, made at build time (shared by all
# variants). KOS mounts a linked-in romdisk at /rd.
build/romdisk/big.bin: tools/make_romdisk.py
	python3 tools/make_romdisk.py build/romdisk

build/romdisk.img: build/romdisk/big.bin
	$(KOS_GENROMFS) -f $@ -d build/romdisk -x .DS_Store > /dev/null

$(BUILD)/romdisk.o: build/romdisk.img | $(BUILD)
	$(KOS_BASE)/utils/bin2c/bin2c $< $(BUILD)/romdisk_tmp.c romdisk
	kos-cc -o $(BUILD)/romdisk_tmp.o -c $(BUILD)/romdisk_tmp.c
	$(KOS_CC) -o $@ -r $(BUILD)/romdisk_tmp.o $(KOS_LIB_PATHS) -Wl,--whole-archive -lromdiskbase -nostdlib
	rm $(BUILD)/romdisk_tmp.c $(BUILD)/romdisk_tmp.o

$(ELF): $(OBJS) $(KOS_BASE)/lib/$(KOS_ARCH)/libkallisti.a
	kos-cc -o $@ $(OBJS) -lppp -lpthread -lm

clean:
	-rm -rf build kos-test*.elf

run: $(ELF)
	./run-flycast.sh --skip-build

# Scripted run in Flycast on blank VMUs: every screen, every suite.
autotest:
	$(MAKE) VARIANT=autotest
	KT_ELF=kos-test-autotest.elf KT_EXIT_ON_LOG='KT AUTOTEST DONE' \
	    KT_FAIL_ON_LOG='KOS panic' KT_BLANK_VMUS=1 ./run-flycast.sh --skip-build

# Real Dreamcast through tools/dc-hw.sh (dcload-ip hands the adapter over).
DC_HW = ../../../tools/dc-hw.sh
hw: $(TARGET)
	$(DC_HW) run -c $(TARGET)

# Auto mode on the console over dcload-ip: every suite and benchmark, the
# results recorded in benchmark_results/ against the KOS commit, console off.
AUTO_DIR = build/auto
RECORD = python3 tools/record_results.py
auto:
	$(MAKE) VARIANT=auto AUTO_VMU=$(AUTO_VMU)
	@mkdir -p $(AUTO_DIR)
	$(DC_HW) run -c -o -T 600 -i 300 -u 'KT AUTOTEST DONE' -l $(AUTO_DIR)/console.log kos-test-auto.elf
	$(RECORD) $(AUTO_DIR)/console.log

# The same against an unpatched KOS tree from ../../../tools/kos/stock-sdk.sh,
# to show what this repo's KOS patches fix on the console. Recorded with
# kos_patches "none".
STOCK_KOS ?= $(HOME)/.local/share/dreamcast/kos-stock
STOCK_MAKE = bash -c 'set +u; source "$(STOCK_KOS)/environ.sh"; set -u; \
                      $(MAKE) VARIANT=auto SDK=stock AUTO_VMU=$(AUTO_VMU)'
auto-stock:
	@test -f "$(STOCK_KOS)/environ.sh" || { echo "No stock KOS at $(STOCK_KOS): run ../../../tools/kos/stock-sdk.sh" >&2; exit 1; }
	$(STOCK_MAKE)
	@mkdir -p $(AUTO_DIR)
	$(DC_HW) run -c -o -T 600 -i 300 -u 'KT AUTOTEST DONE' -l $(AUTO_DIR)/console-stock.log kos-test-auto-stock.elf
	$(RECORD) $(AUTO_DIR)/console-stock.log

# The same in Flycast, recorded under build/ (emulator timings aren't kept).
auto-flycast:
	$(MAKE) VARIANT=auto AUTO_VMU=$(AUTO_VMU)
	@mkdir -p $(AUTO_DIR)
	KT_ELF=kos-test-auto.elf KT_EXIT_ON_LOG='KT AUTOTEST DONE' KT_FAIL_ON_LOG='KOS panic' \
	    KT_BLANK_VMUS=1 ./run-flycast.sh --skip-build | tee $(AUTO_DIR)/flycast.log
	$(RECORD) --out $(AUTO_DIR)/results $(AUTO_DIR)/flycast.log

auto-stock-flycast:
	@test -f "$(STOCK_KOS)/environ.sh" || { echo "No stock KOS at $(STOCK_KOS): run ../../../tools/kos/stock-sdk.sh" >&2; exit 1; }
	$(STOCK_MAKE)
	@mkdir -p $(AUTO_DIR)
	KT_ELF=kos-test-auto-stock.elf KT_EXIT_ON_LOG='KT AUTOTEST DONE' KT_FAIL_ON_LOG='KOS panic' \
	    KT_BLANK_VMUS=1 ./run-flycast.sh --skip-build | tee $(AUTO_DIR)/flycast-stock.log
	$(RECORD) --out $(AUTO_DIR)/results $(AUTO_DIR)/flycast-stock.log

.PHONY: all clean run autotest hw auto auto-stock auto-flycast auto-stock-flycast FORCE
