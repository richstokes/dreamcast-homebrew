# Console microbenchmark for src/xform.S (see micro.c).
TARGET = micro.elf
OBJS = micro.o xform.o

KOS_BUILD_SUBARCHS = pristine

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

xform.o: ../../src/xform.S
	kos-cc -c $< -o $@

micro.o: micro.c
	kos-cc -O2 -c $< -o $@

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS)

DC_HW = ../../../../../tools/dc-hw.sh
run: $(TARGET)
	$(DC_HW) run -c -o -T 120 -u 'MICRO: done' $(TARGET)

clean:
	-rm -f $(OBJS) $(TARGET)

.PHONY: all clean run
