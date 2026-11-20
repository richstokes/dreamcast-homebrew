/* The HUD and the screens: clock, bill, combo, breath, threat; the title,
   pause and results. */
#ifndef KL_HUD_H
#define KL_HUD_H

#include "kl.h"
#include "engine/text.h"

void hud_init(void);
const EngFont *hud_font(void);
void hud_draw(void);
void hud_draw_title(void);
void hud_draw_pause(void);
void hud_draw_results(void);

#endif
