/* Loading and history: the state machine that fetches pages and images one
   frame at a time, what is kept for Back and Forward, and the browser's own
   internal pages. */

#include "app.h"

#include <arch/arch.h>
#include <arch/stack.h>
#include <arch/timer.h>
#include <kos/mm.h>
#include <kos/net.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

load_state_t load;
pending_request_t pending;
static int network_ready;
static uint64_t last_progress_draw;
static unsigned progress_frame;

history_entry_t back_history[MAX_HISTORY];
history_entry_t forward_history[MAX_HISTORY];
int back_count;
int forward_count;

size_t available_memory(void) {
    struct mallinfo info = mallinfo();
    uintptr_t top = _arch_mem_top - THD_KERNEL_STACK_SIZE;
    uintptr_t used = (uintptr_t)mm_sbrk(0);
    return info.fordblks + (top > used ? top - used : 0);
}

void memory_report(const char *stage) {
    struct mallinfo info = mallinfo();
    printf("browser: memory %s: allocated %lu KiB, available %lu KiB, document %lu KiB "
           "+ %lu KiB text, %d cached page(s) %lu KiB\n",
           stage, (unsigned long)info.uordblks / 1024,
           (unsigned long)available_memory() / 1024,
           (unsigned long)sizeof(document) / 1024,
           (unsigned long)document.arena_bytes / 1024, page_cache_count(),
           (unsigned long)page_cache_bytes() / 1024);
}

/* A download, its TLS session and the page laid out from it need room.
   Pages kept for Back and Forward are the first thing to give it up. */
#define MEMORY_HEADROOM (4 * 1024 * 1024)
static void ensure_headroom(void) {
    while(available_memory() < MEMORY_HEADROOM && page_cache_evict())
        ;
}

/* Navigation during a transfer replaces the pending intent and cancels the
   current transfer. It is dispatched only after that transfer has ended. */
static int defer_action(pending_action_t action, const char *url, const char *body) {
    /* A newer action can arrive after the canceled transfer has ended but
       before the main loop dispatches the previous pending action. It still
       replaces that intent instead of running ahead of stale navigation. */
    if(!loading_label && pending.action == PENDING_NONE) return 0;
    pending.action = action;
    snprintf(pending.url, sizeof(pending.url), "%s", url ? url : "");
    memset(pending.body, 0, sizeof(pending.body));
    pending.post = body != NULL;
    if(body) snprintf(pending.body, sizeof(pending.body), "%s", body);
    if(loading_label) loading_cancelled = 1;
    return 1;
}

/* Shows how the transfer in progress is getting on, a few times a second. */
static void update_progress(void) {
    static const char spinner[] = "|/-\\";
    uint32_t received = 0, total = 0;
    uint64_t now = timer_ms_gettime64();
    const char *connection = NULL;

    if(editing || editing_field >= 0 || show_help ||
       (last_progress_draw && now - last_progress_draw < 200))
        return;
    last_progress_draw = now;
    if(load.phase == PHASE_CONNECT) connection = modem_connect_status();
    else network_progress(&received, &total);
    if(loading_cancelled)
        snprintf(status_text, sizeof(status_text), "Canceling %s...", loading_label);
    else if(connection)
        snprintf(status_text, sizeof(status_text), "Connecting: %.30s %c | Esc/B cancel",
                 connection, spinner[progress_frame++ & 3]);
    else if(total)
        snprintf(status_text, sizeof(status_text),
                 "Loading %s %lu/%luK %c | Esc/B cancel", loading_label,
                 (unsigned long)(received / 1024),
                 (unsigned long)((total + 1023) / 1024),
                 spinner[progress_frame++ & 3]);
    else if(received)
        snprintf(status_text, sizeof(status_text),
                 "Loading %s %luK %c | Esc/B cancel", loading_label,
                 (unsigned long)(received / 1024), spinner[progress_frame++ & 3]);
    else
        snprintf(status_text, sizeof(status_text),
                 "Connecting %s %c | Esc/B cancel", loading_label,
                 spinner[progress_frame++ & 3]);
    redraw_needed = 1;
}

