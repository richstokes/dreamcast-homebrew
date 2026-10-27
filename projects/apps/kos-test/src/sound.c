/* Sound test: tones on the left, right and both speakers, sweeps of pitch,
   pan and volume, a musical scale and the basic waveforms, played from
   generated samples on one reserved AICA channel. Pitch, pan and volume
   change while the channel plays, through the driver's update command. */
#include "gfx.h"
#include "kt.h"

#include <dc/sound/aica_comm.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Samples hold whole cycles of 32 samples, so a rate of R plays R/32 Hz. */
#define CYCLE 32
#define TONE_LEN (CYCLE * 100)
#define NOISE_LEN 16384
#define BASE_RATE 32000

enum { W_SINE, W_SQUARE, W_SAW, W_NOISE, WAVES };
static const char *const wave_names[WAVES] = { "SINE", "SQUARE", "SAW", "NOISE" };

static int16_t *wave_data[WAVES];
static sfxhnd_t wave_sfx[WAVES];
static int chn = -1;
static bool ready;

enum { T_LEFT, T_RIGHT, T_CENTRE, T_PAN, T_PITCH, T_SCALE, T_WAVES, T_VOLUME, T_STOP, TESTS };
static const char *const items[TESTS] = {
    "1 KHZ LEFT", "1 KHZ RIGHT", "1 KHZ BOTH", "STEREO PAN SWEEP", "PITCH 100 HZ-4 KHZ",
    "SCALE C4-C5", "WAVEFORMS", "VOLUME STEPS", "STOP",
};

static int sel, playing = -1;
static uint32_t play_frames;
static int cur_wave, cur_vol, cur_pan;
static float cur_hz;

static void chan_update(uint32_t what, uint32_t value) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, ch);

    memset(tmp, 0, sizeof(tmp));
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = (uint32_t)chn;
    ch->cmd = AICA_CH_CMD_UPDATE | what;
    ch->freq = ch->vol = ch->pan = value;
    snd_sh4_to_aica(tmp, cmd->size);
}

static void set_hz(float hz) {
    cur_hz = hz;
    chan_update(AICA_CH_UPDATE_SET_FREQ, (uint32_t)(hz * CYCLE));
}

static void set_pan(int pan) {
    cur_pan = pan;
    chan_update(AICA_CH_UPDATE_SET_PAN, (uint32_t)pan);
}

static void set_vol(int vol) {
    cur_vol = vol;
    chan_update(AICA_CH_UPDATE_SET_VOL, (uint32_t)vol);
}

static void start(int wave, float hz, int vol, int pan) {
    sfx_play_data_t d;

    snd_sfx_stop(chn);
    memset(&d, 0, sizeof(d));
    d.chn = chn;
    d.idx = wave_sfx[wave];
    d.vol = vol;
    d.pan = pan;
    d.loop = 1;
    d.freq = wave == W_NOISE ? 22050 : (int)(hz * CYCLE);
    snd_sfx_play_ex(&d);
    cur_wave = wave;
    cur_hz = hz;
    cur_vol = vol;
    cur_pan = pan;
}

static void stop(void) {
    if(chn >= 0)
        snd_sfx_stop(chn);
    playing = -1;
}

static void sound_init(void) {
    uint32_t seed = 77;

    if(ready)
        return;
    snd_init();
    for(int w = 0; w < WAVES; w++) {
        int len = w == W_NOISE ? NOISE_LEN : TONE_LEN;
        wave_data[w] = memalign(32, (size_t)len * 2);
        for(int i = 0; i < len; i++) {
            int p = i % CYCLE;
            float s;
            switch(w) {
                case W_SINE: s = sinf(p * 2 * (float)M_PI / CYCLE); break;
                case W_SQUARE: s = p < CYCLE / 2 ? 0.7f : -0.7f; break;
                case W_SAW: s = (p / (float)CYCLE) * 1.6f - 0.8f; break;
                default: s = ((int)(rng_next(&seed) & 0xffff) - 32768) / 49152.0f;
            }
            wave_data[w][i] = (int16_t)(s * 26000);
        }
        wave_sfx[w] = snd_sfx_load_raw_buf((char *)wave_data[w], (size_t)len * 2,
                                           w == W_NOISE ? 22050 : BASE_RATE, 16, 1);
    }
    chn = snd_sfx_chn_alloc();
    ready = true;
    KT_LOG("Sound: AICA channel %d, %lu KB sound RAM free\n", chn,
           (unsigned long)(snd_mem_available() >> 10));
}

static void play(int t) {
    playing = t;
    play_frames = 0;
    switch(t) {
        case T_LEFT: start(W_SINE, 1000, 200, 0); break;
        case T_RIGHT: start(W_SINE, 1000, 200, 255); break;
        case T_CENTRE: start(W_SINE, 1000, 200, 128); break;
        case T_PAN: start(W_SINE, 500, 200, 128); break;
        case T_PITCH: start(W_SINE, 100, 200, 128); break;
        case T_SCALE: start(W_SQUARE, 261.63f, 160, 128); break;
        case T_WAVES: start(W_SINE, 440, 180, 128); break;
        case T_VOLUME: start(W_SINE, 660, 255, 128); break;
        default: stop();
    }
}

