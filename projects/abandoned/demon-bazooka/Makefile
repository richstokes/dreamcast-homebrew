TARGET = demon-bazooka.elf
OBJS = demon-bazooka.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean:
	-rm -f $(OBJS) $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) -lm

run: $(TARGET)
	./run-flycast.sh --skip-build

.PHONY: all clean run
