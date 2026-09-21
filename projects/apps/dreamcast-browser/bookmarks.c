/* Bookmark list, its VMU record format, and the internal bookmarks page.
   Storage I/O lives in storage.c; everything here is platform-independent. */

#include "browser.h"

#include <stdio.h>
#include <string.h>

static const unsigned char bookmark_magic[8] = {
    'D', 'C', 'B', 'R', 'W', 'B', 'M', '1'
};
#define BOOKMARKS_VERSION 1

static uint32_t crc32(const unsigned char *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    size_t i;
    int bit;
    for(i = 0; i < size; ++i) {
        crc ^= data[i];
        for(bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put16(unsigned char *out, unsigned value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *out, uint32_t value) {
    put16(out, value & 0xffff);
    put16(out + 2, value >> 16);
}

static unsigned get16(const unsigned char *in) {
    return (unsigned)in[0] | ((unsigned)in[1] << 8);
}

static uint32_t get32(const unsigned char *in) {
    return get16(in) | ((uint32_t)get16(in + 2) << 16);
}

static int web_url(const char *url) {
    return !strncmp(url, "https://", 8) || !strncmp(url, "http://", 7);
}

/* Tabs and newlines delimit the record, so they never enter a field. */
static void copy_field(char *out, size_t size, const char *text, size_t length) {
    size_t i;
    if(length >= size) length = size - 1;
    for(i = 0; i < length; ++i)
        out[i] = text[i] == '\t' || text[i] == '\n' || text[i] == '\r' ? ' ' : text[i];
    out[length] = 0;
}

int bookmarks_find(const bookmark_list_t *list, const char *url) {
    int i;
    for(i = 0; i < list->count; ++i)
        if(!strcmp(list->items[i].url, url)) return i;
    return -1;
}

/* Returns the bookmark's index (an existing one for a duplicate URL),
   -1 when the list is full, or -2 for a URL that cannot be bookmarked. */
int bookmarks_add(bookmark_list_t *list, const char *url, const char *title) {
    bookmark_t *item;
    int existing;
    if(!web_url(url) || strlen(url) >= MAX_URL) return -2;
    existing = bookmarks_find(list, url);
    if(existing >= 0) return existing;
    if(list->count >= MAX_BOOKMARKS) return -1;
    item = &list->items[list->count];
    copy_field(item->url, sizeof(item->url), url, strlen(url));
    copy_field(item->title, sizeof(item->title), title && title[0] ? title : url,
               strlen(title && title[0] ? title : url));
    return list->count++;
}

int bookmarks_remove(bookmark_list_t *list, int index) {
    if(index < 0 || index >= list->count) return -1;
    memmove(&list->items[index], &list->items[index + 1],
            sizeof(list->items[0]) * (size_t)(list->count - index - 1));
    list->count--;
    return 0;
}

/* Record: magic, version, count, text length, CRC-32 of the text, then one
   "url<TAB>title<LF>" line per bookmark. Returns 0 if out is too small. */
size_t bookmarks_serialize(const bookmark_list_t *list, unsigned char *out,
                           size_t out_size) {
    size_t n = BOOKMARKS_HEADER_BYTES;
    int i;
    if(out_size < BOOKMARKS_HEADER_BYTES) return 0;
    for(i = 0; i < list->count; ++i) {
        size_t written = (size_t)snprintf((char *)out + n, out_size - n, "%s\t%s\n",
                                          list->items[i].url, list->items[i].title);
        if(n + written >= out_size) return 0;
        n += written;
    }
    memcpy(out, bookmark_magic, sizeof(bookmark_magic));
    put16(out + 8, BOOKMARKS_VERSION);
    put16(out + 10, (unsigned)list->count);
    put32(out + 12, (uint32_t)(n - BOOKMARKS_HEADER_BYTES));
    put32(out + 16, crc32(out + BOOKMARKS_HEADER_BYTES, n - BOOKMARKS_HEADER_BYTES));
    return n;
}

/* Replaces list only when the whole record is valid. */
int bookmarks_deserialize(bookmark_list_t *list, const unsigned char *data,
                          size_t size) {
    static bookmark_list_t parsed;
    const unsigned char *p;
    const unsigned char *end;
    unsigned count;
    uint32_t length;

    if(size < BOOKMARKS_HEADER_BYTES ||
       memcmp(data, bookmark_magic, sizeof(bookmark_magic)) ||
       get16(data + 8) != BOOKMARKS_VERSION)
        return -1;
    count = get16(data + 10);
    length = get32(data + 12);
    if(count > MAX_BOOKMARKS || length > size - BOOKMARKS_HEADER_BYTES ||
       crc32(data + BOOKMARKS_HEADER_BYTES, length) != get32(data + 16))
        return -1;

    memset(&parsed, 0, sizeof(parsed));
    p = data + BOOKMARKS_HEADER_BYTES;
    end = p + length;
    while(p < end) {
        const unsigned char *tab = memchr(p, '\t', (size_t)(end - p));
        const unsigned char *newline = memchr(p, '\n', (size_t)(end - p));
        bookmark_t *item;
        if(!tab || !newline || tab > newline || parsed.count >= MAX_BOOKMARKS ||
           (size_t)(tab - p) >= MAX_URL)
            return -1;
        item = &parsed.items[parsed.count++];
        copy_field(item->url, sizeof(item->url), (const char *)p, (size_t)(tab - p));
        copy_field(item->title, sizeof(item->title), (const char *)tab + 1,
                   (size_t)(newline - tab - 1));
        if(!web_url(item->url)) return -1;
        p = newline + 1;
    }
    if((unsigned)parsed.count != count) return -1;
    *list = parsed;
    return 0;
}

static void append(char *out, size_t out_size, size_t *n, const char *text) {
    size_t length = strlen(text);
    if(*n + length >= out_size) length = *n + 1 < out_size ? out_size - *n - 1 : 0;
    memcpy(out + *n, text, length);
    *n += length;
    out[*n] = 0;
}

static void append_escaped(char *out, size_t out_size, size_t *n, const char *text) {
    for(; *text; ++text) {
        char c[2] = { *text, 0 };
        append(out, out_size, n,
               *text == '&' ? "&amp;" : *text == '<' ? "&lt;" : *text == '>' ? "&gt;" :
               *text == '"' ? "&quot;" : *text == '\'' ? "&#39;" : c);
    }
}

/* Builds the internal bookmarks page. Text is ISO-8859-1, so the page must
   be parsed as windows-1252. Returns the length written. */
size_t bookmarks_page_html(const bookmark_list_t *list, const char *candidate_url,
                           const char *candidate_title, const char *home_url,
                           const char *storage_note, char *out, size_t out_size) {
    size_t n = 0;
    int i;

    if(!out_size) return 0;
    out[0] = 0;
    append(out, out_size, &n, "<title>Bookmarks</title><h1>Bookmarks</h1>");
    if(candidate_url && web_url(candidate_url)) {
        const char *title = candidate_title && candidate_title[0] ?
                            candidate_title : candidate_url;
        append(out, out_size, &n, bookmarks_find(list, candidate_url) >= 0 ?
               "<p>Already bookmarked: " :
               "<p><a href=\"about:bookmark-add\">Bookmark this page</a> ");
        append_escaped(out, out_size, &n, title);
        append(out, out_size, &n, "</p>");
    }
    if(list->count) {
        append(out, out_size, &n, "<ul>");
        for(i = 0; i < list->count; ++i) {
            char remove[96];
            append(out, out_size, &n, "<li><a href=\"");
            append_escaped(out, out_size, &n, list->items[i].url);
            append(out, out_size, &n, "\">");
            append_escaped(out, out_size, &n, list->items[i].title);
            snprintf(remove, sizeof(remove),
                     "</a> <a href=\"about:bookmark-remove?%d\">[remove]</a></li>", i);
            append(out, out_size, &n, remove);
        }
        append(out, out_size, &n, "</ul>");
    } else {
        append(out, out_size, &n,
               "<p>No bookmarks yet. On any page, press Start or Ctrl+B and "
               "choose Bookmark this page, or press Ctrl+D.</p>");
    }
    append(out, out_size, &n, "<p>");
    append_escaped(out, out_size, &n, storage_note ? storage_note : "");
    append(out, out_size, &n, "</p><h2>Browser</h2><p><a href=\"");
    append_escaped(out, out_size, &n, home_url);
    append(out, out_size, &n, "\">Home page</a></p>"
           "<p><a href=\"about:exit\">Exit browser</a></p>"
           "<h2>Controller</h2>"
           "<p>A open | B back | Y next link | X address bar | Start this menu | "
           "L/R triggers back/forward | D-pad scroll</p>"
           "<p>In a text box: D-pad picks a key, A types it, B deletes, "
           "X space, Y shift, L/R move the cursor, Start finishes.</p>"
           "<h2>Keyboard</h2>"
           "<p>Ctrl+D bookmark this page | Ctrl+B bookmarks | F1 all shortcuts</p>");
    return n;
}
