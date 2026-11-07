/* DreamKarts' colours for the engine's menu chrome (engine/ui.h): navy
   outlines, bright blue panels, yellow plaques. Lettering is white with the
   font's own navy outline wherever it sits on yellow: navy lettering there
   merged with the outline and read as a smudge. */
#include "ui.h"
#include "hud.h"

/* argb() as a constant expression. */
#define C(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define NAVY C(255, 14, 24, 64)

static const EngUiTheme theme = {
    .roles = {
        [UI_TITLE] = C(255, 255, 255, 255),    [UI_ITEM] = C(255, 240, 246, 255),     [UI_ITEM_SEL] = C(255, 255, 255, 255),
        [UI_LABEL] = C(255, 196, 226, 255),    [UI_VALUE] = C(255, 255, 255, 255),    [UI_VALUE_SEL] = C(255, 255, 255, 255),
        [UI_DIM] = C(240, 200, 222, 250),      [UI_TEXT] = C(255, 255, 255, 255),     [UI_HINT] = C(255, 255, 255, 255),
        [UI_ACCENT] = C(255, 255, 222, 70),    [UI_GOOD] = C(255, 120, 255, 150),     [UI_WARN] = C(255, 255, 120, 110),
    },
    .outline = NAVY,
    .shadow = C(110, 0, 10, 40),
    .panel_top = C(240, 64, 150, 255), .panel_bottom = C(240, 22, 76, 210),
    .well_top = C(255, 14, 40, 110), .well_bottom = C(255, 10, 30, 90),
    .plaque_top = C(255, 255, 228, 100), .plaque_bottom = C(255, 255, 176, 40), .plaque_glow = C(255, 255, 238, 130),
    .button_top = C(255, 60, 130, 235), .button_bottom = C(255, 30, 90, 200),
    .hint_top = C(255, 30, 60, 150), .hint_bottom = C(255, 16, 36, 110),
    .dim = C(130, 0, 8, 36),
};

void dk_ui_init(void) {
    ui_init(hud_font(), &theme);
}