void begin_loading(const char *label) {
    loading_label = label;
    last_progress_draw = 0;
    loading_cancelled = 0;
}

void end_loading(void) {
    loading_label = NULL;
}

static void normalize_address(char *url, size_t size) {
    char temp[MAX_URL];
    if(strstr(url, "://") || !strncmp(url, "about:", 6)) return;
    snprintf(temp, sizeof(temp), "https://%.*s", (int)sizeof(temp) - 9, url);
    snprintf(url, size, "%s", temp);
}

void history_push(history_entry_t *history, int *count,
                         const char *url, int saved_scroll,
                         const char *name) {
    history_entry_t *entry;

    if(!url[0]) return;
    if(*count && !strcmp(history[*count - 1].url, url)) {
        history[*count - 1].scroll_y = saved_scroll;
        return;
    }
    if(*count == MAX_HISTORY) {
        memmove(&history[0], &history[1],
                sizeof(history[0]) * (MAX_HISTORY - 1));
        (*count)--;
    }
    entry = &history[(*count)++];
    snprintf(entry->url, sizeof(entry->url), "%s", url);
    entry->scroll_y = saved_scroll;
    printf("browser: %s history saved %s (%d/%d)\n",
           name, entry->url, *count, MAX_HISTORY);
}

static int is_web_url(const char *url) {
    return !strncmp(url, "https://", 8) || !strncmp(url, "http://", 7);
}

/* Builds an internal page in place of a network response. */
int load_internal(const char *url, const char *previous_url) {
    static char html[65536];
    char note[96];
    int found = !strcmp(url, BOOKMARKS_URL);

    /* The page being left is what "Bookmark this page" offers. */
    if(found && is_web_url(previous_url)) {
        snprintf(bookmark_candidate_url, sizeof(bookmark_candidate_url), "%s",
                 previous_url);
        snprintf(bookmark_candidate_title, sizeof(bookmark_candidate_title), "%.*s",
                 (int)sizeof(bookmark_candidate_title) - 1, document.title);
    }
    page_source = NULL;
    page_cache_leave();
    images_requested = 0;
    document_free(&document);
    if(found) {
        storage_describe(note, sizeof(note));
        bookmarks_page_html(&bookmarks, bookmark_candidate_url, bookmark_candidate_title,
                            BROWSER_HOME_URL, note, html, sizeof(html));
        document_init(&document, url);
        document_parse_html(&document, html, strlen(html),
                            "text/html; charset=windows-1252");
    } else {
        document_make_error(&document, "Unknown internal page",
                            "This browser has no internal page at that address.");
    }
    snprintf(current_url, sizeof(current_url), "%s", url);
    snprintf(address, sizeof(address), "%s", url);
    snprintf(status_text, sizeof(status_text), "%s", document.title);
    scroll_y = 0;
    focused_link = -1;
    redraw_needed = 1;
    return found ? 0 : LOAD_FAILED;
}

static const char reader_hint[] = " | F7 view | F4 images";

/* Lays out a downloaded page and makes it the one on screen. */
static void show_source(const fetch_result_t *source, const char *url) {
    const char *fragment;
    int attempt;

    if(editing_field >= 0) finish_field_edit(0);
    page_source = source;
    for(attempt = 0; attempt < 2; ++attempt) {
        document_free(&document);
        document_init(&document, source->effective_url);
        document_parse_html_mode(&document, (const char *)source->data, source->size,
                                 source->content_type, prefer_reader);
        /* Short of memory: give up the pages kept for Back, and lay out again. */
        if(!(document.limit_flags & DOCUMENT_LIMIT_MEMORY) || !page_cache_evict()) break;
        while(page_cache_evict())
            ;
    }
    if(source->truncated)
        document_mark_shortened(&document,
            "[Download limit reached: part of this page is unavailable]");
    if(!editing) snprintf(address, sizeof(address), "%s", url);
    snprintf(current_url, sizeof(current_url), "%s", url);
    scroll_y = 0;
    fragment = strchr(current_url, '#');
    if(fragment) {
        int y = document_anchor_y(&document, fragment);
        if(y >= 0) scroll_y = y;
    }
    clamp_scroll();
    focused_link = -1;
    images_requested = 0;
    snprintf(status_text, sizeof(status_text), "%s%s%s",
             document.reader_active ? "Reader" : "Page ready",
             document.truncated ? " [shortened]" : "", reader_hint);
    printf("browser: page ready: %d items, %d links, reader %d, shortened %d\n",
           document.item_count, document.link_count, document.reader_active, document.truncated);
    memory_report("page ready");
    redraw_needed = 1;
}

