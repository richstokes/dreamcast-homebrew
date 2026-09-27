#ifndef DREAMCAST_BROWSER_APP_H
#define DREAMCAST_BROWSER_APP_H

/* What the browser's own modules share: the state of the page on screen
   and of the user's editing, and the few functions each module offers the
   others. browser.h describes the libraries underneath them. */

#include "browser.h"
#include "storage.h"

#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/maple/mouse.h>
#include <stdint.h>

#ifndef BROWSER_HOME_URL
#ifdef BROWSER_PERF_SELF_TEST
#define BROWSER_HOME_URL "about:bookmarks"
#else
#define BROWSER_HOME_URL "https://appsbyrich.com/"
#endif
#endif

/* Address-bar text that is not a location is searched here. */
#ifndef BROWSER_SEARCH_URL
#define BROWSER_SEARCH_URL "https://lite.duckduckgo.com/lite/?q="
#endif

#define BOOKMARKS_URL "about:bookmarks"

#if defined(BROWSER_HISTORY_SELF_TEST) || defined(BROWSER_PERF_SELF_TEST) || \
    defined(BROWSER_FORM_SELF_TEST) || defined(BROWSER_LOADING_SELF_TEST) || \
    defined(BROWSER_SITE_SELF_TEST)
#define BROWSER_SELF_TEST 1
#endif

/* ---- State (dreamcast-browser.c) ---------------------------------------- */

extern browser_document_t document;
extern char address[MAX_URL];
extern char current_url[MAX_URL];
extern char status_text[96];
extern int scroll_y;
extern int mouse_x;
extern int mouse_y;
extern int focused_link;
/* The last thing to choose a link was the pointer, not Tab or Y. */
extern int pointer_active;
extern int redraw_needed;
extern int show_help;
extern int toolbar_hidden;
extern int quit_requested;

/* Editing: the address bar, or the form field with this index. */
extern int editing;
extern int address_caret;
extern int address_selected;
extern int editing_field;
extern char field_backup[MAX_FIELD_VALUE];
extern int select_backup;
extern int osk_open;
extern int osk_row;
extern int osk_column;
extern int osk_shift;
extern int exit_armed;
extern int escape_released;

extern bookmark_list_t bookmarks;
extern char bookmark_candidate_url[MAX_URL];
extern char bookmark_candidate_title[BOOKMARK_TITLE];
extern int storage_enabled;

/* The downloaded HTML of the page on screen, for switching between reader
   and full-page views. The page cache owns it; internal pages have none. */
extern const fetch_result_t *page_source;
extern int prefer_reader;
extern int images_requested;

/* Set while something is loading, to the word the status line uses for it. */
extern const char *loading_label;
extern int loading_cancelled;

int toolbar_shown(void);
int page_top(void);
void current_view(browser_view_t *out);
void compose(void);
void redraw(void);
int frame_step(int dispatch);
#ifdef BROWSER_PROFILE
extern unsigned prof_keys;
#endif

/* ---- Loading and history (loader.c) ------------------------------------- */

enum {
    LOAD_OK = 0,
    LOAD_FAILED = -1,
    LOAD_CANCELED = -2,
    LOAD_STARTED = 1 /* in progress: the frame loop will finish it */
};

typedef struct {
    char url[MAX_URL];
    int scroll_y;
} history_entry_t;

/* Loading is a state machine advanced once per frame. Nothing ever waits
   for the network: a transfer is started, the frame loop carries on reading
   input and drawing, and the phase ends when the worker reports back. */
typedef enum {
    PHASE_IDLE,
    PHASE_CONNECT, /* dialing, before the page can be requested */
    PHASE_PAGE,
    PHASE_IMAGES
} load_phase_t;

/* What a finished page load does to the history. */
typedef enum {
    NAV_PLAIN, /* nothing: the home page at startup */
    NAV_LINK,
    NAV_BACK,
    NAV_FORWARD,
    NAV_RELOAD
} nav_kind_t;

