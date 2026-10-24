TARGET = ping-cube.elf
OBJS = ping-cube.o vmu-display.o dialup.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules
include ../../common/common.mk

ping-cube.o vmu-display.o: ping-cube.h
ping-cube.o: $(COMMON_DIR)/dialup.h

clean:
	-rm -f $(OBJS) $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lppp

run: $(TARGET)
	./run-flycast.sh --skip-build

.PHONY: all clean run