/* Replaces the page with an explanation of why the address did not load. */
static int fail_page(const char *title, const char *message, const char *status) {
    page_source = NULL;
    page_cache_leave();
    snprintf(current_url, sizeof(current_url), "%s", load.target);
    if(editing_field >= 0) finish_field_edit(0);
    document_free(&document);
    document_make_error(&document, title, message);
    snprintf(status_text, sizeof(status_text), "%s", status);
    scroll_y = 0;
    focused_link = -1;
    images_requested = 0;
    redraw_needed = 1;
    return LOAD_FAILED;
}

/* The page on screen stays: only the address bar has to be put back. */
static int keep_page(const char *status) {
    snprintf(current_url, sizeof(current_url), "%s", load.previous_url);
    if(!editing)
        snprintf(address, sizeof(address), "%s",
                 load.previous_url[0] ? load.previous_url : load.previous_address);
    snprintf(status_text, sizeof(status_text), "%s", status);
    printf("browser: page load canceled; keeping %s\n",
           current_url[0] ? current_url : "startup page");
    redraw_needed = 1;
    return LOAD_CANCELED;
}

static void release_load_body(void) {
    if(!load.body) return;
    memset(load.body, 0, strlen(load.body));
    free(load.body);
    load.body = NULL;
}

static int link_ready(void) {
    return net_default_dev && (!modem_in_use() || modem_link_up());
}

static int bring_up_network(void) {
    const uint8_t *ip;
    if(network_ready) return 0;
    if(!net_default_dev || network_init() < 0) return -1;
    network_ready = 1;
    ip = net_default_dev->ip_addr;
    printf("browser: network %s, IP %u.%u.%u.%u\n", net_default_dev->name,
           ip[0], ip[1], ip[2], ip[3]);
    return 0;
}

/* Asks the network for load.target, dialing first when nothing is
   connected. Returns LOAD_STARTED, or how the load has already ended. */
static int request_page(void) {
    fetch_request_t request = { .url = load.target, .body = load.body,
                                .kind = FETCH_PAGE, .limit = MAX_DOCUMENT_BYTES,
                                .timeout_ms = network_page_timeout() };
    char error[160];
    char message[256];

    if(!link_ready()) {
        /* No Ethernet adapter, or the call has dropped: use the modem. */
        if(modem_connect_start() < 0)
            return fail_page("No network connection",
                             "The modem could not be started. Check the hardware or "
                             "Flycast network settings.", "Offline");
        load.phase = PHASE_CONNECT;
        begin_loading("connection");
        snprintf(status_text, sizeof(status_text), "Connecting: finding modem");
        redraw_needed = 1;
        return LOAD_STARTED;
    }
    if(bring_up_network() < 0)
        return fail_page("Network startup failed",
                         "The secure HTTP client could not be initialized.", "Offline");
    ensure_headroom();
    if(network_start(&request, error, sizeof(error)) < 0) {
        snprintf(message, sizeof(message), "Could not load this address: %s", error);
        return fail_page("Page load failed", message, "Network error");
    }
    load.phase = PHASE_PAGE;
    begin_loading("page");
    snprintf(status_text, sizeof(status_text), "Connecting page...");
    redraw_needed = 1;
    return LOAD_STARTED;
}

/* Starts showing an address. Pages that need no download are shown at
   once; otherwise the transfer is started and LOAD_STARTED returned. */
