# Shared build rules for the games on the engine (DreamKarts, Ridgeline).
# A game's Makefile sets these, then includes this file before any rule of
# its own, inside the tools/sh4zam.mk guard:
#
#   TARGET          dreamkarts.elf
#   GAME_PREFIX     DK: names the version macro ($(GAME_PREFIX)_GIT_SHA)
#   SOURCES         .c files in link order, the engine's among them by
#                   bare name (render.c); the order sets the code layout
#                   the console's timings were measured on
#   COMMON_SOURCES  .c files from projects/common (optional)
#   ASM_SOURCES     .S files from the engine (xform.S)
#   BLOBS           the generated binaries src/blobs.S includes
#   VARIANT_FLAGS_<variant>  -D flags per build variant; VARIANT picks one
#   LIBS            -lm, plus the game's own
#   EXTRA_INCLUDES  more -I flags
#   VERSION_SOURCES .c files that use the version header (main.c)
#
# Variants share sources and each gets an object directory; extra -D flags
# (EXTRA_CFLAGS) get their own too, so builds never mix. Targets:
# $(TARGET), <name>-<variant>.elf, clean. Debug info is kept beside the
# ELF (sh-elf-addr2line on panic addresses) and stripped from it, which
# keeps a game under the 16 MB Flycast will boot.
ENGINE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
COMMON_DIR := $(patsubst %/,%,$(dir $(ENGINE_DIR)))
GEN ?= assets/generated
ASM_SOURCES ?= xform.S
LIBS ?= -lm
VERSION_SOURCES ?= main.c
VARIANT ?= release
NAME = $(TARGET:.elf=)

GEN_HEADERS = $(wildcard $(GEN)/*.h)
SH4ZAM_HEADERS = $(wildcard third_party/sh4zam/include/sh4zam/*.h \
	third_party/sh4zam/include/sh4zam/inline/*.h \
	third_party/sh4zam/include/sh4zam/inline/sh4/*.h)
HEADERS = $(wildcard src/*.h $(COMMON_DIR)/*.h $(ENGINE_DIR)/*.h) $(GEN_HEADERS) $(SH4ZAM_HEADERS)
# -ffast-math lets GCC use the SH-4's FSCA for sinf/cosf (KOS passes -mfsca
# but libm calls remain without it) and plain FSQRT for sqrtf.
WARN = -std=gnu17 -O2 -ffast-math -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(EXTRA_CFLAGS)
INCLUDES = -Isrc -I$(GEN) -Ibuild -I$(COMMON_DIR) -Ithird_party/sh4zam/include $(EXTRA_INCLUDES)
CFLAGS_ALL = $(WARN) $(VARIANT_FLAGS_$(VARIANT)) $(INCLUDES)

empty :=
space := $(empty) $(empty)
FLAGTAG = $(subst =,-,$(subst $(space),,$(subst -D,+,$(EXTRA_CFLAGS))))
OBJDIR = build/$(VARIANT)$(FLAGTAG)
# A game source named like an engine module would take its object's name
# and quietly replace it; the game's copy has to be renamed.
CLASH := $(foreach s,$(SOURCES),$(if $(and $(wildcard src/$(s)),$(wildcard $(ENGINE_DIR)/$(s))),$(s)))
ifneq ($(strip $(CLASH)),)
$(error $(strip $(CLASH)): both src/ and the engine have this file; rename the game's copy)
endif
OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(SOURCES) $(COMMON_SOURCES)) $(patsubst %.S,$(OBJDIR)/%.o,$(ASM_SOURCES)) \
	$(OBJDIR)/blobs.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules
ifneq ($(strip $(COMMON_SOURCES)),)
include $(COMMON_DIR)/common.mk
endif

$(OBJDIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(CFLAGS_ALL) -c $< -o $@

$(OBJDIR)/%.o: $(COMMON_DIR)/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(CFLAGS_ALL) -c $< -o $@

$(OBJDIR)/%.o: $(ENGINE_DIR)/%.c $(HEADERS)
	@mkdir -p $(@D)
	kos-cc $(CFLAGS_ALL) -c $< -o $@

$(OBJDIR)/%.o: $(ENGINE_DIR)/%.S
	@mkdir -p $(@D)
	kos-cc -c $< -o $@

$(OBJDIR)/blobs.o: src/blobs.S $(BLOBS)
	@mkdir -p $(@D)
	kos-cc $(VARIANT_FLAGS_$(VARIANT)) $(EXTRA_CFLAGS) -c $< -o $@

define link_elf
	kos-cc -o $@ $(OBJS) $(LIBS)
	$(KOS_OBJCOPY) --only-keep-debug $@ $@.debug
	$(KOS_STRIP) -g $@
	$(KOS_OBJCOPY) --add-gnu-debuglink=$@.debug $@
endef

# A build with other EXTRA_CFLAGS has its own objects, but the ELF it links
# is named the same, so switching back would leave the newer ELF in place:
# the stamp changes with the flags and forces the link. macOS's make 3.81
# compares whole seconds, so the stamp waits one out to be newer than an
# ELF linked just before.
LINK_STAMP = build/$(VARIANT).flags
$(LINK_STAMP): force
	@mkdir -p build
	@echo '$(FLAGTAG)' | cmp -s - $@ || { sleep 1; echo '$(FLAGTAG)' > $@; }

# The commit the game was built from, shown on its title screen so bug
# reports can name it: short SHA plus "-dirty" when this directory has
# uncommitted changes; <PREFIX>_GIT_SHA=... overrides it (CI, tarballs).
# Only rewritten when it changes, so a new commit relinks VERSION_SOURCES
# rather than everything.
$(GAME_PREFIX)_GIT_SHA ?= $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD -- . 2>/dev/null || echo -dirty)
VERSION_H = build/version.h
VERSION_LINE = \#define $(GAME_PREFIX)_GIT_SHA "$($(GAME_PREFIX)_GIT_SHA)"
$(VERSION_H): force
	@mkdir -p build
	@echo '$(VERSION_LINE)' | cmp -s - $@ || echo '$(VERSION_LINE)' > $@
$(patsubst %.c,$(OBJDIR)/%.o,$(VERSION_SOURCES)): $(VERSION_H)

$(TARGET): $(OBJS) $(LINK_STAMP)
	$(link_elf)

$(NAME)-$(VARIANT).elf: $(OBJS) $(LINK_STAMP)
	$(link_elf)

clean:
	-rm -rf build $(NAME).elf $(NAME)-*.elf $(NAME)*.elf.debug $(NAME).cdi

.PHONY: all clean force
