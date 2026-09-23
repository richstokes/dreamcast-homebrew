#ifndef DREAMCAST_BROWSER_H
#define DREAMCAST_BROWSER_H

#include <stddef.h>
#include <stdint.h>

#define SCREEN_W 640
#define SCREEN_H 480
#define PAGE_TOP 72
#define PAGE_MARGIN 24
#define PAGE_WIDTH (SCREEN_W - PAGE_MARGIN * 2)

#define MAX_URL 512
#define MAX_TITLE 96
#define MAX_TEXT 96
#define MAX_ITEMS 512
#define MAX_LINKS 96
#define MAX_IMAGES 6
#define MAX_HISTORY 8
#define MAX_DOCUMENT_BYTES (512 * 1024)
#define MAX_IMAGE_BYTES (24 * 1024)
#define MAX_PAGE_IMAGE_BYTES (64 * 1024)
#define MIN_IMAGE_FETCH_BYTES (4 * 1024)

typedef enum {
    ITEM_TEXT,
    ITEM_IMAGE,
    ITEM_RULE,
    ITEM_NOTICE
} item_type_t;

typedef enum {
    TEXT_NORMAL,
    TEXT_HEADING,
    TEXT_LINK,
    TEXT_MUTED,
    TEXT_CODE,
    TEXT_STRONG,
    TEXT_EMPHASIS
} text_style_t;

typedef struct {
    uint16_t *pixels;
    int width;
    int height;
    char url[MAX_URL];
    char alt[64];
    int loaded;
} browser_image_t;

typedef struct {
    item_type_t type;
    text_style_t style;
    int x;
    int y;
    int width;
    int height;
    int link_id;
    int image_id;
    char text[MAX_TEXT];
} document_item_t;

#define MAX_FORMS 8
#define MAX_FIELDS 64
#define MAX_FIELD_VALUE 512
#define MAX_OPTIONS 128
typedef struct { char action[MAX_URL]; int post; int valid; } browser_form_t;
typedef struct {
    char name[64], value[MAX_FIELD_VALUE], type[16];
    char label[48]; /* visible text of a submit button */
    int form, checked, link, item, maxlength, disabled;
    int caret; /* insertion point while being edited, otherwise -1 */
    int option_first, option_count, selected; /* <select> options */
} browser_field_t;
typedef struct {
    char value[64];
    char label[48];
} browser_option_t;
typedef struct {
    browser_form_t forms[MAX_FORMS];
    browser_field_t fields[MAX_FIELDS];
    browser_option_t options[MAX_OPTIONS];
    int form_count, field_count, option_count;
    document_item_t items[MAX_ITEMS];
    char links[MAX_LINKS][MAX_URL];
    browser_image_t images[MAX_IMAGES];
    int item_count;
    int link_count;
    int image_count;
    int height;
    int truncated;
    int unsupported_count;
    /* Bumped by every change to the laid-out page, so the renderer can
       tell when the page area must be redrawn. */
    unsigned generation;
    char title[MAX_TITLE];
    char base_url[MAX_URL];
} browser_document_t;

void document_touch(browser_document_t *doc);

typedef struct {
    unsigned char *data;
    size_t size;
    long status;
    int truncated;
    int cancelled;
    char content_type[96];
    char effective_url[MAX_URL];
    char error[160];
} fetch_result_t;

typedef int (*network_progress_callback_t)(uint64_t received, uint64_t total,
                                           void *userdata);

int network_init(void);
void network_shutdown(void);
void network_set_progress_callback(network_progress_callback_t callback,
                                   void *userdata);
int network_fetch(const char *url, size_t limit, fetch_result_t *out);
int network_post(const char *url, const char *body, size_t limit, fetch_result_t *out);
int network_same_origin(const char *a, const char *b);
void document_refresh_field(browser_document_t *doc, int field);
int document_field_is_text(const browser_field_t *field);
int document_field_is_submit(const browser_field_t *field);
void document_toggle_field(browser_document_t *doc, int field);
void document_select_option(browser_document_t *doc, int field, int option);
int resolve_url(const char *base, const char *reference, char *out, size_t out_size);
void fetch_result_free(fetch_result_t *result);

