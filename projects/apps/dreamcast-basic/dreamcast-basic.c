/*
 * Dreamcast BASIC: the KallistiOS front end. Implements platform.h with the
 * Dreamcast keyboard and controller, a 640x480 RGB565 framebuffer, an AICA
 * square-wave stream, and one VMU save file per BASIC program. romdisk.o
 * mounts the bundled examples at /rd.
 */
#include <kos.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "app.h"
#include "platform.h"
#include "screen.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

#define FRAME_MS 16
#define KEY_QUEUE_SIZE 64
#define TONE_QUEUE_SIZE 256
#define SAMPLE_RATE 44100
#define STREAM_BYTES 8192
#define TONE_AMPLITUDE 5000
#define RAMP_SAMPLES 96
#define STATUS_BACK 0x0555          /* RGB565 of 0x00aaaa */
#define STATUS_INK 0x0000

/* ------------------------------------------------------------------------ */
/* Display                                                                   */

static uint16_t palette565[256];
static uint64_t last_frame_ms;
static bool cursor_shown;
static int cursor_x, cursor_y, cursor_w, cursor_h;

static void mark_rows(int top, int bottom) {
    if(scr.dirty_top > scr.dirty_bottom) {
        scr.dirty_top = top;
        scr.dirty_bottom = bottom;
        return;
    }
    if(top < scr.dirty_top)
        scr.dirty_top = top;
    if(bottom > scr.dirty_bottom)
        scr.dirty_bottom = bottom;
}

static void present(uint64_t now) {
    static uint8_t status_mask[SCR_CELL_H][SCR_W];
    const bool want_cursor = scr.cursor_on && (now / 400) % 2 == 0;
    const int status_top = scr_status_y();
    int x, y, cx, cy, cw, ch;

    if(scr.palette_dirty) {
        for(x = 0; x < 256; ++x) {
            const uint32_t rgb = scr.palette[x];
            palette565[x] = (uint16_t)((rgb >> 19 & 31) << 11 |
                                       (rgb >> 10 & 63) << 5 |
                                       (rgb >> 3 & 31));
        }
        scr.palette_dirty = false;
    }
    if(scr.status_dirty) {
        scr_status_mask(status_mask);
        mark_rows(status_top, status_top + SCR_CELL_H - 1);
        scr.status_dirty = false;
    }
    /* Repaint under a cursor that has moved or blinked off. */
    scr_cursor_rect(&cx, &cy, &cw, &ch);
    if(cursor_shown && (!want_cursor || cx != cursor_x || cy != cursor_y)) {
        mark_rows(cursor_y, cursor_y + cursor_h - 1);
        cursor_shown = false;
    }
    if(want_cursor && !cursor_shown)
        mark_rows(cy, cy + ch - 1);

    if(scr.dirty_top > scr.dirty_bottom)
        return;
    for(y = scr.dirty_top; y <= scr.dirty_bottom && y < SCR_H; ++y) {
        const uint8_t *source = scr.pixels + y * SCR_W;
        uint16_t *dest = vram_s + y * SCR_W;

        if(scr.status_on && y >= status_top && y < status_top + SCR_CELL_H) {
            const uint8_t *mask = status_mask[y - status_top];
            for(x = 0; x < SCR_W; ++x)
                dest[x] = mask[x] ? STATUS_INK : STATUS_BACK;
            continue;
        }
        for(x = 0; x < SCR_W; x += 4) {
            dest[x] = palette565[source[x]];
            dest[x + 1] = palette565[source[x + 1]];
            dest[x + 2] = palette565[source[x + 2]];
            dest[x + 3] = palette565[source[x + 3]];
        }
    }
    if(want_cursor && cy >= 0 && cy + ch <= SCR_H && cx >= 0 &&
       cx + cw <= SCR_W) {
        for(y = cy; y < cy + ch; ++y) {
            for(x = cx; x < cx + cw; ++x)
                vram_s[y * SCR_W + x] = 0xffff;
        }
        cursor_shown = true;
        cursor_x = cx;
        cursor_y = cy;
        cursor_w = cw;
        cursor_h = ch;
    }
    scr.dirty_top = 1;
    scr.dirty_bottom = 0;
}

