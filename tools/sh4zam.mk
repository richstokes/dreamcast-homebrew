# Check out before the inner make reads header dependencies and timestamps. An
# order-only prerequisite would let incremental/parallel builds use stale headers.
#
# third_party/sh4zam is an ignored clone of upstream SH4ZAM;
# tools/update-sh4zam.sh creates it or moves it to the pinned commit.
.DEFAULT_GOAL := all
SH4ZAM_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
SH4ZAM_UPDATE := $(dir $(lastword $(MAKEFILE_LIST)))update-sh4zam.sh

# SH4ZAM checks alignment, sizes and indices with assert(), and upstream keeps
# adding debug-only checks, so builds define NDEBUG as upstream asks for
# release builds. DEBUG=1 keeps assert() for tracking down API misuse. This
# covers the whole project, and make does not rebuild objects when flags
# change: run make clean after switching.
ifneq ($(DEBUG),1)
export KOS_CFLAGS += -DNDEBUG
endif

.PHONY: sh4zam-build $(SH4ZAM_GOALS)
$(SH4ZAM_GOALS): sh4zam-build ;

sh4zam-build:
ifneq ($(filter-out clean,$(SH4ZAM_GOALS)),)
	$(SH4ZAM_UPDATE) third_party/sh4zam
endif
	+$(MAKE) SH4ZAM_UPDATED=1 $(MAKECMDGOALS)
