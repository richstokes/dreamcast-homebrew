/* Engine: menu chrome. Rounded, outlined panels with heading plaques and
   selection pills, control hints with controller button glyphs, on-screen
   keyboard keys, page dots and a dim wash. All of it is flat 2D quads and
   fans under the translucent list (no texture beyond the font), so it
   costs no VRAM. The game picks the colours with a theme; the shapes are
   the same in every game. */
#ifndef ENG_UI_H
#define ENG_UI_H

#include "text.h"

/* Text colour roles, so every screen agrees. */
enum {
    UI_TITLE,      /* panel headings, on their plaque */
    UI_ITEM,       /* a menu item at rest */
    UI_ITEM_SEL,   /* the selected item, on its pill */
    UI_LABEL,      /* a row's label (MODE, LAPS) */
    UI_VALUE,      /* a row's value at rest */
    UI_VALUE_SEL,  /* the selected row's value */
    UI_DIM,        /* secondary text: blurbs, column headings */
    UI_TEXT,       /* body text */
    UI_HINT,       /* the controls strip */
    UI_ACCENT,     /* gold: points, track names */
    UI_GOOD,       /* green: READY!, gains */
    UI_WARN,       /* red: errors, CONNECT CONTROLLER */
    UI_ROLES
};

typedef struct {
    uint32_t roles[UI_ROLES];
    uint32_t outline;                        /* round every shape */
    uint32_t shadow;                         /* under panels and headings */
    uint32_t panel_top, panel_bottom;        /* ui_panel's body gradient */
    uint32_t well_top, well_bottom;          /* ui_well */
    uint32_t plaque_top, plaque_bottom;      /* headings and the selected row's pill */
    uint32_t plaque_glow;                    /* plaque_top at the top of its breathing */
    uint32_t button_top, button_bottom;      /* ui_button and ui_key at rest */
    uint32_t hint_top, hint_bottom;          /* the controls strip's pill */
    uint32_t dim;                            /* ui_dim's wash */
} EngUiTheme;

/* The font the chrome letters with, and its colours. */
void ui_init(const EngFont *font, const EngUiTheme *theme);
uint32_t ui_col(int role);
/* The clock the breathing pill and bobbing arrows run on; the game sets
   it each frame (seconds on the current screen, say). */
extern float ui_time;

/* A menu panel. */
void ui_panel(float x0, float y0, float x1, float y1);
/* An inset area inside a panel (the track map, a name box). */
void ui_well(float x0, float y0, float x1, float y1);
/* The heading plaque straddling the top edge (y0) of a panel; returns the
   y just below it, where the panel's content starts. */
float ui_title(float x0, float y0, float x1, const char *text);
/* A free-standing heading plaque centred on (cx, cy): SELECT TRACK. */
void ui_heading(float cx, float cy, const char *text);
/* The pill behind a selected row (nothing when not selected). */
void ui_row(float x0, float y0, float x1, float y1, bool sel);
/* A button-like row (NEXT, SAVE): its own pill and lettering. */
void ui_button(float cx, float cy, float w, const char *text, bool sel);
/* Bobbing < > either side of a value, cap-centred on cy. */
void ui_arrows(float xl, float xr, float cy, float scale, uint32_t col);
/* The controls strip along the bottom of the screen: "A: OK   B: BACK"
   drawn with controller button glyphs for A, B, X, Y, START, LEFT/RIGHT,
   UP/DOWN and L+R. */
void ui_hint(const char *text);
/* The same line centred elsewhere (a split-screen half). */
void ui_hint_at(float cx, float cy, const char *text);
/* A controller button glyph: key is "A", "B", "X", "Y", "START", "LEFT/RIGHT",
   "UP/DOWN" or "L+R". Returns its width. */
float ui_glyph(float x, float cy, const char *key, bool draw);
/* An on-screen keyboard key (name entry, chat). */
void ui_key(float x0, float y0, float x1, float y1, bool sel);
/* A two-tone colour swatch (kart paint, flat colour), w x h. */
void ui_swatch(float x, float y, float w, float h, uint32_t a, uint32_t b);
/* Page dots: n of them centred on cx, the current one bigger. */
void ui_dots(float cx, float cy, int n, int cur);
/* The dark wash behind a pause or prompt. */
void ui_dim(void);
/* Vertex-colour shapes the screens share. */
void ui_rrect(float x0, float y0, float x1, float y1, float r, uint32_t top, uint32_t bottom);
void ui_circle(float cx, float cy, float r, uint32_t col);
void ui_pill(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom);

#endif