/* ------------------------------------------------------------------------ */
/* Keyboard and controller                                                   */

static int key_queue[KEY_QUEUE_SIZE];
static unsigned key_head, key_tail;
static bool break_flag;
static uint32_t previous_buttons;
static cont_state_t pad;
static bool pad_present;

/* Test builds pass bare words: -DBASIC_AUTORUN=STARTREK -DBASIC_AUTOKEYS=SRS_
   where _ stands for Enter; see plat_key() for the other escapes. */
#if defined(BASIC_AUTORUN) || defined(BASIC_AUTOKEYS)
#define BASIC_TEST_BUILD 1
#endif
#define STRINGIFY_ARGS(...) #__VA_ARGS__
#define STRINGIFY(...) STRINGIFY_ARGS(__VA_ARGS__)
#ifdef BASIC_AUTOKEYS
static const char *auto_keys = STRINGIFY(BASIC_AUTOKEYS);
#endif

static void push_key(int key) {
    const unsigned next = (key_tail + 1) % KEY_QUEUE_SIZE;
    if(key == 27 || key == 3)
        break_flag = true;
    if(next == key_head)
        return;
    key_queue[key_tail] = key;
    key_tail = next;
}

static int translate_key(kbd_key_t key) {
    switch(key) {
        case KBD_KEY_ENTER:
        case KBD_KEY_PAD_ENTER: return 13;
        case KBD_KEY_BACKSPACE: return 8;
        case KBD_KEY_TAB: return 9;
        case KBD_KEY_ESCAPE: return 27;
        case KBD_KEY_UP: return K_UP;
        case KBD_KEY_DOWN: return K_DOWN;
        case KBD_KEY_LEFT: return K_LEFT;
        case KBD_KEY_RIGHT: return K_RIGHT;
        case KBD_KEY_HOME: return K_HOME;
        case KBD_KEY_END: return K_END;
        case KBD_KEY_PGUP: return K_PGUP;
        case KBD_KEY_PGDOWN: return K_PGDN;
        case KBD_KEY_INSERT: return K_INS;
        case KBD_KEY_DEL: return K_DEL;
        default: break;
    }
    if(key >= KBD_KEY_F1 && key <= KBD_KEY_F12)
        return K_F1 + (int)(key - KBD_KEY_F1);
    return 0;
}

static void poll_keyboard(void) {
    maple_device_t *keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    kbd_state_t *state;
    int event;

    if(!keyboard || !(state = kbd_get_state(keyboard)))
        return;
    /* Raw events keep HID codes such as Up (0x52) apart from ASCII 'R'. */
    while((event = kbd_queue_pop(keyboard, false)) != KBD_QUEUE_END) {
        const kbd_key_t key = (kbd_key_t)(event & 0xff);
        kbd_mods_t mods;
        kbd_leds_t leds;
        int code = translate_key(key);

        mods.raw = (uint8_t)(event >> 8 & 0xff);
        leds.raw = (uint8_t)(event >> 16 & 0xff);
        if(mods.raw & KBD_MOD_CTRL) {
            if(code == K_HOME)
                code = K_CTRL_HOME;
            else if(code == K_END)
                code = K_CTRL_END;
            else if(key >= KBD_KEY_A && key <= KBD_KEY_Z)
                code = 1 + (int)(key - KBD_KEY_A);
        }
        if(!code) {
            code = (unsigned char)kbd_key_to_ascii(key, state->region, mods,
                                                   leds);
            if(code < 32)
                code = 0;
        }
        if(code)
            push_key(code);
    }
}

/* A controller can stand in for the keys needed to load and run examples,
   and lets INKEY$ games be steered with the D-pad. */
