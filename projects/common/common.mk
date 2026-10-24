# Shared code in projects/common, compiled into each project with that
# project's own flags. A project on KOS's default rules lists the objects it
# wants in OBJS and includes this after Makefile.rules:
#
#   OBJS = main.o dialup.o
#   include $(KOS_BASE)/Makefile.rules
#   include ../../common/common.mk
#
# make then finds dialup.c here and builds dialup.o in the project directory.
# Projects with their own object directories compile $(COMMON_DIR)/<name>.c
# with their own rule. Sources include the headers by relative path, as with
# softreset.h.
COMMON_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
COMMON_MODULES := dialup

$(foreach module,$(COMMON_MODULES),$(eval vpath $(module).c $(COMMON_DIR)))
$(COMMON_MODULES:%=%.o): %.o: $(COMMON_DIR)/%.h