static int begin_load(const char *requested, const char *post_body, int force_reload) {
    const fetch_result_t *kept;
    const char *fragment;
    size_t target_base, current_base;

    /* requested may point into the page that is about to be replaced. */
    snprintf(load.target, sizeof(load.target), "%s", requested);
    release_load_body();
    if(post_body && !(load.body = strdup(post_body))) {
        snprintf(status_text, sizeof(status_text), "Not enough memory to submit form");
        redraw_needed = 1;
        return LOAD_CANCELED;
    }
    finish_field_edit(0);
    editing = 0;
    address_selected = 0;
    show_help = 0;
    close_osk();
    snprintf(load.previous_url, sizeof(load.previous_url), "%s", current_url);
    snprintf(load.previous_address, sizeof(load.previous_address), "%s", address);
    normalize_address(load.target, sizeof(load.target));
    /* In-page navigation never needs another HTTP request. */
    fragment = strchr(load.target, '#');
    target_base = strcspn(load.target, "#");
    current_base = strcspn(current_url, "#");
    if(!force_reload && !post_body && (fragment || strchr(current_url, '#')) &&
       target_base == current_base &&
       !strncmp(load.target, current_url, target_base)) {
        int y = fragment ? document_anchor_y(&document, fragment) : 0;
        if(y < 0) {
            snprintf(address, sizeof(address), "%s", current_url);
            snprintf(status_text, sizeof(status_text), "Section unavailable in this view; F7 switches view");
            redraw_needed = 1;
            return LOAD_CANCELED;
        }
        snprintf(current_url, sizeof(current_url), "%s", load.target);
        snprintf(address, sizeof(address), "%s", load.target);
        scroll_y = y;
        clamp_scroll();
        focused_link = -1;
        snprintf(status_text, sizeof(status_text), "%.90s", document.title);
        redraw_needed = 1;
        return LOAD_OK;
    }
    if(!strncmp(load.target, "about:", 6))
        return load_internal(load.target, load.previous_url);
    /* Back and Forward return to the page as it was downloaded. */
    if((load.kind == NAV_BACK || load.kind == NAV_FORWARD) && !post_body &&
       (kept = page_cache_find(load.target))) {
        printf("browser: showing the kept copy of %s\n", load.target);
        show_source(kept, load.target);
        return LOAD_OK;
    }
    snprintf(address, sizeof(address), "%s", load.target);
    return request_page();
}

/* Takes the finished page transfer and shows what it brought. */
static int complete_page(int code, fetch_result_t *result) {
    const fetch_result_t *kept;
    char message[256];

    if(code < 0 || loading_cancelled) {
        if(result->cancelled || loading_cancelled) {
            fetch_result_free(result);
            return keep_page("Canceled; page unchanged");
        }
        /* Whatever can be released is, so that trying again may work. */
        if(result->out_of_memory)
            while(page_cache_evict())
                ;
        snprintf(message, sizeof(message), "Could not load this address: %s", result->error);
        fetch_result_free(result);
        return fail_page("Page load failed", message, "Network error");
    }
    if((result->status < 200 || result->status >= 400) &&
       !strstr(result->content_type, "text/html")) {
        char status[32];
        snprintf(message, sizeof(message), "The server returned HTTP status %ld.",
                 result->status);
        snprintf(status, sizeof(status), "HTTP %ld", result->status);
        fetch_result_free(result);
        return fail_page("Server error", message, status);
    }
    if(result->content_type[0] && !strstr(result->content_type, "text/html") &&
       !strstr(result->content_type, "text/plain") &&
       !strstr(result->content_type, "application/xhtml")) {
        snprintf(message, sizeof(message), "Unsupported page type: %.90s",
                 result->content_type);
        fetch_result_free(result);
        return fail_page("Unsupported content", message, "Unsupported content");
    }
    kept = page_cache_store(result->effective_url, result);
    show_source(kept, kept->effective_url);
    return LOAD_OK;
}

