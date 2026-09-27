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
#define MAX_ITEMS 12288
#define MAX_LINKS 4096
#define MAX_ANCHORS 2048
#define MAX_ANCHOR_NAME 128
#define MAX_IMAGES 12
#define MAX_HISTORY 8
#define MAX_DOCUMENT_BYTES (2 * 1024 * 1024)
#define MAX_IMAGE_BYTES (192 * 1024)
#define MAX_PAGE_IMAGE_BYTES (768 * 1024)
#define MIN_IMAGE_FETCH_BYTES (4 * 1024)
/* Decoded RGB565 pixels kept for one page, across all of its images. */
#define MAX_PAGE_IMAGE_PIXELS (1280 * 1024)

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
    TEXT_EMPHASIS,
    TEXT_SEPARATOR /* the bar between two cells of a data table */
} text_style_t;

typedef struct {
    uint16_t *pixels;
    int width;
    int height;
    char url[MAX_URL];
    char alt[64];
    int loaded;
} browser_image_t;

/* Item text and link addresses live in the document's arena, so a page
   costs what it contains rather than the largest page allowed. text is never
   NULL and holds fewer than MAX_TEXT characters; only a form control's text
   is rewritten after layout, within its own MAX_TEXT bytes. */
typedef struct {
    char *text;
    int y;
    int16_t x;
    int16_t width;
    int16_t height;
    int16_t link_id;
    uint8_t type;  /* item_type_t */
    uint8_t style; /* text_style_t */
    int8_t image_id;
} document_item_t;

typedef struct document_chunk document_chunk_t;

#define MAX_FORMS 8
#define MAX_FIELDS 64
#define MAX_FIELD_VALUE 512
#define MAX_OPTIONS 128
typedef struct { char action[MAX_URL]; int post; int valid; } browser_form_t;
typedef struct {
    char name[64], value[MAX_FIELD_VALUE], type[16];
    char label[48]; /* visible text of a submit button */
    int form, checked, link, item, maxlength, disabled, hidden;
    int caret; /* insertion point while being edited, otherwise -1 */
    int option_first, option_count, selected; /* <select> options */
} browser_field_t;
typedef struct {
    char value[64];
    char label[48];
} browser_option_t;
typedef struct {
    char *name;
    uint32_t hash; /* of name, so that most comparisons need not read it */
    int item; /* First following layout item; remains valid after image reflow. */
    unsigned priority; /* Referenced targets/headings displace incidental IDs. */
} browser_anchor_t;
enum {
    DOCUMENT_LIMIT_LAYOUT = 1,
    DOCUMENT_LIMIT_LINKS = 2,
    DOCUMENT_LIMIT_ANCHORS = 4,
    DOCUMENT_LIMIT_FORMS = 8,
    DOCUMENT_LIMIT_IMAGES = 16,
    DOCUMENT_LIMIT_MEMORY = 32
};
typedef struct {
    browser_form_t forms[MAX_FORMS];
    browser_field_t fields[MAX_FIELDS];
    browser_option_t options[MAX_OPTIONS];
    int form_count, field_count, option_count;
    document_item_t items[MAX_ITEMS];
    char *links[MAX_LINKS];
    document_chunk_t *chunks; /* arena holding item text, links and anchors */
    size_t arena_bytes;
    browser_image_t images[MAX_IMAGES];
    browser_anchor_t anchors[MAX_ANCHORS];
    int anchor_count;
    int item_count;
    int link_count;
    int image_count;
    int height;
    int truncated;
    int unsupported_count;
    unsigned limit_flags;
    int reader_available;
    int reader_active;
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
    int out_of_memory;
    char content_type[96];
    char effective_url[MAX_URL];
    char error[160];
} fetch_result_t;

typedef enum {
    FETCH_PAGE,
    FETCH_IMAGE
} fetch_kind_t;

/* Runs on the network worker after a successful transfer, so slow work such
   as decoding an image never holds up the frame loop. */
