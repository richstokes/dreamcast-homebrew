/* The bot and the test builds (autotest.h). */
#include "autotest.h"
#include "city.h"
#include "kaiju.h"
#include "score.h"
#include "engine/qa.h"
#include "engine/frame.h"
#include "engine/prof.h"
#include "save.h"
#include "audio.h"
#include "life.h"

void bot_ctl(Ctl *c, float dt) {
    static float t, think;
    static int target = -1;
    memset(c, 0, sizeof(*c));
    t += dt;
    think -= dt;
    if(target < 0 || target >= city_nb || city_b[target].state != B_INTACT || think <= 0) {
        target = city_nearest(kj.pos, 8);
        think = 5;
    }
    if(target < 0) return;
    const Building *b = &city_b[target];
    float dx = b->x - kj.pos.x, dz = b->z - kj.pos.z, d = sqrtf(dx * dx + dz * dz) + 0.01f;
    float reach = fmaxf(b->hw, b->hd) + 18.0f;
    c->mx = dx / d;
    c->mz = dz / d;
    c->dash = d > reach + 70.0f;    /* a long way: charge at it */
    if(d > reach + 8.0f) return;
    float ph = fmodf(t, 6.5f);
    if(ph < 2.4f) c->claw = true;
    else if(ph < 3.0f) c->tail = true;
    else if(ph < 3.6f) c->jump = true;
    else if(ph < 4.2f) c->chomp = true;
    else if(ph < 5.8f) c->breath = true;
    else c->roar = true;
    /* The jaws stay shut on what they caught every other time round, and
       it is thrown as the breath would start. */
    c->grab = ph >= 3.6f && ph < 4.6f && ((int)(t / 6.5f) & 1);
    if(d < reach - 5.0f) {
        c->mx *= 0.2f;
        c->mz *= 0.2f;
    }
}

#ifdef KL_AUTOTEST
#ifndef KL_AT_SECONDS
#define KL_AT_SECONDS 45
#endif
static QaWindow win;
static float clock_s, log_at = 5;
static int shot;
static bool started, done;

void autotest_boot(void) {
#ifdef KL_AT_VMUTEST
    /* The save's round trip: nothing on the card, a record written, then
       read back by a game that has forgotten it. */
    bool ok = save_wait() && save.best_total == 0;
    save_submit(123456789ull, 42, 3);
    save_forget();
    save_cards();
    ok = ok && save_wait() && save.best_total == 123456789ull && save.best_mult == 42 && save.grade == 3 && save.lunches == 1;
    printf(ok ? "KL AUTOTEST: PASS (card)\n" : "KL AUTOTEST: FAIL (card: total=%llu mult=%lu)\n", (unsigned long long)save.best_total,
           (unsigned long)save.best_mult);
    done = true;
    return;
#endif
    printf("KL AUTOTEST: %d s of lunch\n", KL_AT_SECONDS);
}

void autotest_frame(float dt_real) {
    if(done) return;
    clock_s += dt_real;
#ifdef KL_CAPTURE
    /* The title first. */
    if(!started && clock_s > 2.5f && shot == 0) {
        qa_shot("title");
        shot++;
    }
    if(!started && clock_s < 3.2f) return;
#endif
    if(!started) {
        started = true;
        game_start();
        game.clock = (float)KL_AT_SECONDS;
        clock_s = 0;
        qa_window_reset(&win);
        return;
    }
    qa_window_frame(&win, dt_real, eng_frame.steps, eng_frame.cpu_ms);
#ifdef KL_CAPTURE
    /* The pause menu, on its SHOW FPS row with the readout on behind it. */
    static int pause_shot;
    if(game.state == ST_PLAY && pause_shot == 0 && clock_s >= 6.0f) {
        game.paused = game.show_fps = true;
        game.pause_sel = PAUSE_FPS;
        pause_shot = 1;
    }
    else if(pause_shot == 1 && clock_s >= 6.7f) {
        qa_shot("pause");
        pause_shot = 2;
    }
    else if(pause_shot == 2 && clock_s >= 7.4f) {
        game.paused = false;
        pause_shot = 3;
    }
    else if(pause_shot == 3 && clock_s >= 9.8f) {
        /* (left on for the shot at 9 s, to show the readout) */
        game.show_fps = false;
        pause_shot = 4;
    }
    static const float at[] = {1.2f, 5, 9, 13, 17, 22, 27, 32, 37, 42};
    int k = shot - 1;
    if(game.state == ST_PLAY && k >= 0 && k < (int)(sizeof(at) / sizeof(at[0])) && clock_s >= at[k]) {
        qa_shot("play-%02d", k);
        shot++;
    }
#endif
    if(clock_s >= log_at) {
        log_at += 5;
        printf("KL AT: t=%5.1f frames=%d missed=%d cpu=%.2f/%.2f ms pvr=%.2f/%.2f ms vtx=%lu KiB tris=%d standing=%d/%d total=%llu mult=%d best=%d damage=%llu threat=%d\n",
               (double)clock_s, win.frames, win.missed, (double)(win.cpu_sum / (float)(win.frames ? win.frames : 1)), (double)win.cpu_max,
               (double)(win.rnd_sum / (float)(win.frames ? win.frames : 1)), (double)win.rnd_max, (unsigned long)(win.vtx_max / 1024),
               rstats.tris, city_standing(), city_total(), (unsigned long long)score.total, score.mult, score.best_mult, (unsigned long long)score.damage, life_threat());
        float fr = (float)(prof_phase_frames ? prof_phase_frames : 1) * 1000.0f;
        printf("KL AT:   update=%.2f city=%.2f life=%.2f kaiju=%.2f fx=%.2f tr=%.2f ms\n", (double)((float)prof_phase_us[PH_UPD] / fr),
               (double)((float)prof_phase_us[PH_CITY] / fr), (double)((float)prof_phase_us[PH_LIFE] / fr),
               (double)((float)prof_phase_us[PH_KAIJU] / fr), (double)((float)prof_phase_us[PH_FX] / fr),
               (double)((float)prof_phase_us[PH_TR] / fr));
        /* The music: on a disc, the song in the drive, its status (3 is
           playing in Flycast) and the CD volume register. */
        int cd_status, cd_volume, cd_song;
        audio_music_state(&cd_status, &cd_volume, &cd_song);
        printf("KL AT:   music: song %d, drive status %d, CD volume %d\n", cd_song, cd_status, cd_volume);
        prof_phase_reset();
        qa_window_reset(&win);
    }
#ifdef KL_CAPTURE
    if(game.state == ST_RESULTS && game.time > 3.4f && shot < 100) {
        qa_shot("results");
        shot = 100;
    }
#endif
    if(game.state == ST_RESULTS && game.time > 4.6f) {
        done = true;
        printf("KL AT: buildings=%d vehicles=%d eaten=%d tricks=%d total=%llu best_combo=%llu best_mult=%d grade=%d\n", score.buildings,
               score.vehicles, score.eaten, score.tricks, (unsigned long long)score.total, (unsigned long long)score.best_combo,
               score.best_mult, score_grade(score.total));
        printf(score.buildings > 0 ? "KL AUTOTEST: PASS\n" : "KL AUTOTEST: FAIL (nothing fell down)\n");
    }
}
#endif