/* Ends a page load, however it went, and settles the history. */
static void finish_load(int outcome) {
    load.phase = PHASE_IDLE;
    end_loading();
    release_load_body();
    load.result = outcome;
    switch(load.kind) {
    case NAV_LINK:
        if(outcome != LOAD_CANCELED) {
            history_push(back_history, &back_count, load.origin.url,
                         load.origin.scroll_y, "back");
            forward_count = 0;
        }
        break;
    case NAV_BACK:
        if(outcome == LOAD_OK) {
            back_count--;
            history_push(forward_history, &forward_count, load.origin.url,
                         load.origin.scroll_y, "forward");
            scroll_y = load.entry.scroll_y;
        }
        break;
    case NAV_FORWARD:
        if(outcome == LOAD_OK) {
            forward_count--;
            history_push(back_history, &back_count, load.origin.url,
                         load.origin.scroll_y, "back");
            scroll_y = load.entry.scroll_y;
        }
        break;
    case NAV_RELOAD:
        if(outcome == LOAD_OK) scroll_y = load.origin.scroll_y;
        break;
    default:
        break;
    }
    clamp_scroll();
    redraw_needed = 1;
}

void start_load(nav_kind_t kind, const char *requested, const char *post_body) {
    int outcome;
    load.kind = kind;
    snprintf(load.origin.url, sizeof(load.origin.url), "%s", current_url);
    load.origin.scroll_y = scroll_y;
    outcome = begin_load(requested, post_body, kind == NAV_RELOAD);
    if(outcome != LOAD_STARTED) finish_load(outcome);
}

void navigate_request(const char *requested, const char *post_body) {
    if(defer_action(PENDING_NAVIGATE, requested, post_body)) return;
    start_load(NAV_LINK, requested, post_body);
}

void navigate_to(const char *requested) {
    navigate_request(requested, NULL);
}

/* Opens address-bar text as a location or, failing that, as a search. */
void open_typed_address(void) {
    char target[MAX_URL];
    if(address_resolve(address, BROWSER_SEARCH_URL, target, sizeof(target)) < 0) {
        cancel_address_edit();
        return;
    }
    navigate_to(target);
}

void toggle_toolbar(void) {
    toolbar_hidden = !toolbar_hidden;
    clamp_scroll();
    snprintf(status_text, sizeof(status_text), "%s",
             toolbar_hidden ? "Address bar hidden; Ctrl+B shows it" : document.title);
    redraw_needed = 1;
}

void toggle_bookmarks_page(void) {
    if(!strcmp(current_url, BOOKMARKS_URL) && back_count) navigate_back();
    else if(strcmp(current_url, BOOKMARKS_URL)) navigate_to(BOOKMARKS_URL);
}

static void persist_bookmarks(const char *done) {
    int result = storage_enabled ? storage_save_bookmarks(&bookmarks) : STORAGE_OK;
    if(result == STORAGE_OK)
        snprintf(status_text, sizeof(status_text), "%s", done);
    else if(result == STORAGE_NO_VMU)
        snprintf(status_text, sizeof(status_text), "%s (no VMU to save to)", done);
    else
        snprintf(status_text, sizeof(status_text), "Could not write bookmarks to VMU");
    redraw_needed = 1;
}

void bookmark_current_page(void) {
    int result;
    if(!is_web_url(current_url)) {
        snprintf(status_text, sizeof(status_text), "Open a web page to bookmark it");
        redraw_needed = 1;
        return;
    }
    if(bookmarks_find(&bookmarks, current_url) >= 0) {
        snprintf(status_text, sizeof(status_text), "Already bookmarked");
        redraw_needed = 1;
        return;
    }
    result = bookmarks_add(&bookmarks, current_url, document.title);
    if(result == -1) {
        snprintf(status_text, sizeof(status_text), "Bookmarks full: remove one first");
        redraw_needed = 1;
        return;
    }
    persist_bookmarks("Bookmarked");
}

/* Refreshes the bookmarks page after a change without adding history. */
static void refresh_internal_page(void) {
    int saved_scroll = scroll_y;
    char saved_status[sizeof(status_text)];
    snprintf(saved_status, sizeof(saved_status), "%s", status_text);
    load_internal(current_url, current_url);
    scroll_y = saved_scroll;
    clamp_scroll();
    snprintf(status_text, sizeof(status_text), "%s", saved_status);
}

/* Actions exist only on internal pages, so a web page cannot exit the
   browser or change bookmarks by linking to them. */
