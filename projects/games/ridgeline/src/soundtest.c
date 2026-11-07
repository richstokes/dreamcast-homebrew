/* Ridgeline sound test: every effect in the pak by name. A plays a
   one-shot, or starts and stops a loop; left and right change the pitch,
   so an engine loop can be heard across its rpm range. */
#include "menu.h"
#include "hud.h"
#include "camera.h"
#include "audio.h"
#include "engine/sfx.h"
#include "sound_assets.h"

#define ROWS 11

static int sel, top;
static float rate = 1.0f;
static SfxVoice loop_v = {.channel = -1};
static int loop_id = -1;

void soundtest_stop(void) {
    sfx_voice_stop(&loop_v);
    loop_id = -1;
}

static void play(int id) {
    if(!rl_sfx_assets[id].loop) {
        sfx_play(id, 220, 128, rate);
        return;
    }
    bool same = loop_id == id;
    soundtest_stop();
    if(same) return;
    if(sfx_voice_start(&loop_v, id)) {
        loop_id = id;
        sfx_voice_set(&loop_v, rate, 215, 128);
    }
}

void soundtest_update(float dt) {
    camera_flyover(game.time + 40.0f);
    if(input_menu_up()) sel = (sel + RL_SFX_COUNT - 1) % RL_SFX_COUNT;
    if(input_menu_down()) sel = (sel + 1) % RL_SFX_COUNT;
    if(sel < top) top = sel;
    if(sel >= top + ROWS) top = sel - ROWS + 1;
    int d = input_menu_right() ? 1 : input_menu_left() ? -1 : 0;
    if(d) {
        rate = clampf(rate + 0.05f * d, 0.5f, 2.0f);
        if(loop_id >= 0) sfx_voice_set(&loop_v, rate, 215, 128);
    }
    if(pad.pressed & BTN_Y) {
        rate = 1.0f;
        if(loop_id >= 0) sfx_voice_set(&loop_v, rate, 215, 128);
    }
    if(pad.pressed & BTN_A) play(sel);
    if(pad.pressed & BTN_B) {
        audio_sfx(RL_SFX_MENU_BACK, 200);
        game_set_state(ST_TITLE);
    }
}

void soundtest_draw(void) {
    char buf[48];
    hud_text_c(320, 44, 0.9f, ALIGN_CENTER, 0xfff8f0dc, "SOUND TEST");
    hud_panel(110, 70, 530, 82 + ROWS * 28, 0xc0182028);
    for(int r = 0; r < ROWS && top + r < RL_SFX_COUNT; r++) {
        int id = top + r;
        float y = 90 + r * 28;
        bool on = id == sel, playing = id == loop_id;
        if(on) hud_panel(114, y - 13, 526, y + 13, 0xd0e8a030);
        uint32_t c = on ? 0xff201810 : playing ? 0xffffd070 : 0xfff4ecd8;
        snprintf(buf, sizeof(buf), "%02d", id + 1);
        hud_text_c(126, y, 0.55f, ALIGN_LEFT, on ? 0xff403020 : 0xff988f80, buf);
        hud_text_c(164, y, 0.6f, ALIGN_LEFT, c, rl_sfx_names[id]);
        const rl_sfx_asset_t *a = &rl_sfx_assets[id];
        if(playing) snprintf(buf, sizeof(buf), "PLAYING");
        else if(a->loop) snprintf(buf, sizeof(buf), "LOOP");
        else snprintf(buf, sizeof(buf), "%.2f S", (double)(a->bytes / 2.0f / (float)a->rate));
        hud_text_c(514, y, 0.5f, ALIGN_RIGHT, on ? 0xff403020 : 0xffb8b0a0, buf);
    }
    snprintf(buf, sizeof(buf), "PITCH %.2f", (double)rate);
    hud_text_c(320, 412, 0.62f, ALIGN_CENTER, 0xfff8f0dc, buf);
    hud_text_c(320, 444, 0.5f, ALIGN_CENTER, 0xffd8d0c0, "A: PLAY / STOP    LEFT/RIGHT: PITCH    Y: RESET    B: BACK");
}
