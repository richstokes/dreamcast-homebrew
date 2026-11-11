/* The areas' packed grids, read from assets/generated at start-up (the
   game links them in), so a rebuilt level needs no rebuild here unless
   its header changed too. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

uint8_t rl_level_cascade_lvl[1 << 20], rl_level_moab_lvl[1 << 20], rl_level_dunes_lvl[1 << 20];

static void load(const char *path, uint8_t *to) {
    FILE *f = fopen(path, "rb");
    if(!f) {
        perror(path);
        exit(1);
    }
    fread(to, 1, 1 << 20, f);
    fclose(f);
}

__attribute__((constructor)) static void load_levels(void) {
    load(LEVEL_DIR "/level_cascade.lvl", rl_level_cascade_lvl);
    load(LEVEL_DIR "/level_moab.lvl", rl_level_moab_lvl);
    load(LEVEL_DIR "/level_dunes.lvl", rl_level_dunes_lvl);
}