void handle_internal_link(const char *link) {
    if(defer_action(PENDING_INTERNAL, link, NULL)) return;
    int action = !strcmp(link, "about:bookmark-add") || !strcmp(link, "about:exit") ||
                 !strncmp(link, "about:bookmark-remove?", 22);
    if(!action) {
        navigate_to(link);
        return;
    }
    if(strncmp(current_url, "about:", 6)) {
        snprintf(status_text, sizeof(status_text), "Blocked a web page's internal link");
        redraw_needed = 1;
        return;
    }
    if(!strcmp(link, "about:exit")) {
        quit_requested = 1;
    } else if(!strcmp(link, "about:bookmark-add")) {
        int result = bookmarks_add(&bookmarks, bookmark_candidate_url,
                                   bookmark_candidate_title);
        if(result == -1)
            snprintf(status_text, sizeof(status_text), "Bookmarks full: remove one first");
        else if(result >= 0)
            persist_bookmarks("Bookmarked");
        refresh_internal_page();
    } else if(!bookmarks_remove(&bookmarks, atoi(link + 22))) {
        persist_bookmarks("Bookmark removed");
        refresh_internal_page();
    }
}

void navigate_back(void) {
    if(defer_action(PENDING_BACK, NULL, NULL)) return;
    if(!back_count) {
        snprintf(status_text, sizeof(status_text), "No previous page");
        redraw_needed = 1;
        return;
    }
    load.entry = back_history[back_count - 1];
    printf("browser: back -> %s (%d remaining)\n", load.entry.url, back_count - 1);
    start_load(NAV_BACK, load.entry.url, NULL);
}

void navigate_forward(void) {
    if(defer_action(PENDING_FORWARD, NULL, NULL)) return;
    if(!forward_count) {
        snprintf(status_text, sizeof(status_text), "No next page");
        redraw_needed = 1;
        return;
    }
    load.entry = forward_history[forward_count - 1];
    printf("browser: forward -> %s (%d remaining)\n",
           load.entry.url, forward_count - 1);
    start_load(NAV_FORWARD, load.entry.url, NULL);
}

void toggle_reader(void) {
    if(defer_action(PENDING_READER, NULL, NULL)) return;
    if(!page_source || !page_source->data || !document.reader_available) {
        snprintf(status_text, sizeof(status_text), "No separate article area on this page");
        redraw_needed = 1;
        return;
    }
    finish_field_edit(0);
    prefer_reader = !document.reader_active;
    document_free(&document);
    document_init(&document, page_source->effective_url);
    document_parse_html_mode(&document, (const char *)page_source->data, page_source->size,
                             page_source->content_type, prefer_reader);
    if(page_source->truncated)
        document_mark_shortened(&document, "[Download limit reached: part of this page is unavailable]");
    scroll_y = 0;
    focused_link = -1;
    images_requested = 0;
    snprintf(status_text, sizeof(status_text), "%s%s%s",
             document.reader_active ? "Reader" : "Full page",
             document.truncated ? " [shortened]" : "", reader_hint);
    redraw_needed = 1;
}

static void finish_images(void) {
    load.phase = PHASE_IDLE;
    end_loading();
    clamp_scroll();
    snprintf(status_text, sizeof(status_text), "%s",
             loading_cancelled ? "Images canceled; page ready" : "Images ready; skipped files keep placeholders");
    memory_report("after images");
    redraw_needed = 1;
}

/* Requests the next image that has not been tried; ends the phase when
   there is none. An image that cannot even be requested counts as tried. */
static void next_image(void) {
    fetch_request_t request;
    fetch_result_t refused;
    while(image_loader_next(&load.images, &document, &request)) {
        /* Pictures already shown have used memory: keep room for this one. */
        ensure_headroom();
        memset(&refused, 0, sizeof(refused));
        if(network_start(&request, refused.error, sizeof(refused.error)) == 0) return;
        if(image_loader_finish(&load.images, &document, -1, &refused) < 0) break;
    }
    finish_images();
}

/* Lays the page out again around a picture that has arrived, keeping the
   reader's place when the picture is above what they are looking at. */
