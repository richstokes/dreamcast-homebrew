TARGET = dreamcast-irc.elf
OBJS = dreamcast-irc.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS)

run: $(TARGET)
	./run-flycast.sh --skip-build

.PHONY: all clean run
