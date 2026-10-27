/*
 * KOS_Test: a service-menu style test and benchmark suite for KallistiOS on
 * the Sega Dreamcast. Boot asks whether to test a VMU, then the main menu
 * lists the tests. Each test screen is its own module (see kt.h).
 */
#include "gfx.h"
#include "kt.h"
#include "../../../common/softreset.h"

#include <dc/maple/keyboard.h>
#include <stdio.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

/* ---- Input ---- */

static uint32_t prev_held;
static int repeat_frames;

void input_poll(kt_input_t *in) {
    uint32_t held = 0;
    int lt = 0, rt = 0;
    maple_device_t *dev;

    for(int n = 0; (dev = maple_enum_type(n, MAPLE_FUNC_CONTROLLER)) != NULL; n++) {
        cont_state_t *st = maple_dev_status(dev);
        if(!st)
            continue;
        held |= st->buttons;
        if(st->joyy < -96) held |= CONT_DPAD_UP;
        if(st->joyy > 96) held |= CONT_DPAD_DOWN;
        if(st->joyx < -96) held |= CONT_DPAD_LEFT;
        if(st->joyx > 96) held |= CONT_DPAD_RIGHT;
        if(st->ltrig > lt) lt = st->ltrig;
        if(st->rtrig > rt) rt = st->rtrig;
    }
    for(int n = 0; (dev = maple_enum_type(n, MAPLE_FUNC_KEYBOARD)) != NULL; n++) {
        kbd_state_t *k = kbd_get_state(dev);
        if(!k)
            continue;
        if(k->key_states[KBD_KEY_UP].is_down) held |= CONT_DPAD_UP;
        if(k->key_states[KBD_KEY_DOWN].is_down) held |= CONT_DPAD_DOWN;
        if(k->key_states[KBD_KEY_LEFT].is_down) held |= CONT_DPAD_LEFT;
        if(k->key_states[KBD_KEY_RIGHT].is_down) held |= CONT_DPAD_RIGHT;
        if(k->key_states[KBD_KEY_ENTER].is_down ||
           k->key_states[KBD_KEY_PAD_ENTER].is_down) held |= CONT_A;
        if(k->key_states[KBD_KEY_ESCAPE].is_down ||
           k->key_states[KBD_KEY_BACKSPACE].is_down) held |= CONT_B;
        if(k->key_states[KBD_KEY_X].is_down) held |= CONT_X;
        if(k->key_states[KBD_KEY_Y].is_down) held |= CONT_Y;
    }
    in->held = held;
    in->pressed = held & ~prev_held;
    in->repeat = in->pressed;
    in->ltrig = lt;
    in->rtrig = rt;

    const uint32_t dirs = CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_DPAD_LEFT | CONT_DPAD_RIGHT;
    if(held & dirs & prev_held) {
        if(++repeat_frames > 18 && (repeat_frames & 3) == 0)
            in->repeat |= held & dirs;
    }
    else
        repeat_frames = 0;
    prev_held = held;
}

void input_flush(void) {
    kt_input_t in;

    for(int i = 0; i < 120; i++) {
        input_poll(&in);
        if(!in.held)
            break;
        vid_waitvbl();
    }
}

/* ---- Screen stack ---- */

#define STACK_MAX 8

static const kt_screen_t *stack[STACK_MAX];
static int depth;
static uint32_t frame;
static bool quitting;

const kt_screen_t *kt_current(void) {
    return depth ? stack[depth - 1] : NULL;
}

uint32_t kt_frame(void) {
    return frame;
}

void kt_push(const kt_screen_t *s) {
    if(depth >= STACK_MAX)
        return;
    stack[depth++] = s;
    if(s->enter)
        s->enter();
    vmu_lcd_banner(KT_TITLE, s->title);
}

void kt_pop(void) {
    if(depth <= 1)
        return;
    const kt_screen_t *s = stack[--depth];
    if(s->leave)
        s->leave();
    vmu_lcd_banner(KT_TITLE, stack[depth - 1]->title);
}

void kt_replace(const kt_screen_t *s) {
    const kt_screen_t *old = stack[depth - 1];

    if(old->leave)
        old->leave();
    stack[depth - 1] = s;
    if(s->enter)
        s->enter();
}

