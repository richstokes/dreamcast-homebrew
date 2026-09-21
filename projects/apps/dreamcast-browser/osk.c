/* On-screen keyboard layout and geometry, shared by input and rendering so
   the controller, mouse hit-testing, and drawing always agree. */

#include "browser.h"

#include <stddef.h>

#define OSK_CHAR_COLUMNS 12
#define OSK_ACTION_COLUMNS 6
#define OSK_LEFT 8
#define OSK_ROW_PITCH 32
#define OSK_KEY_HEIGHT 30

/* Every printable ASCII character is reachable across the two layers. */
static const char *const layers[2][4] = {
    { "1234567890-=", "qwertyuiop/_", "asdfghjkl:;'", "zxcvbnm,.?&#" },
    { "!@#$%^&*()+~", "QWERTYUIOP\\|", "ASDFGHJKL\"[]", "ZXCVBNM<>{}`" }
};

static const struct {
    osk_action_t action;
    const char *label;
    const char *text;
} actions[OSK_ACTION_COLUMNS] = {
    { OSK_SHIFT, "Shift", NULL },
    { OSK_SPACE, "Space", NULL },
    { OSK_BACKSPACE, "Del", NULL },
    { OSK_TEXT, ".com", ".com" },
    { OSK_CANCEL, "Cancel", NULL },
    { OSK_DONE, "Done", NULL }
};

int osk_columns(int row) {
    return row == OSK_ROWS - 1 ? OSK_ACTION_COLUMNS : OSK_CHAR_COLUMNS;
}

void osk_key(int row, int column, int shift, osk_key_t *key) {
    static char labels[2][4][OSK_CHAR_COLUMNS][2];
    if(row < 0 || row >= OSK_ROWS) row = 0;
    if(column < 0 || column >= osk_columns(row)) column = 0;
    if(row == OSK_ROWS - 1) {
        key->action = actions[column].action;
        key->ch = 0;
        key->text = actions[column].text;
        key->label = actions[column].label;
        return;
    }
    shift = shift ? 1 : 0;
    key->action = OSK_CHAR;
    key->ch = layers[shift][row][column];
    key->text = NULL;
    labels[shift][row][column][0] = key->ch;
    labels[shift][row][column][1] = 0;
    key->label = labels[shift][row][column];
}

/* Moves the highlight, wrapping at the edges. Moving between rows of
   different widths keeps the highlight over the same horizontal position. */
void osk_move(int *row, int *column, int row_delta, int column_delta) {
    if(row_delta) {
        int old_columns = osk_columns(*row);
        *row = ((*row + row_delta) % OSK_ROWS + OSK_ROWS) % OSK_ROWS;
        *column = (*column * 2 + 1) * osk_columns(*row) / (old_columns * 2);
    }
    if(column_delta) {
        int columns = osk_columns(*row);
        *column = ((*column + column_delta) % columns + columns) % columns;
    }
}

void osk_key_rect(int row, int column, int *x, int *y, int *w, int *h) {
    int pitch = (SCREEN_W - OSK_LEFT * 2) / osk_columns(row);
    *x = OSK_LEFT + column * pitch;
    *y = OSK_TOP + 4 + row * OSK_ROW_PITCH;
    *w = pitch - 2;
    *h = OSK_KEY_HEIGHT;
}

int osk_hit(int x, int y, int *row, int *column) {
    int r, c;
    for(r = 0; r < OSK_ROWS; ++r) {
        for(c = 0; c < osk_columns(r); ++c) {
            int kx, ky, kw, kh;
            osk_key_rect(r, c, &kx, &ky, &kw, &kh);
            if(x >= kx && x < kx + kw && y >= ky && y < ky + kh) {
                *row = r;
                *column = c;
                return 1;
            }
        }
    }
    return 0;
}
