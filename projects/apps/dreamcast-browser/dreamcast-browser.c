/* The browser's state, its frame loop and its start and finish. Loading is
   in loader.c, input in input.c and forms in forms.c. */

#include "app.h"

#include <dc/vblank.h>
#include <dc/video.h>
#include <kos.h>
#include <kos/sem.h>
#include <stdio.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

browser_document_t document;
char address[MAX_URL] = BROWSER_HOME_URL;
char current_url[MAX_URL];
char status_text[96];
int scroll_y;
int mouse_x = SCREEN_W / 2;
int mouse_y = SCREEN_H / 2;
int focused_link = -1;
int pointer_active;
int redraw_needed = 1;
int show_help;
int toolbar_hidden;
int quit_requested;

int editing;
int address_caret;
int address_selected;
int editing_field = -1;
char field_backup[MAX_FIELD_VALUE];
int select_backup;
int osk_open;
int osk_row = 1;
int osk_column;
int osk_shift;
int exit_armed;
int escape_released;

bookmark_list_t bookmarks;
char bookmark_candidate_url[MAX_URL];
char bookmark_candidate_title[BOOKMARK_TITLE];
int storage_enabled = 1;

const fetch_result_t *page_source;
int prefer_reader = 1;
int images_requested;

const char *loading_label;
int loading_cancelled;

/* Ctrl+B hides the toolbar for a full-height page; it comes back while
   the address is being edited or a page is loading. */
int toolbar_shown(void) {
    return !toolbar_hidden || editing || loading_label != NULL;
}

int page_top(void) {
    return toolbar_shown() ? PAGE_TOP : 0;
}

void current_view(browser_view_t *out) {
    browser_view_t view = {
        .scroll_y = scroll_y,
        .mouse_x = mouse_x,
        .mouse_y = mouse_y,
        .focused_link = focused_link,
        .address = address,
        .editing = editing,
        .address_caret = address_caret,
        .address_selected = address_selected,
        .can_go_back = back_count > 0,
        .can_go_forward = forward_count > 0,
        .toolbar = toolbar_shown(),
        .show_help = show_help,
        .osk_open = osk_open,
        .osk_row = osk_row,
        .osk_column = osk_column,
        .osk_shift = osk_shift,
        .status = status_text
    };
    *out = view;
}

/* Frame clock. The main loop sleeps until the vertical blank interrupt
   rather than polling on a timer, so input is sampled and frames are
   presented once per display refresh with no scheduler-tick rounding. */
static semaphore_t frame_signal;
static int vblank_handle = -1;
#ifdef BROWSER_PROFILE
static volatile uint32_t prof_vblank_at;
#endif

static void on_vblank(uint32_t code, void *data) {
    kthread_t *frame_thread = data;
    int waiting = sem_count(&frame_signal) < 0;
    (void)code;
#ifdef BROWSER_PROFILE
    prof_vblank_at = (uint32_t)timer_us_gettime64();
#endif
    sem_signal(&frame_signal);
    /* Signaling makes the thread runnable, but KOS otherwise waits for its
       next 10 ms scheduler tick. Resume a frame waiter as this IRQ returns. */
    if(waiting) thd_schedule_next(frame_thread);
}

static void frame_clock_init(void) {
    sem_init(&frame_signal, 0);
    vblank_handle = vblank_handler_add(on_vblank, thd_get_current());
    if(vblank_handle < 0)
        printf("browser: no vblank handler; falling back to timed frames\n");
}

static void frame_clock_shutdown(void) {
    if(vblank_handle >= 0) vblank_handler_remove(vblank_handle);
    vblank_handle = -1;
    sem_destroy(&frame_signal);
}

/* Blocks until the next vertical blank, coalescing any that passed while
   the loop was busy so a long page load is not followed by a burst. */
static void wait_for_vblank(void) {
    if(vblank_handle < 0) {
        vid_waitvbl();
        return;
    }
    sem_wait(&frame_signal);
    while(sem_trywait(&frame_signal) == 0)
        ;
}

/* KOS polls every Maple device at each vertical blank; its DMA completes
   about a millisecond later and only then queues new key presses. Waiting
   for it means this frame's input is this frame's, not the previous one's. */
static void wait_for_maple_poll(void) {
    uint64_t deadline = timer_us_gettime64() + 4000;
    while(maple_state.dma_in_progress && timer_us_gettime64() < deadline)
        thd_pass();
}

#ifdef BROWSER_PROFILE
static uint64_t prof_draw_us, prof_present_us, prof_wait_us, prof_input_us, prof_report_at;
static uint64_t prof_poll_us, prof_wake_us;
static unsigned prof_wake_max;
static unsigned prof_frames, prof_iters, prof_draw_max, prof_iter_max;
unsigned prof_keys;
#define PROF_BEGIN uint64_t prof_t0 = timer_us_gettime64()
#define PROF_ADD(acc) (acc += timer_us_gettime64() - prof_t0)
#else
#define PROF_BEGIN (void)0
#define PROF_ADD(acc) (void)0
#endif

/* Composes the changed rows now; presenting waits for the display. */
void compose(void) {
    browser_view_t view;
    current_view(&view);
#ifdef BROWSER_PROFILE
    {
        uint64_t t0 = timer_us_gettime64(), t1;
        render_frame(&document, &view);
        t1 = timer_us_gettime64();
        prof_draw_us += t1 - t0;
        if(t1 - t0 > prof_draw_max) prof_draw_max = (unsigned)(t1 - t0);
    }
#else
    render_frame(&document, &view);
#endif
}

static void present(void) {
    PROF_BEGIN;
    /* The buffer flipped away from last time is scanned out until the next
       vertical blank; sleep through that rather than spin. */
    while(!render_present_ready()) wait_for_vblank();
    render_present();
    PROF_ADD(prof_present_us);
#ifdef BROWSER_PROFILE
    prof_frames++;
#endif
}