void kt_quit(void) {
    quitting = true;
}

static void pop_to_menu(void) {
    while(depth > 1)
        kt_pop();
}

/* ---- Main menu ---- */

static void go_sysinfo(void) { kt_push(&sysinfo_screen); }
static void go_input(void) { kt_push(&input_screen); }
static void go_selftest(void) { kt_push(&selftest_screen); }
static void go_bench(void) { kt_push(&bench_screen); }
static void go_gpu(void) { kt_push(&gpu_screen); }
static void go_monitor(void) { kt_push(&monitor_screen); }
static void go_sound(void) { kt_push(&sound_screen); }
static void go_net(void) { kt_push(&net_screen); }

static const char *const menu_labels[] = {
    "SYSTEM INFORMATION",
    "INPUT TEST",
    "MEMORY & HARDWARE SELF-TEST",
    "CPU TEST SUITE",
    "BENCHMARKS",
    "GPU FEATURE TEST",
    "MONITOR TEST",
    "SOUND TEST",
    "NETWORK TEST",
    "VMU TEST",
    "RUN ALL TESTS",
    "EXIT TO BIOS",
};

static void (*const menu_actions[])(void) = {
    go_sysinfo, go_input, go_selftest, cpu_suite_run, go_bench, go_gpu,
    go_monitor, go_sound, go_net, vmu_prompt_start, run_everything, kt_quit,
};

#define MENU_COUNT ((int)(sizeof(menu_labels) / sizeof(menu_labels[0])))

static int menu_sel;

static void menu_update(const kt_input_t *in) {
    if(in->repeat & CONT_DPAD_DOWN) menu_sel = (menu_sel + 1) % MENU_COUNT;
    if(in->repeat & CONT_DPAD_UP) menu_sel = (menu_sel + MENU_COUNT - 1) % MENU_COUNT;
    if(in->pressed & (CONT_A | CONT_START))
        menu_actions[menu_sel]();
}

