/* Synthetic transport regression. No VMU reads/writes or real account needed. */
#include <kos.h>
#include <mbedtls/sha256.h>
#include "client.h"

#ifndef DCVMU_STRESS_COUNT
#define DCVMU_STRESS_COUNT 30
#endif

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

void client_status(const char *message) {
    printf("STATUS: %s\n", message);
}

int client_connect_update(const char *message) {
    client_status(message);
    return 0;
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

    for(int i = 0; i < DCVMU_STRESS_COUNT; ++i) {
        int revision = 0;
        size_t size = sizes[i % 3];
        printf("UPLOAD STRESS BEGIN %d size=%u\n", i, (unsigned)size);
        if(service_upload("synthetic-test-token", "stress", "DCVMU_TEST",
                          "Test", "", 1, data, size, 0, "ask", 0, &revision))
            break;
        ++passed;
    }
    int downloaded=0;
    if(passed==DCVMU_STRESS_COUNT) {
        remote_save_t item={.id=1,.revision=1,.size=sizeof(data)};
        unsigned char digest[32];void *copy=NULL;
        if(mbedtls_sha256(data,sizeof(data),digest,0)==0) {
            for(int i=0;i<32;++i)snprintf(item.sha256+i*2,3,"%02x",digest[i]);
            if(service_download("synthetic-test-token",&item,&copy)==0) {
                downloaded=!memcmp(copy,data,sizeof(data));free(copy);
            }
        }
        printf("LARGE DOWNLOAD %s\n",downloaded?"PASSED":"FAILED");
    }
    service_net_shutdown();
    printf("UPLOAD STRESS %s: %d/%d (network shut down)\n",
           passed==DCVMU_STRESS_COUNT && downloaded?"PASSED":"FAILED",passed,DCVMU_STRESS_COUNT);
    /* The host runner stops this isolated emulator after reading the result. */
    while(1)
        thd_sleep(1000);
}