static void reflow_in_place(void) {
    int i, first = -1, offset = 0;
    for(i = 0; i < document.item_count && first < 0; ++i)
        if(document.items[i].y >= scroll_y) first = i;
    if(first >= 0) offset = document.items[first].y - scroll_y;
    document_reflow(&document);
    if(first >= 0 && scroll_y > 0) scroll_y = document.items[first].y - offset;
    clamp_scroll();
    redraw_needed = 1;
}

void load_page_images(void) {
    if(defer_action(PENDING_IMAGES, NULL, NULL)) return;
    if(images_requested || !document.image_count) {
        snprintf(status_text, sizeof(status_text), "%s",
                 images_requested ? "Images already attempted; reload to try again" : "No images on this page");
        redraw_needed = 1;
        return;
    }
    if(!link_ready() || bring_up_network() < 0) {
        snprintf(status_text, sizeof(status_text), "Not connected: reload the page first");
        redraw_needed = 1;
        return;
    }
    images_requested = 1;
    memory_report("before images");
    image_loader_begin(&load.images);
    load.phase = PHASE_IMAGES;
    begin_loading("image");
    redraw_needed = 1;
    next_image();
}

void reload_page(void) {
    if(defer_action(PENDING_RELOAD, NULL, NULL)) return;
    start_load(NAV_RELOAD, address, NULL);
}

void dispatch_pending(void) {
    pending_action_t action = pending.action;
    char url[MAX_URL];
    char *body = NULL;
    if(action == PENDING_NONE || loading_label) return;
    snprintf(url, sizeof(url), "%s", pending.url);
    if(pending.post) {
        body = strdup(pending.body);
        if(!body) {
            pending.action = PENDING_NONE;
            memset(pending.body, 0, sizeof(pending.body));
            snprintf(status_text, sizeof(status_text), "Not enough memory to submit form");
            redraw_needed = 1;
            return;
        }
    }
    pending.action = PENDING_NONE;
    pending.post = 0;
    memset(pending.body, 0, sizeof(pending.body));
    switch(action) {
    case PENDING_NAVIGATE: navigate_request(url, body); break;
    case PENDING_BACK: navigate_back(); break;
    case PENDING_FORWARD: navigate_forward(); break;
    case PENDING_RELOAD: reload_page(); break;
    case PENDING_READER: toggle_reader(); break;
    case PENDING_IMAGES: load_page_images(); break;
    case PENDING_INTERNAL: handle_internal_link(url); break;
    default: break;
    }
    if(body) { memset(body, 0, strlen(body)); free(body); }
}

/* Advances whatever is loading by one frame. */
void loader_tick(void) {
    fetch_result_t result;
    const char *status;
    int code = -1;
    int connected;

    switch(load.phase) {
    case PHASE_CONNECT:
        if(loading_cancelled) modem_connect_cancel();
        connected = modem_connect_poll(&status);
        if(!connected) {
            update_progress();
            break;
        }
        if(loading_cancelled) {
            finish_load(keep_page("Canceled; page unchanged"));
        } else if(connected < 0) {
            char message[256];
            snprintf(message, sizeof(message),
                     "%s. Press F5, or Start then the home page, to try again.", status);
            finish_load(fail_page("No network connection", message, "Offline"));
        } else {
            int outcome = request_page();
            if(outcome != LOAD_STARTED) finish_load(outcome);
        }
        break;
    case PHASE_PAGE:
        if(loading_cancelled) network_cancel();
        if(!network_poll(&result, &code)) {
            update_progress();
            break;
        }
        finish_load(complete_page(code, &result));
        break;
    case PHASE_IMAGES:
        if(loading_cancelled) network_cancel();
        if(!network_poll(&result, &code)) {
            update_progress();
            break;
        }
        if(image_loader_finish(&load.images, &document, code, &result) < 0) {
            clamp_scroll();
            finish_images();
        } else {
            reflow_in_place();
            next_image();
        }
        break;
    default:
        break;
    }
}

void loader_shutdown(void) {
    memset(pending.body, 0, sizeof(pending.body));
    release_load_body();
    if(network_ready) network_shutdown();
    free(load.images.decoded.pixels);
    load.images.decoded.pixels = NULL;
    page_cache_clear();
    modem_disconnect_link();
}