static void poll_controller(void) {
    static const struct { uint32_t button; int key; } map[] = {
        {CONT_DPAD_UP, K_UP}, {CONT_DPAD_DOWN, K_DOWN},
        {CONT_DPAD_LEFT, K_LEFT}, {CONT_DPAD_RIGHT, K_RIGHT},
        {CONT_A, 13}, {CONT_B, 27}, {CONT_Y, K_F2}, {CONT_X, K_F7},
        {CONT_START, K_F5}
    };
    maple_device_t *controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *state = controller ? maple_dev_status(controller) : NULL;
    uint32_t pressed;
    size_t i;

    pad_present = state != NULL;
    if(!state) {
        previous_buttons = 0;
        return;
    }
    pad = *state;
    pressed = state->buttons & ~previous_buttons;
    previous_buttons = state->buttons;
    for(i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if(pressed & map[i].button)
            push_key(map[i].key);
    }
}

void plat_poll(void) {
    const uint64_t now = timer_ms_gettime64();

    if(now - last_frame_ms < FRAME_MS)
        return;
    last_frame_ms = now;
    poll_keyboard();
    poll_controller();
    present(now);
}

int plat_key(void) {
    int key;

    if(key_head == key_tail) {
#ifdef BASIC_AUTOKEYS
        if(*auto_keys) {
            key = (unsigned char)*auto_keys++;
            if(key == '@' && *auto_keys) {
                /* @1..@9 are F1..F9; @S space, @E Escape, @U up, @D down. */
                const int which = (unsigned char)*auto_keys++;
                key = which == 'S' ? ' ' : which == 'E' ? 27 :
                      which == 'U' ? K_UP : which == 'D' ? K_DOWN :
                      K_F1 + (which - '1');
                if(key == 27)
                    break_flag = true;
                return key;
            }
            return key == '_' ? 13 : key;
        }
#endif
        return 0;
    }
    key = key_queue[key_head];
    key_head = (key_head + 1) % KEY_QUEUE_SIZE;
    return key;
}

void plat_flush_keys(void) {
    plat_poll();
    key_head = key_tail;
    break_flag = false;
}

bool plat_break_pressed(void) {
    const bool pressed = break_flag;
    break_flag = false;
    return pressed;
}

int plat_stick(int axis) {
    if(!pad_present)
        return 0;
    switch(axis) {
        case 0: return pad.joyx;
        case 1: return pad.joyy;
        case 2:
            return (pad.buttons & CONT_DPAD_RIGHT ? 127 : 0) -
                   (pad.buttons & CONT_DPAD_LEFT ? 127 : 0);
        default:
            return (pad.buttons & CONT_DPAD_DOWN ? 127 : 0) -
                   (pad.buttons & CONT_DPAD_UP ? 127 : 0);
    }
}

bool plat_button(int button) {
    static const uint32_t masks[] = {CONT_A, CONT_X, CONT_B, CONT_Y};
    return pad_present && button >= 0 && button < 4 &&
           (pad.buttons & masks[button]) != 0;
}

double plat_seconds(void) {
    return (double)timer_ms_gettime64() / 1000.0;
}

double plat_clock_seconds(void) {
    static double midnight_offset = -1;

    if(midnight_offset < 0) {
        const time_t now = time(NULL);
        const struct tm *parts = localtime(&now);
        const double of_day = parts ? parts->tm_hour * 3600.0 +
                                      parts->tm_min * 60.0 + parts->tm_sec : 0;
        midnight_offset = of_day - plat_seconds();
    }
    return fmod(midnight_offset + plat_seconds() + 86400.0, 86400.0);
}

void plat_sleep_ms(int ms) {
    thd_sleep(ms);
}

void plat_debug_output(const char *text, size_t size) {
#ifdef BASIC_TEST_BUILD
    fwrite(text, 1, size, stdout);
#else
    (void)text;
    (void)size;
#endif
}

/* ------------------------------------------------------------------------ */
/* Sound                                                                     */

typedef struct {
    uint32_t phase_step;            /* 16.16 cycles per sample; 0 is a rest */
    uint32_t samples;
} tone_t;

static tone_t tones[TONE_QUEUE_SIZE];
static unsigned tone_head, tone_tail;
static uint32_t tone_position, tone_phase;
static uint64_t queued_samples;
static mutex_t tone_lock = MUTEX_INITIALIZER;
static snd_stream_hnd_t stream = SND_STREAM_INVALID;
static int16_t stream_buffer[SND_STREAM_BUFFER_MAX / 2];
static volatile bool audio_running;

