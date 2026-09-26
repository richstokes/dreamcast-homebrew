# Fetch before the inner make reads header dependencies and timestamps. An
# order-only prerequisite would let incremental/parallel builds use stale headers.
#
# third_party/sh4zam is an ignored, unpinned clone of upstream SH4ZAM master;
# tools/update-sh4zam.sh creates it or fast-forwards it to the current head.
.DEFAULT_GOAL := all
SH4ZAM_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
SH4ZAM_UPDATE := $(dir $(lastword $(MAKEFILE_LIST)))update-sh4zam.sh

.PHONY: sh4zam-build $(SH4ZAM_GOALS)
$(SH4ZAM_GOALS): sh4zam-build ;

sh4zam-build:
ifneq ($(filter-out clean,$(SH4ZAM_GOALS)),)
	$(SH4ZAM_UPDATE) third_party/sh4zam
endif
	+$(MAKE) SH4ZAM_UPDATED=1 $(MAKECMDGOALS)
