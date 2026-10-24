TARGET = dcvmu-client.elf
OBJS = dcvmu-client.o title.o menu-audio.o net.o modem.o dialup.o auth.o archive.o romdisk.o
KOS_BUILD_SUBARCHS = pristine
KOS_ROMDISK_DIR = romdisk
KOS_CSTD = -std=gnu17
LDLIBS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lm -lpthread -lppp
all: $(TARGET)
include $(KOS_BASE)/Makefile.rules
include ../../common/common.mk
clean:
	-rm -f $(OBJS) $(TARGET) romdisk.img
dcvmu-client.o title.o menu-audio.o net.o modem.o auth.o archive.o: client.h
modem.o: $(COMMON_DIR)/dialup.h
auth.o: $(COMMON_DIR)/vmusave.h
$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS) $(LDLIBS)
run: $(TARGET)
	./run-flycast.sh --skip-build
.PHONY: all clean run

# A normal build must never package temporary integration credentials.
.PHONY: release-check
ifeq ($(filter -DDCVMU_SELF_TEST -DDCVMU_DOWNLOAD_TEST,$(CPPFLAGS)),)
release-check:
	@test ! -f romdisk/test-credentials.txt && test ! -f romdisk/test-service.txt && test ! -f romdisk/test-ca.pem || { echo "Remove romdisk/test-credentials.txt before a normal build."; exit 1; }
else
release-check:
endif
$(OBJS): | release-check