void redraw(void) {
    compose();
    present();
}

static void init_video(void) {
    vid_mode_t double_buffered;

    /* Let KOS select the correct VGA/NTSC/PAL timing, then reuse that mode
       with exactly two framebuffers instead of DM_MULTIBUFFER's 13. */
    vid_set_mode(DM_640x480, PM_RGB565);
    double_buffered = *vid_mode;
    double_buffered.fb_count = 2;
    vid_set_mode_ex(&double_buffered);
    /* Show the (blank) second buffer so the first frame is drawn off screen. */
    vid_flip(-1);
}

/* One frame: wait for the display, read input, advance the loader, act on
   what the input asked for, and draw. Returns nonzero to exit. */
int frame_step(int dispatch) {
    int quit = 0;
#ifdef BROWSER_PROFILE
    uint64_t iter_start = timer_us_gettime64();
#endif
    {
        PROF_BEGIN;
        wait_for_vblank();
        PROF_ADD(prof_wait_us);
#ifdef BROWSER_PROFILE
        {
            unsigned wake = (unsigned)((uint32_t)timer_us_gettime64() - prof_vblank_at);
            prof_wake_us += wake;
            if(wake > prof_wake_max) prof_wake_max = wake;
        }
#endif
    }
    {
        PROF_BEGIN;
        wait_for_maple_poll();
        PROF_ADD(prof_poll_us);
    }
    {
        PROF_BEGIN;
        quit |= process_keyboard(maple_enum_type(0, MAPLE_FUNC_KEYBOARD));
        process_mouse(maple_enum_type(0, MAPLE_FUNC_MOUSE));
        process_controller(maple_enum_type(0, MAPLE_FUNC_CONTROLLER));
        PROF_ADD(prof_input_us);
    }
    quit |= quit_requested;
#ifdef BROWSER_SELF_TEST
    frame_heartbeat++;
#endif
#ifdef BROWSER_HISTORY_SELF_TEST
    if(self_test_cancel_mode == 1 ||
       (self_test_cancel_mode == 2 && loading_label &&
        !strcmp(loading_label, "image"))) {
        self_test_cancel_mode = 0;
        loading_cancelled = 1;
    }
#endif
#ifdef BROWSER_LOADING_SELF_TEST
    if(load.phase != PHASE_IDLE) loading_test_tick();
#endif
    loader_tick();
    if(dispatch && !quit) dispatch_pending();
    if(redraw_needed) {
        redraw_needed = 0;
        compose();
        present();
    }
#ifdef BROWSER_PROFILE
    {
        uint64_t now = timer_us_gettime64();
        unsigned iter = (unsigned)(now - iter_start);
        prof_iters++;
        if(iter > prof_iter_max) prof_iter_max = iter;
        if(!prof_report_at) prof_report_at = now + 2000000;
        if(now >= prof_report_at) {
            printf("PROF iters %u frames %u keys %u | wait %lu us/it poll %lu us/it wake %lu us/it (max %u) input %lu us/it draw %lu us/frame (max %u) present %lu us/frame | iter max %u us\n",
                   prof_iters, prof_frames, prof_keys,
                   (unsigned long)(prof_wait_us / (prof_iters ? prof_iters : 1)),
                   (unsigned long)(prof_poll_us / (prof_iters ? prof_iters : 1)),
                   (unsigned long)(prof_wake_us / (prof_iters ? prof_iters : 1)), prof_wake_max,
                   (unsigned long)(prof_input_us / (prof_iters ? prof_iters : 1)),
                   (unsigned long)(prof_draw_us / (prof_frames ? prof_frames : 1)), prof_draw_max,
                   (unsigned long)(prof_present_us / (prof_frames ? prof_frames : 1)),
                   prof_iter_max);
            printf("PROF state editing %d address_length %u\n", editing, (unsigned)strlen(address));
            prof_iters = prof_frames = prof_keys = prof_draw_max = prof_iter_max = 0;
            prof_input_us = prof_draw_us = prof_present_us = prof_wait_us = 0;
            prof_poll_us = prof_wake_us = prof_wake_max = 0;
            prof_report_at = now + 2000000;
        }
    }
#endif
    return quit;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    init_video();
    frame_clock_init();
    storage_load_bookmarks(&bookmarks);
    document_make_error(&document, "Dreamcast Browser",
        "Starting network. With a keyboard, F6 opens the address bar and F1 lists "
        "every shortcut. With a controller, X opens the address bar, the stick "
        "moves the pointer and Start opens bookmarks and the menu.");
    snprintf(status_text, sizeof(status_text), "Starting network...");
    redraw();

    /* An Ethernet adapter is ready by now. Without one, asking for the home
       page dials the modem first. */
    start_load(NAV_PLAIN, address, NULL);

#ifdef BROWSER_SELF_TEST
    run_self_tests();
#endif
#ifdef BROWSER_EXIT_AFTER_TESTS
    /* Lets a scripted run exercise the shutdown path as well. */
    quit_requested = 1;
#endif
#ifdef BROWSER_PROFILE
    /* Startup, network loads, and the synthetic tests aren't interactive frames. */
    prof_iters = prof_frames = prof_keys = prof_draw_max = prof_iter_max = 0;
    prof_input_us = prof_draw_us = prof_present_us = prof_wait_us = 0;
    prof_poll_us = prof_wake_us = prof_wake_max = 0;
    prof_report_at = 0;
#endif
    while(!frame_step(1))
        ;

    frame_clock_shutdown();
    document_free(&document);
    loader_shutdown();
    printf("browser: clean shutdown\n");
    return 0;
}
