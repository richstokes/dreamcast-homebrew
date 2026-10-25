/* Procedurally drawn icons (see icons.c). */
#ifndef LEMONPLAY_ICONS_H
#define LEMONPLAY_ICONS_H

#include <stdint.h>

enum {
    ICON_LEMON,     /* 64x64, in colour */
    ICON_FOLDER,    /* the rest are 24x24 white masks, tinted when drawn */
    ICON_FILM,
    ICON_NOTE,
    ICON_SERVER,
    ICON_PLAY,
    ICON_PAUSE,
    ICON_DISC,
    ICON_SEARCH,
    ICON_PLUS,
    ICON_WARNING,
    ICON_COUNT
};

/* Where an icon sits in the atlas. */
void icons_cell(int icon, int *x, int *y, int *size);
/* Draws every icon into an ARGB4444 atlas (at least 256x64). */
void icons_draw(uint16_t *pixels, int stride);

#endif
