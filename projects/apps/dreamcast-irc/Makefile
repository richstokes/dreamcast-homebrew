TARGET = dreamcast-irc.elf
OBJS = dreamcast-irc.o modem.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET)

dreamcast-irc.o modem.o: modem.h

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lppp

run: $(TARGET)
	./run-flycast.sh --skip-build

.PHONY: all clean run