static void menu_draw(void) {
    char right[48];
    uint64_t s = timer_ms_gettime64() / 1000;

    ui_background();
    snprintf(right, sizeof(right), "KOS %s  UP %02lu:%02lu:%02lu", KOS_VERSION_STRING,
             (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
             (unsigned long)(s % 60));
    ui_header(KT_TITLE " " KT_VERSION, right);
    gfx_text_center(SCREEN_W / 2, 46, C_DIM, "KALLISTIOS TEST & BENCHMARK MENU", false);
    ui_menu(110, 68, 420, menu_labels, MENU_COUNT, menu_sel, 0, MENU_COUNT);

    int port = vmu_selected_port();
    if(port >= 0)
        gfx_textf(110, 408, C_DIM, "VMU %c%d SELECTED%s", 'A' + port, vmu_selected_unit(),
                  bench_baseline_valid() ? "  -  BENCHMARK BASELINE LOADED" : "");
    else
        gfx_text(110, 408, C_DIM, "NO VMU SELECTED");
    gfx_textf(110, 426, C_DIM, "BUILT %s  GCC %s", kt_build_date, __VERSION__);
    ui_footer("UP/DOWN SELECT   A RUN   A+B+X+Y+START ON THIS MENU EXITS");
}

const kt_screen_t menu_screen = {
    "Main menu", NULL, menu_update, menu_draw, NULL
};

/* ---- Scripted runs ----
   KT_AUTOTEST walks every screen with no input: each entry pushes a screen
   or starts a suite, then waits until the menu is back on top (suites) or
   for a number of frames (pages), and prints KT AUTOTEST DONE at the end. */
#ifdef KT_AUTOTEST
typedef struct {
    const char *name;
    void (*go)(void);
    int frames;     /* 0: wait for the menu to return */
} autotest_t;

extern void gpu_autotest_page(int page);
extern int gpu_page_count(void);
extern void monitor_autotest_page(int page);
extern int monitor_page_count(void);

static void at_report(void) { /* the runner pushes a report; pop it */ }

static const autotest_t at_script[] = {
#ifdef KT_AUTO      /* dcload-ip runs: no VMU writes, and the BBA stays dcload's */
    { "auto run", run_auto, 0 },
#else
#ifndef KT_TOUR     /* the tour build only walks the visual pages */
    { "vmu", vmu_prompt_start, 0 },
    { "sysinfo", go_sysinfo, 90 },
    { "input", go_input, 90 },
    { "run all", run_everything, 0 },
#endif
#ifndef KT_TOUR_NET    /* just the network test */
    { "gpu", go_gpu, -1 },
    { "monitor", go_monitor, -2 },
    { "sound", go_sound, -3 },
#endif
    { "network", go_net, -4 },
#endif
};
static int at_index = -1, at_wait, at_page;
static uint64_t at_started;

bool autotest_active(void) {
    return at_index >= 0 && at_index < (int)(sizeof(at_script) / sizeof(at_script[0]));
}

void autotest_next(void) {
    at_wait = 0;
}

extern int sound_autotest(int frame);
extern int net_autotest(int frame);

static void autotest_tick(void) {
    const int n = (int)(sizeof(at_script) / sizeof(at_script[0]));
    const kt_screen_t *cur = kt_current();

    (void)at_report;
    if(at_index < 0 && kt_frame() < 120)
        return;     /* show the menu first */
    if(at_index < 0) {
        at_index = 0;
        at_started = timer_ms_gettime64();
        KT_LOG("AUTOTEST step %s\n", at_script[0].name);
        at_script[0].go();
        at_wait = 0;
        return;
    }
    if(at_index >= n)
        return;

    const autotest_t *a = &at_script[at_index];
    bool finished = false;
    at_wait++;
    if(a->frames > 0)
        finished = at_wait >= a->frames;
    else if(a->frames == 0) {
        /* Suites end on a report screen: give it a second, then pop. */
        if(cur != &menu_screen && cur->title && !strcmp(cur->title, "Report")) {
            if(at_wait > 150)
                kt_pop();
        }
        else if(cur != &menu_screen)
            at_wait = 0;
        finished = cur == &menu_screen && at_wait > 10;
    }
    else if(a->frames == -1) {
        if(at_wait % 300 == 1) {
            if(at_page >= gpu_page_count()) finished = true;
            else gpu_autotest_page(at_page++);
        }
    }
    else if(a->frames == -2) {
        if(at_wait % 45 == 1) {
            if(at_page >= monitor_page_count()) finished = true;
            else monitor_autotest_page(at_page++);
        }
    }
    else if(a->frames == -3)
        finished = sound_autotest(at_wait);
    else if(a->frames == -4)
        finished = net_autotest(at_wait);

    if(!finished)
        return;
    pop_to_menu();
    at_page = 0;
    at_wait = 0;
    if(++at_index >= n) {
        KT_LOG("AUTOTEST DONE in %lu s\n",
               (unsigned long)((timer_ms_gettime64() - at_started) / 1000));
#ifdef KT_AUTO
        kt_quit();      /* back to dcload-ip, so dc-tool-ip ends too */
#endif
        return;
    }
    KT_LOG("AUTOTEST step %s\n", at_script[at_index].name);
    at_script[at_index].go();
}
#endif

/* ---- Main loop ---- */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    KT_LOG("%s %s on KOS %s, built %s\n", KT_TITLE, KT_VERSION,
           KOS_VERSION_STRING, kt_build_date);
    gfx_init();
    maple_events_start();
    kt_push(&menu_screen);
#ifndef KT_AUTOTEST
    vmu_prompt_start();     /* the script asks for it itself */
#endif

    while(!quitting) {
        kt_input_t in;

        input_poll(&in);
        int reset = softreset_poll(kt_current() == &menu_screen);
        if(reset == SOFTRESET_QUIT)
            break;
        if(reset == SOFTRESET_TITLE) {
            pop_to_menu();
            input_flush();
            continue;
        }
#ifdef KT_AUTOTEST
        autotest_tick();
#endif
        const kt_screen_t *cur = kt_current();
        if(cur->update)
            cur->update(&in);
        if(quitting)
            break;
        cur = kt_current();
        gfx_begin();
        cur->draw();
        gfx_end();
        frame++;
    }

    pop_to_menu();
    vmu_lcd_banner(NULL, NULL);
    softreset_leave();
    return 0;
}
