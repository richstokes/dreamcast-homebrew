/* The level blob, read from assets/generated/level.bin at start-up. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

uint8_t rl_level_bin[900000] __attribute__((aligned(32)));

__attribute__((constructor)) static void load_level(void) {
    FILE *f = fopen(LEVEL_BIN_FILE, "rb");
    if(!f) {
        perror(LEVEL_BIN_FILE);
        exit(1);
    }
    fread(rl_level_bin, 1, sizeof(rl_level_bin), f);
    fclose(f);
}
