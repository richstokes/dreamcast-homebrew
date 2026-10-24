# Builds every project in the release manifest under one jobserver, so the
# CPUs stay busy across projects instead of idling inside the small ones.
# build-in-container.sh runs it with -j and -Orecurse, which prints each
# project's log in one piece once that project finishes.

manifest := $(WORKSPACE)/.github/console-projects.txt
projects := $(shell cut -d'|' -f1 $(manifest))

# PICO-8 Player takes ~23 s on its own, about a third of all the project
# builds, so it starts first rather than holding up the end of the run.
first := $(filter projects/apps/pico8-dreamcast,$(projects))

all: $(first) $(filter-out $(first),$(projects))

# CI already fetched current SH4ZAM on the host before entering Docker.
$(projects):
	+$(MAKE) -C $(WORKSPACE)/$@ clean && $(MAKE) -C $(WORKSPACE)/$@ SH4ZAM_UPDATED=1

.PHONY: all $(projects)
