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
    TEXT_CODE
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

typedef struct {
    document_item_t items[MAX_ITEMS];
    char links[MAX_LINKS][MAX_URL];
    browser_image_t images[MAX_IMAGES];
    int item_count;
    int link_count;
    int image_count;
    int height;
    int truncated;
    int unsupported_count;
    char title[MAX_TITLE];
    char base_url[MAX_URL];
} browser_document_t;

typedef struct {
    unsigned char *data;
    size_t size;
    long status;
    int truncated;
    char content_type[96];
    char effective_url[MAX_URL];
    char error[160];
} fetch_result_t;

typedef int (*network_progress_callback_t)(uint64_t received, uint64_t total,
                                           void *userdata);

int network_init(void);
void network_shutdown(void);
void network_idle_poll(void);
void network_set_progress_callback(network_progress_callback_t callback,
                                   void *userdata);
int network_fetch(const char *url, size_t limit, fetch_result_t *out);
int resolve_url(const char *base, const char *reference, char *out, size_t out_size);
void fetch_result_free(fetch_result_t *result);

void document_init(browser_document_t *doc, const char *base_url);
void document_free(browser_document_t *doc);
void document_parse_html(browser_document_t *doc, const char *html, size_t size);
void document_make_error(browser_document_t *doc, const char *title, const char *message);
void document_load_images(browser_document_t *doc);
void document_reflow(browser_document_t *doc);

void render_browser(const browser_document_t *doc, int scroll_y, int mouse_x,
                    int mouse_y, int focused_link, const char *address,
                    int editing, const char *status);

#endif
