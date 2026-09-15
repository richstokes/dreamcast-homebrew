/* Synthetic transport regression. No VMU reads/writes or real account needed. */
#include <kos.h>
#include "client.h"

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

void client_status(const char *message) {
    printf("STATUS: %s\n", message);
}

int auth_is_save(const char *name, const void *data, size_t size) {
    (void)name; (void)data; (void)size;
    return 0;
}

int transfer_update(uint64_t done, uint64_t total) {
    (void)done; (void)total;
    thd_sleep(1);
    return 0;
}

int main(void) {
    static unsigned char data[98816];
    const size_t sizes[] = {4608, 8704, sizeof(data)};
    int passed = 0;

    if(service_net_init()) {
        printf("UPLOAD STRESS FAILED: network initialization\n");
        return 1;
    }
    for(unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (i * 31 + 7) & 255;

    for(int i = 0; i < 30; ++i) {
        int revision = 0;
        size_t size = sizes[i % 3];
        printf("UPLOAD STRESS BEGIN %d size=%u\n", i, (unsigned)size);
        if(service_upload("synthetic-test-token", "stress", "DCVMU_TEST",
                          "Test", "", 1, data, size, 0, "ask", &revision))
            break;
        ++passed;
    }
    printf("UPLOAD STRESS %s: %d/30\n", passed == 30 ? "PASSED" : "FAILED", passed);
    service_net_shutdown();
    /* The host runner stops this isolated emulator after reading the result. */
    while(1)
        thd_sleep(1000);
}