/* Per-frame changes for the moving tests. */
static void animate(void) {
    static const float scale[8] = { 261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f,
                                    493.88f, 523.25f };
    uint32_t f = ++play_frames;

    switch(playing) {
        case T_PAN:
            if(f % 2 == 0)
                set_pan((int)(128 + 127 * sinf(f * 0.03f)));
            break;
        case T_PITCH: {
            /* 100 Hz to 4 kHz and back, exponentially, over 8 seconds */
            float t = (f % 480) / 240.0f;
            float k = t < 1 ? t : 2 - t;
            if(f % 2 == 0)
                set_hz(100.0f * powf(40.0f, k));
            break;
        }
        case T_SCALE:
            if(f % 30 == 0)
                set_hz(scale[(f / 30) % 8]);
            break;
        case T_WAVES:
            if(f % 60 == 0)
                start((cur_wave + 1) % WAVES, 440, 180, 128);
            break;
        case T_VOLUME:
            if(f % 30 == 0)
                set_vol(cur_vol <= 31 ? 255 : cur_vol - 32);
            break;
    }
}

static void sound_enter(void) {
    sound_init();
    playing = -1;
}

static void sound_leave(void) {
    stop();
}

static void sound_update(const kt_input_t *in) {
    if(in->repeat & CONT_DPAD_DOWN) sel = (sel + 1) % TESTS;
    if(in->repeat & CONT_DPAD_UP) sel = (sel + TESTS - 1) % TESTS;
    if(in->pressed & (CONT_B | CONT_START)) {
        kt_pop();
        return;
    }
    if(in->pressed & CONT_A) {
        if(playing == sel || sel == T_STOP)
            stop();
        else
            play(sel);
    }
    if(playing >= 0)
        animate();
}

static void sound_draw(void) {
    ui_background();
    ui_header("SOUND TEST", "AICA");
    ui_menu(24, 52, 360, items, TESTS, sel, 0, TESTS);
    if(playing >= 0)
        gfx_text(8, 52 + playing * 28 + 5, C_PASS, "\xbb");

    ui_panel(400, 52, 224, 260, "CHANNEL");
    if(playing >= 0) {
        float x = 410;
        gfx_textf(x, 78, C_TEXT, "WAVE    %s", wave_names[cur_wave]);
        if(cur_wave != W_NOISE)
            gfx_textf(x, 96, C_TEXT, "PITCH   %.1f HZ", (double)cur_hz);
        gfx_textf(x, 114, C_TEXT, "VOLUME  %d", cur_vol);
        gfx_textf(x, 132, C_TEXT, "PAN     %d %s", cur_pan,
                  cur_pan < 40 ? "LEFT" : cur_pan > 215 ? "RIGHT" : "");
        gfx_textf(x, 150, C_TEXT, "POS     %u", snd_get_pos((unsigned)chn));
        /* pan meter */
        gfx_rect(x, 176, 200, 8, 0xff1a3050);
        gfx_rect(x + cur_pan * 196 / 255.0f, 172, 4, 16, C_HI);
        gfx_text(x, 190, C_DIM, "L");
        gfx_text(x + 192, 190, C_DIM, "R");
        /* one cycle of the wave */
        const int16_t *w = wave_data[cur_wave];
        for(int i = 0; i < 99; i++) {
            float y0 = 260 - w[i * (cur_wave == W_NOISE ? 3 : 1) % TONE_LEN] / 700.0f;
            float y1 = 260 - w[(i + 1) * (cur_wave == W_NOISE ? 3 : 1) % TONE_LEN] / 700.0f;
            gfx_line(x + i * 2, y0, x + i * 2 + 2, y1, 1.5f, C_PASS);
        }
    }
    else
        gfx_text(410, 78, C_DIM, "Nothing playing");
    gfx_text(24, 330, C_DIM, "Tones play on a channel reserved with snd_sfx_chn_alloc; pitch,");
    gfx_text(24, 348, C_DIM, "pan and volume change live with AICA channel update commands.");
    gfx_textf(24, 376, C_DIM, "Channel %d   Sound RAM free %lu KB", chn,
              (unsigned long)(snd_mem_available() >> 10));
    ui_footer("UP/DOWN SELECT   A PLAY/STOP   B BACK");
}

const kt_screen_t sound_screen = {
    "Sound test", sound_enter, sound_update, sound_draw, sound_leave
};

#ifdef KT_AUTOTEST
/* Play each test for a second, then stop. Returns 1 when finished. */
int sound_autotest(int frame) {
    if(frame == 1)
        kt_push(&sound_screen);
    if(frame % 60 == 2) {
        int t = frame / 60;
        if(t >= T_STOP) {
            stop();
            return 1;
        }
        sel = t;
        play(t);
        KT_LOG("Sound test: %s\n", items[t]);
    }
    return 0;
}
#endif