void document_init(browser_document_t *doc, const char *base_url);
void document_free(browser_document_t *doc);
/* content_type may be NULL; its charset and text/plain type are honored. */
void document_parse_html(browser_document_t *doc, const char *html, size_t size,
                         const char *content_type);
void document_mark_shortened(browser_document_t *doc, const char *message);
void document_make_error(browser_document_t *doc, const char *title, const char *message);
void document_load_images(browser_document_t *doc);
void document_reflow(browser_document_t *doc);

/* On-screen keyboard (osk.c): four character rows and one action row. */
#define OSK_ROWS 5
#define OSK_TOP 312
typedef enum {
    OSK_CHAR,
    OSK_SHIFT,
    OSK_SPACE,
    OSK_BACKSPACE,
    OSK_TEXT,
    OSK_CANCEL,
    OSK_DONE
} osk_action_t;
typedef struct {
    osk_action_t action;
    char ch;
    const char *text;
    const char *label;
} osk_key_t;
int osk_columns(int row);
void osk_key(int row, int column, int shift, osk_key_t *key);
void osk_move(int *row, int *column, int row_delta, int column_delta);
void osk_key_rect(int row, int column, int *x, int *y, int *w, int *h);
int osk_hit(int x, int y, int *row, int *column);

/* Address-bar interpretation (address.c). */
int address_is_search(const char *text);
int address_resolve(const char *text, const char *search_prefix, char *out,
                    size_t out_size);

/* Bookmarks (bookmarks.c) and their VMU record format. */
#define MAX_BOOKMARKS 16
#define BOOKMARK_TITLE 64
#define BOOKMARKS_HEADER_BYTES 20
/* Header, the longest possible lines, and a terminator for snprintf. */
#define BOOKMARKS_MAX_BYTES (BOOKMARKS_HEADER_BYTES + \
                             MAX_BOOKMARKS * (MAX_URL + BOOKMARK_TITLE) + 1)
typedef struct {
    char url[MAX_URL];
    char title[BOOKMARK_TITLE];
} bookmark_t;
typedef struct {
    bookmark_t items[MAX_BOOKMARKS];
    int count;
} bookmark_list_t;
int bookmarks_find(const bookmark_list_t *list, const char *url);
int bookmarks_add(bookmark_list_t *list, const char *url, const char *title);
int bookmarks_remove(bookmark_list_t *list, int index);
size_t bookmarks_serialize(const bookmark_list_t *list, unsigned char *out,
                           size_t out_size);
int bookmarks_deserialize(bookmark_list_t *list, const unsigned char *data,
                          size_t size);
size_t bookmarks_page_html(const bookmark_list_t *list, const char *candidate_url,
                           const char *candidate_title, const char *home_url,
                           const char *storage_note, char *out, size_t out_size);

typedef struct {
    int scroll_y;
    int mouse_x;
    int mouse_y;
    int focused_link;
    const char *address;
    int editing;
    int address_caret;
    int address_selected;
    int can_go_back;
    int can_go_forward;
    int toolbar; /* address bar and status footer are shown */
    int show_help;
    int osk_open;
    int osk_row;
    int osk_column;
    int osk_shift;
    const char *status;
} browser_view_t;

/* Frames are composed in main RAM and only the rows that changed since the
   previous frame are drawn and copied to video RAM (render.c). */
void render_frame(const browser_document_t *doc, const browser_view_t *view);
int render_present_ready(void);
void render_present(void);
void render_invalidate(void);
/* The composed frame, SCREEN_W * SCREEN_H RGB565 pixels (for tests). */
const uint16_t *render_frame_pixels(void);
void render_browser(const browser_document_t *doc, const browser_view_t *view);
/* Draws every row and copies the whole frame, for benchmarks and tests. */
void render_draw(const browser_document_t *doc, const browser_view_t *view);
#ifdef BROWSER_FRAME_DUMP
void render_dump_frame(const char *label);
#endif

#endif