static void *stream_callback(snd_stream_hnd_t handle, int bytes_requested,
                             int *bytes_returned) {
    int count = bytes_requested / 2, i;

    (void)handle;
    if(count > (int)(sizeof(stream_buffer) / sizeof(stream_buffer[0])))
        count = (int)(sizeof(stream_buffer) / sizeof(stream_buffer[0]));
    mutex_lock(&tone_lock);
    for(i = 0; i < count; ++i) {
        tone_t *tone = &tones[tone_head];
        int32_t level = 0;

        if(tone_head != tone_tail) {
            if(tone->phase_step) {
                const uint32_t left = tone->samples - tone_position;
                int32_t gain = TONE_AMPLITUDE;

                /* Short ramps keep note edges from clicking. */
                if(tone_position < RAMP_SAMPLES)
                    gain = gain * (int32_t)tone_position / RAMP_SAMPLES;
                if(left < RAMP_SAMPLES)
                    gain = gain * (int32_t)left / RAMP_SAMPLES;
                level = tone_phase & 0x8000 ? -gain : gain;
                tone_phase += tone->phase_step;
            }
            --queued_samples;
            if(++tone_position >= tone->samples) {
                tone_head = (tone_head + 1) % TONE_QUEUE_SIZE;
                tone_position = 0;
                tone_phase = 0;
            }
        }
        stream_buffer[i] = (int16_t)level;
    }
    mutex_unlock(&tone_lock);
    *bytes_returned = count * 2;
    return stream_buffer;
}

static void *audio_thread(void *unused) {
    (void)unused;
    while(audio_running) {
        snd_stream_poll(stream);
        thd_sleep(10);
    }
    return NULL;
}

static void audio_init(void) {
    if(snd_stream_init() < 0) {
        printf("Dreamcast BASIC: AICA stream init failed; no sound.\n");
        return;
    }
    stream = snd_stream_alloc(stream_callback, STREAM_BYTES);
    if(stream == SND_STREAM_INVALID) {
        printf("Dreamcast BASIC: stream allocation failed; no sound.\n");
        return;
    }
    snd_stream_volume(stream, 220);
    snd_stream_start(stream, SAMPLE_RATE, 0);
    audio_running = true;
    if(!thd_create(true, audio_thread, NULL)) {
        audio_running = false;
        printf("Dreamcast BASIC: audio thread failed; no sound.\n");
    }
}

void plat_tone(double hz, int ms) {
    unsigned next;

    if(ms <= 0)
        return;
    mutex_lock(&tone_lock);
    next = (tone_tail + 1) % TONE_QUEUE_SIZE;
    if(next != tone_head && audio_running) {
        tones[tone_tail].phase_step = hz >= 20 && hz < SAMPLE_RATE / 2 ?
            (uint32_t)(hz * 65536.0 / SAMPLE_RATE) : 0;
        tones[tone_tail].samples = (uint32_t)((uint64_t)ms * SAMPLE_RATE /
                                              1000);
        queued_samples += tones[tone_tail].samples;
        tone_tail = next;
    }
    mutex_unlock(&tone_lock);
#ifdef BASIC_TEST_BUILD
    printf("[tone %dHz %dms]\n", (int)hz, ms);
#endif
}

int plat_tone_queued_ms(void) {
    uint64_t samples;

    mutex_lock(&tone_lock);
    samples = queued_samples;
    mutex_unlock(&tone_lock);
    return (int)((samples * 1000 + SAMPLE_RATE - 1) / SAMPLE_RATE);
}

void plat_tone_stop(void) {
    mutex_lock(&tone_lock);
    tone_head = tone_tail;
    tone_position = 0;
    queued_samples = 0;
    mutex_unlock(&tone_lock);
}

/* ------------------------------------------------------------------------ */
/* Program storage                                                           */

#define EXTENSION ".BAS"

static bool has_extension(const char *file_name, size_t *stem) {
    const char *dot = strrchr(file_name, '.');

    if(!dot || strcasecmp(dot, EXTENSION) != 0)
        return false;
    *stem = (size_t)(dot - file_name);
    return *stem >= 1 && *stem <= PLAT_NAME_MAX;
}