typedef void (*fetch_process_t)(fetch_result_t *result, void *userdata);

typedef struct {
    const char *url;
    const char *body; /* form data to POST, or NULL to GET */
    fetch_kind_t kind;
    size_t limit;
    unsigned timeout_ms;
    fetch_process_t process;
    void *userdata;
} fetch_request_t;

int network_init(void);
void network_shutdown(void);
/* One transfer at a time: start it, poll it once per frame, take its result. */
int network_start(const fetch_request_t *request, char *error, size_t error_size);
int network_active(void);
void network_cancel(void);
void network_progress(uint32_t *received, uint32_t *total);
int network_poll(fetch_result_t *out, int *code);
int network_fetch_wait(const char *url, const char *body, fetch_kind_t kind,
                       size_t limit, fetch_result_t *out);
void network_report_stall(void);
int network_slow_link(void);
unsigned network_page_timeout(void);
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
/* Reader mode selects a main/article region when one exists; otherwise the
   full page is retained. The caller keeps the source for reversible toggles. */
void document_parse_html_mode(browser_document_t *doc, const char *html, size_t size,
                              const char *content_type, int reader_requested);
/* fragment may start with '#'; returns -1 if no matching anchor exists. */
int document_anchor_y(const browser_document_t *doc, const char *fragment);
void document_mark_shortened(browser_document_t *doc, const char *message);
void document_make_error(browser_document_t *doc, const char *title, const char *message);
void document_reflow(browser_document_t *doc);

/* Optional page images (images.c). One image is downloaded at a time; the
   network worker decodes it, and the frame loop adopts the pixels. */
typedef enum {
    IMAGE_NOT_DECODED,
    IMAGE_DECODED,
    IMAGE_UNSUPPORTED,
    IMAGE_NO_MEMORY
} image_status_t;
typedef struct {
    uint16_t *pixels;
    int width;
    int height;
    image_status_t status;
} image_decode_t;
typedef struct {
    int index;          /* the image being downloaded, or -1 */
    size_t bytes_left;  /* download allowance for the page */
    size_t pixels_left; /* decoded-pixel allowance for the page */
    uint64_t deadline;
    image_decode_t decoded;
} image_loader_t;
void image_loader_begin(image_loader_t *loader);
/* Chooses the next image and describes its download. Returns 0 once every
   image has been attempted; the document has then been laid out again. */
int image_loader_next(image_loader_t *loader, browser_document_t *doc,
                      fetch_request_t *request);
/* Takes a finished download, releasing result. Returns 0 to carry on, or -1
   when the rest of the page's images were given up. */
int image_loader_finish(image_loader_t *loader, browser_document_t *doc,
                        int code, fetch_result_t *result);

/* Recently shown pages (cache.c). The cache owns what it is given; the
   page returned by store or find is the one on screen, and stays valid until
   another page is stored or found. */
#define PAGE_CACHE_ENTRIES 8
#define PAGE_CACHE_BYTES (3 * 1024 * 1024)
const fetch_result_t *page_cache_store(const char *url, fetch_result_t *source);
const fetch_result_t *page_cache_find(const char *url);
/* The page on screen no longer comes from the cache. */
void page_cache_leave(void);
/* Releases the least recently shown page that is not on screen. Returns 0
   when there is nothing left to release. */
int page_cache_evict(void);
void page_cache_clear(void);
size_t page_cache_bytes(void);
int page_cache_count(void);

/* Dial-up (modem.c): a Dreamcast modem, normally answered by a DreamPi. The
   connection is made on its own thread and polled from the frame loop. */
int modem_connect_start(void);
/* 0 while connecting, 1 once connected, -1 on failure; status names the
   current step, or explains the failure. */
int modem_connect_poll(const char **status);
const char *modem_connect_status(void);
void modem_connect_cancel(void);
int modem_in_use(void);
int modem_link_up(void);
void modem_disconnect_link(void);

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
