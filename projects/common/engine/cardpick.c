/* Engine: the memory card prompt (cardpick.h). */
#include "cardpick.h"
#include "input.h"
#include <stdio.h>
#include <string.h>

#define NOTE_LEN 20
#define ROW_H 36.0f

static struct {
    bool up;
    int steps;                           /* since it opened */
    int kind, need, rows, sel;
    int full;                            /* the slot that had no room (CARDPICK_ROOM) */
    int8_t row_slot[VMUSAVE_SLOTS + 1];  /* each row's slot, or CARDPICK_NO_SAVE */
    vmusave_slot_t slots[VMUSAVE_SLOTS];
    char notes[VMUSAVE_SLOTS][NOTE_LEN];
} cp;

void cardpick_open(int kind, const vmusave_slot_t *slots, const char *const *notes, int count, int need) {
    memset(&cp, 0, sizeof(cp));
    cp.up = true;
    cp.kind = kind;
    cp.need = need;
    cp.full = -1;
    if(count > VMUSAVE_SLOTS) count = VMUSAVE_SLOTS;
    for(int i = 0; i < count; i++) {
        cp.slots[i] = slots[i];
        cp.slots[i].file = NULL;
        if(notes && notes[i]) snprintf(cp.notes[i], NOTE_LEN, "%s", notes[i]);
        if(slots[i].free_blocks < 0) continue;
        if(kind == CARDPICK_ROOM && cp.full < 0 && !vmusave_fits(&slots[i], need)) cp.full = i;
        if(kind == CARDPICK_WHICH ? slots[i].save : vmusave_fits(&slots[i], need)) cp.row_slot[cp.rows++] = (int8_t)i;
    }
    /* (never without a row: a question with no cards to show is that one) */
    if(kind != CARDPICK_WHICH || !cp.rows) cp.row_slot[cp.rows++] = CARDPICK_NO_SAVE;
}

bool cardpick_up(void) {
    return cp.up;
}

int cardpick_update(void) {
    if(!cp.up) return CARDPICK_WAIT;
    /* A press meant for the screen underneath, or one of a player hammering
       through the title, must not answer it. */
    if(++cp.steps < 20) return CARDPICK_WAIT;
    if(pad.pressed & (BTN_A | BTN_START)) {
        cp.up = false;
        return cp.row_slot[cp.sel];
    }
    int d = input_menu_down() ? 1 : input_menu_up() ? -1 : 0;
    if(!d || cp.rows < 2) return CARDPICK_WAIT;
    cp.sel = (cp.sel + cp.rows + d) % cp.rows;
    return CARDPICK_MOVED;
}

/* "A1": the port's letter and the card's place on the controller. */
static const char *slot_name(const vmusave_slot_t *s) {
    static char name[8];
    snprintf(name, sizeof(name), "%c%d", 'A' + s->port, s->unit);
    return name;
}

void cardpick_draw(void) {
    if(!cp.up) return;
    const EngFont *font = ui_font();
    const float x0 = 96, x1 = 544, h = 64 + cp.rows * ROW_H, y0 = 256 - h * 0.5f;
    char line[64];
    ui_dim();
    ui_panel(x0, y0, x1, y0 + h);
    float y = ui_title(x0, y0, x1, cp.kind == CARDPICK_ROOM ? "MEMORY CARD FULL" : "MEMORY CARD");
    if(cp.kind == CARDPICK_WHICH) snprintf(line, sizeof(line), "MORE THAN ONE CARD HAS A SAVE. USE WHICH?");
    else if(cp.kind == CARDPICK_WHERE) snprintf(line, sizeof(line), "THE SAVE HAS NO CARD TO GO TO. SAVE WHERE?");
    else if(cp.full >= 0)
        snprintf(line, sizeof(line), "THE SAVE NEEDS %d BLOCKS. CARD %s HAS %d.", cp.need, slot_name(&cp.slots[cp.full]),
                 cp.slots[cp.full].free_blocks);
    else snprintf(line, sizeof(line), "THE SAVE NEEDS %d BLOCKS.", cp.need);
    text_draw_c(font, 320, y + 6, 0.5f, ALIGN_CENTER, ui_col(UI_TEXT), line);
    y += 44;
    for(int i = 0; i < cp.rows; i++, y += ROW_H) {
        bool on = i == cp.sel;
        uint32_t c = ui_col(on ? UI_ITEM_SEL : UI_ITEM);
        ui_row(x0 + 14, y - 15, x1 - 14, y + 15, on);
        if(cp.row_slot[i] < 0) {
            text_draw_c(font, 320, y, 0.62f, ALIGN_CENTER, c, "PLAY WITHOUT SAVING");
            continue;
        }
        const vmusave_slot_t *s = &cp.slots[cp.row_slot[i]];
        bool which = cp.kind == CARDPICK_WHICH;
        const char *note = cp.notes[cp.row_slot[i]];
        snprintf(line, sizeof(line), which ? "CARD %s" : "SAVE TO CARD %s", slot_name(s));
        text_draw_c(font, x0 + 34, y, 0.62f, ALIGN_LEFT, c, line);
        if(which && note[0]) text_draw_c(font, 320, y, 0.62f, ALIGN_CENTER, c, note);
        /* Its room; or, choosing where to write, whose save that would
           be written over. */
        if(!which && s->save) snprintf(line, sizeof(line), "OVER %s", note[0] ? note : "A SAVE");
        else snprintf(line, sizeof(line), "%d FREE", s->free_blocks);
        text_draw_c(font, x1 - 34, y, 0.5f, ALIGN_RIGHT, on ? c : ui_col(UI_DIM), line);
    }
    ui_hint("UP/DOWN: CHOOSE   A: OK");
}