static int add_name(char names[][PLAT_NAME_MAX + 1], int count, int max,
                    const char *file_name, size_t stem) {
    char name[PLAT_NAME_MAX + 1];
    size_t k;
    int i;

    for(k = 0; k < stem; ++k)
        name[k] = (char)toupper((unsigned char)file_name[k]);
    name[stem] = '\0';
    for(i = 0; i < count; ++i) {
        if(strcmp(names[i], name) == 0)
            return count;
    }
    if(count == max)
        return count;
    /* Keep the list sorted. */
    for(i = count; i > 0 && strcmp(names[i - 1], name) > 0; --i)
        strcpy(names[i], names[i - 1]);
    strcpy(names[i], name);
    return count + 1;
}

static int list_directory(const char *path, char names[][PLAT_NAME_MAX + 1],
                          int count, int max) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    size_t stem;

    if(!dir)
        return count;
    while((entry = readdir(dir)) != NULL) {
        if(has_extension(entry->d_name, &stem))
            count = add_name(names, count, max, entry->d_name, stem);
    }
    closedir(dir);
    return count;
}

/* Finds NAME.BAS in a directory whatever its case; fills `path`. */
static bool find_file(const char *directory, const char *name, char *path,
                      size_t path_size) {
    DIR *dir = opendir(directory);
    struct dirent *entry;
    size_t stem;
    bool found = false;

    if(!dir)
        return false;
    while(!found && (entry = readdir(dir)) != NULL) {
        if(has_extension(entry->d_name, &stem) && strlen(name) == stem &&
           strncasecmp(entry->d_name, name, stem) == 0) {
            snprintf(path, path_size, "%s/%s", directory, entry->d_name);
            found = true;
        }
    }
    closedir(dir);
    return found;
}

static const char *read_whole_file(const char *path, char **text,
                                   size_t *size) {
    file_t file = fs_open(path, O_RDONLY);
    ssize_t total, got = 0;
    char *data;

    if(file == FILEHND_INVALID)
        return "Cannot open the file";
    total = (ssize_t)fs_total(file);
    if(total < 0 || total > 4 * 1024 * 1024) {
        fs_close(file);
        return "Cannot read the file";
    }
    data = malloc((size_t)total + 1);
    if(!data) {
        fs_close(file);
        return "Out of memory";
    }
    while(got < total) {
        const ssize_t chunk = fs_read(file, data + got, (size_t)(total - got));
        if(chunk <= 0)
            break;
        got += chunk;
    }
    fs_close(file);
    /* VMU files are padded to whole blocks. */
    while(got > 0 && data[got - 1] == '\0')
        --got;
    data[got] = '\0';
    *text = data;
    *size = (size_t)got;
    return NULL;
}

static void vmu_directory(const maple_device_t *vmu, char *path,
                          size_t size) {
    snprintf(path, size, "/vmu/%c%d", 'a' + vmu->port, vmu->unit);
}

int plat_storage_list(char names[][PLAT_NAME_MAX + 1], int max) {
    maple_device_t *vmu;
    char directory[16];
    int index, count = 0;

    for(index = 0; (vmu = maple_enum_type(index, MAPLE_FUNC_MEMCARD)) != NULL;
        ++index) {
        vmu_directory(vmu, directory, sizeof(directory));
        count = list_directory(directory, names, count, max);
    }
    return count;
}

const char *plat_storage_load(const char *name, char **text, size_t *size) {
    maple_device_t *vmu;
    char directory[16], path[48];
    int index;

    for(index = 0; (vmu = maple_enum_type(index, MAPLE_FUNC_MEMCARD)) != NULL;
        ++index) {
        vmu_directory(vmu, directory, sizeof(directory));
        if(find_file(directory, name, path, sizeof(path)))
            return read_whole_file(path, text, size);
    }
    return index ? "That program is not on a VMU" : "No VMU is connected";
}

