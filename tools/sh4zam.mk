# Fetch before the inner make reads header dependencies and timestamps. An
# order-only prerequisite would let incremental/parallel builds use stale headers.
.DEFAULT_GOAL := all
SH4ZAM_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)

.PHONY: sh4zam-build $(SH4ZAM_GOALS)
$(SH4ZAM_GOALS): sh4zam-build ;

sh4zam-build:
ifneq ($(filter-out clean,$(SH4ZAM_GOALS)),)
	git submodule update --init --remote --checkout -- third_party/sh4zam
endif
	+$(MAKE) SH4ZAM_UPDATED=1 $(MAKECMDGOALS)