typedef struct {
    load_phase_t phase;
    nav_kind_t kind;
    int result; /* how the most recent page load ended */
    char target[MAX_URL];
    char *body; /* form data being posted, or NULL */
    char previous_url[MAX_URL];
    char previous_address[MAX_URL];
    history_entry_t origin; /* the page being left */
    history_entry_t entry;  /* the history entry being returned to */
    image_loader_t images;
} load_state_t;
extern load_state_t load;

/* What was asked for while something else was loading. Only the latest
   request is kept; it is carried out once that load has ended. */
typedef enum {
    PENDING_NONE,
    PENDING_NAVIGATE,
    PENDING_BACK,
    PENDING_FORWARD,
    PENDING_RELOAD,
    PENDING_READER,
    PENDING_IMAGES,
    PENDING_INTERNAL
} pending_action_t;
typedef struct {
    pending_action_t action;
    char url[MAX_URL];
    char body[32768];
    int post;
} pending_request_t;
extern pending_request_t pending;

extern history_entry_t back_history[MAX_HISTORY];
extern history_entry_t forward_history[MAX_HISTORY];
extern int back_count;
extern int forward_count;

size_t available_memory(void);
void memory_report(const char *stage);
/* A navigation asked for while something is loading is kept as the pending
   request, and the load in progress is canceled. */
void begin_loading(const char *label);
void end_loading(void);
void history_push(history_entry_t *history, int *count, const char *url,
                  int saved_scroll, const char *name);
int load_internal(const char *url, const char *previous_url);
void start_load(nav_kind_t kind, const char *requested, const char *post_body);
void navigate_request(const char *requested, const char *post_body);
void navigate_to(const char *requested);
void open_typed_address(void);
void toggle_toolbar(void);
void toggle_bookmarks_page(void);
void bookmark_current_page(void);
void handle_internal_link(const char *link);
void navigate_back(void);
void navigate_forward(void);
void toggle_reader(void);
void load_page_images(void);
void reload_page(void);
/* Once per frame: advance what is loading, then carry out what was asked
   for while it was. */
void loader_tick(void);
void dispatch_pending(void);
void loader_shutdown(void);

/* ---- Input, editing and scrolling (input.c) ----------------------------- */

/* The analog stick's dead zone, and how close to the top or bottom of the
   page the pointer comes before a push scrolls instead. */
#define STICK_DEAD_ZONE 24
#define STICK_EDGE 6

int max_scroll(void);
void clamp_scroll(void);
int page_step(void);
int link_on_screen(int link);
void close_osk(void);
void open_osk(void);
void begin_address_edit(int want_osk);
void cancel_address_edit(void);
void finish_field_edit(int restore);
int handle_key(kbd_key_t key, kbd_mods_t mods, char ascii);
int process_keyboard(maple_device_t *keyboard);
void osk_type(kbd_key_t key, char ascii);
void osk_activate(void);
int process_mouse(maple_device_t *mouse);
void process_controller(maple_device_t *controller);

/* ---- Forms and links (forms.c) ------------------------------------------ */

void form_encoding(const char *text, char *out, size_t size);
int build_form_body(int field_index, char *body, size_t size, size_t *out_used);
void submit_form(int field_index);
void begin_field_edit(int index, int want_osk);
void follow_link(int link_id, int want_osk);

/* ---- Self-tests (selftest.c) -------------------------------------------- */

#ifdef BROWSER_SELF_TEST
void run_self_tests(void);
/* Counts frames, so that a watchdog can tell when they have stopped. */
extern volatile unsigned frame_heartbeat;
#endif
#ifdef BROWSER_HISTORY_SELF_TEST
/* 1 cancels the next load at once; 2 cancels the next image load. */
extern int self_test_cancel_mode;
#endif
#ifdef BROWSER_LOADING_SELF_TEST
void loading_test_tick(void);
#endif

#endif