/* "BAS" over "IC>" in the screen font, white on blue. */
static void build_icon(vmu_pkg_t *header, uint8_t *icon) {
    static const char *const rows[2] = {"BAS", "IC>"};
    static uint8_t pixels[32][32];
    int row, col, x, y;

    memset(pixels, 0, sizeof(pixels));
    for(row = 0; row < 2; ++row) {
        for(col = 0; col < 3; ++col)
            scr_glyph(pixels[row * 16] + 4 + col * 8, 32,
                      rows[row][col], 8, 1, 0);
    }
    for(y = 0; y < 32; ++y) {
        for(x = 0; x < 32; x += 2)
            icon[y * 16 + x / 2] = (uint8_t)(pixels[y][x] << 4 |
                                             pixels[y][x + 1]);
    }
    header->icon_pal[0] = 0xf12a;   /* ARGB4444 */
    header->icon_pal[1] = 0xffff;
    header->icon_cnt = 1;
    header->icon_anim_speed = 0;
    header->icon_data = icon;
}

const char *plat_storage_save(const char *name, const char *text,
                              size_t size) {
    static uint8_t icon[512];
    static uint8_t no_eyecatch;
    maple_device_t *vmu, *target = NULL;
    char directory[16], path[48];
    vmu_pkg_t header;
    file_t file;
    ssize_t written;
    int index, header_result, close_result;

    /* Overwrite the program where it already lives; else use the first VMU. */
    for(index = 0; (vmu = maple_enum_type(index, MAPLE_FUNC_MEMCARD)) != NULL;
        ++index) {
        vmu_directory(vmu, directory, sizeof(directory));
        if(!target)
            target = vmu;
        if(find_file(directory, name, path, sizeof(path))) {
            target = vmu;
            break;
        }
    }
    if(!target)
        return "No VMU is connected";
    vmu_directory(target, directory, sizeof(directory));
    snprintf(path, sizeof(path), "%s/%s" EXTENSION, directory, name);

    memset(&header, 0, sizeof(header));
    snprintf(header.desc_short, sizeof(header.desc_short), "BASIC program");
    snprintf(header.desc_long, sizeof(header.desc_long), "Dreamcast BASIC: %s",
             name);
    snprintf(header.app_id, sizeof(header.app_id), "DCBASIC");
    build_icon(&header, icon);
    header.eyecatch_type = VMUPKG_EC_NONE;
    header.eyecatch_data = &no_eyecatch;
    header.data_len = (int)size;
    header.data = (const uint8_t *)text;

    file = fs_open(path, O_WRONLY | O_TRUNC);
    if(file == FILEHND_INVALID) {
        printf("Dreamcast BASIC: cannot open %s: %s\n", path, strerror(errno));
        return "Cannot create the VMU file";
    }
    written = fs_write(file, text, size);
    header_result = fs_vmu_set_header(file, &header);
    close_result = fs_close(file);
    if(written != (ssize_t)size || header_result < 0 || close_result < 0) {
        printf("Dreamcast BASIC: VMU write failed at %s: %s\n", path,
               strerror(errno));
        return "VMU write failed - is the VMU full?";
    }
    printf("Dreamcast BASIC: saved %s (%u bytes)\n", path, (unsigned)size);
    return NULL;
}

int plat_examples_list(char names[][PLAT_NAME_MAX + 1], int max) {
    return list_directory("/rd", names, 0, max);
}

const char *plat_example_load(const char *name, char **text, size_t *size) {
    char path[48];

    if(!find_file("/rd", name, path, sizeof(path)))
        return "Example not found";
    return read_whole_file(path, text, size);
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    vid_set_mode(DM_640x480, PM_RGB565);
    vid_clear(0, 0, 0);
    audio_init();
    app_init();
    printf("Dreamcast BASIC ready.\n");

#ifdef BASIC_AUTORUN
    {
        /* Test builds load and run one bundled example immediately. */
        char *text;
        size_t size;
        const char *name = STRINGIFY(BASIC_AUTORUN);
        const char *error = plat_example_load(name, &text, &size);
        if(error) {
            printf("Dreamcast BASIC: autorun %s: %s\n", name, error);
        }
        else {
            app_load_text(name, text, size);
            free(text);
            app_run_now();
        }
    }
#endif

    for(;;)
        app_step();
    return 0;
}
