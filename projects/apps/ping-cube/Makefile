TARGET = ping-cube.elf
OBJS = ping-cube.o modem.o vmu-display.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

ping-cube.o modem.o vmu-display.o: ping-cube.h

clean:
	-rm -f $(OBJS) $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lppp

run: $(TARGET)
	./run-flycast.sh --skip-build

.PHONY: all clean run
