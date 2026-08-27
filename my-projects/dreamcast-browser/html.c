#include "browser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int line_y;
static int pending_space;

static void add_notice(browser_document_t *doc, const char *message);

static document_item_t *new_item(browser_document_t *doc) {
    document_item_t *item;
    if(doc->item_count >= MAX_ITEMS) {
        doc->truncated = 1;
        return NULL;
    }
    item = &doc->items[doc->item_count++];
    memset(item, 0, sizeof(*item));
    item->link_id = -1;
    item->image_id = -1;
    return item;
}

static void vertical_space(int amount) {
    if(amount > 0) line_y += amount;
    pending_space = 0;
}

static void add_line(browser_document_t *doc, const char *text,
                     text_style_t style, int link_id) {
    document_item_t *item;
    if(!text[0]) return;
    item = new_item(doc);
    if(!item) return;
    item->type = ITEM_TEXT;
    item->style = link_id >= 0 ? TEXT_LINK : style;
    item->x = PAGE_MARGIN;
    item->y = line_y;
    item->width = (int)strlen(text) * 12;
    item->height = 24;
    item->link_id = link_id;
    snprintf(item->text, sizeof(item->text), "%s", text);
    line_y += style == TEXT_HEADING ? 31 : 25;
}

static int utf8_to_latin1(const unsigned char *src, size_t remain,
                          unsigned char *out, size_t *used) {
    unsigned int cp;
    if(src[0] < 0x80) {
        *out = src[0];
        *used = 1;
        return 0;
    }
    if(remain >= 2 && (src[0] & 0xe0) == 0xc0 && (src[1] & 0xc0) == 0x80) {
        cp = ((src[0] & 0x1f) << 6) | (src[1] & 0x3f);
        *out = cp <= 255 ? (unsigned char)cp : '?';
        *used = 2;
        return 0;
    }
    if(remain >= 3 && (src[0] & 0xf0) == 0xe0) {
        *out = '?';
        *used = 3;
        return 0;
    }
    if(remain >= 4 && (src[0] & 0xf8) == 0xf0) {
        *out = '?';
        *used = 4;
        return 0;
    }
    *out = '?';
    *used = 1;
    return -1;
}

static int decode_entity(const char *src, size_t len, unsigned char *value) {
    char entity[16];
    char *end;
    long number;
    if(len >= sizeof(entity)) return 0;
    memcpy(entity, src, len);
    entity[len] = 0;
    if(!strcmp(entity, "amp")) *value = '&';
    else if(!strcmp(entity, "lt")) *value = '<';
    else if(!strcmp(entity, "gt")) *value = '>';
    else if(!strcmp(entity, "quot")) *value = '"';
    else if(!strcmp(entity, "apos") || !strcmp(entity, "#39")) *value = '\'';
    else if(!strcmp(entity, "nbsp")) *value = ' ';
    else if(entity[0] == '#') {
        number = strtol(entity + (entity[1] == 'x' || entity[1] == 'X' ? 2 : 1),
                        &end, entity[1] == 'x' || entity[1] == 'X' ? 16 : 10);
        if(*end || number < 0 || number > 255) *value = '?';
        else *value = (unsigned char)number;
    } else return 0;
    return 1;
}

static void normalize_text(const char *src, size_t len, char *out,
                           size_t out_size, int preformatted) {
    size_t i = 0, n = 0;
    int was_space = pending_space;
    while(i < len && n + 1 < out_size) {
        unsigned char c = (unsigned char)src[i];
        size_t used = 1;
        if(c == '&') {
            const char *semi = memchr(src + i, ';', len - i);
            if(semi && semi - (src + i) < 16 &&
               decode_entity(src + i + 1, (size_t)(semi - src - i - 1), &c))
                used = (size_t)(semi - src - i + 1);
        } else if(c >= 0x80) {
            utf8_to_latin1((const unsigned char *)src + i, len - i, &c, &used);
        }

        if(!preformatted && isspace(c)) {
            if(!was_space && n) out[n++] = ' ';
            was_space = 1;
        } else {
            if(c == '\r') c = '\n';
            out[n++] = (char)c;
            was_space = 0;
        }
        i += used;
    }
    while(n && out[n - 1] == ' ' && !preformatted) n--;
    out[n] = 0;
    pending_space = was_space;
}

static void wrap_text(browser_document_t *doc, const char *text,
                      text_style_t style, int link_id, int preformatted) {
    char line[48];
    int n = 0;
    const char *p = text;
    while(*p && !doc->truncated) {
        if(*p == '\n') {
            line[n] = 0;
            add_line(doc, line, style, link_id);
            n = 0;
            p++;
            continue;
        }
        if(n >= 45 || (n > 35 && *p == ' ')) {
            line[n] = 0;
            add_line(doc, line, style, link_id);
            n = 0;
            while(*p == ' ') p++;
            continue;
        }
        line[n++] = *p++;
    }
    if(n) {
        line[n] = 0;
        add_line(doc, line, style, link_id);
    }
    if(!preformatted && text[0]) pending_space = 1;
}

static int attr_value(const char *tag, const char *name, char *out, size_t cap) {
    const char *p = tag;
    size_t name_len = strlen(name);
    while(*p) {
        const char *start;
        char quote = 0;
        size_t len;
        while(*p && isspace((unsigned char)*p)) p++;
        if(!*p) break;
        if(!strncasecmp(p, name, name_len) &&
           (isspace((unsigned char)p[name_len]) || p[name_len] == '=')) {
            p += name_len;
            while(*p && isspace((unsigned char)*p)) p++;
            if(*p != '=') continue;
            p++;
            while(*p && isspace((unsigned char)*p)) p++;
            if(*p == '\'' || *p == '"') quote = *p++;
            start = p;
            if(quote) while(*p && *p != quote) p++;
            else while(*p && !isspace((unsigned char)*p) && *p != '>') p++;
            len = (size_t)(p - start);
            if(len >= cap) len = cap - 1;
            memcpy(out, start, len);
            out[len] = 0;
            return 1;
        }
        while(*p && !isspace((unsigned char)*p)) p++;
    }
    return 0;
}

static int add_link(browser_document_t *doc, const char *href) {
    char decoded[MAX_URL];
    char resolved[MAX_URL];
    normalize_text(href, strlen(href), decoded, sizeof(decoded), 1);
    if(doc->link_count >= MAX_LINKS ||
       resolve_url(doc->base_url, decoded, resolved, sizeof(resolved)) < 0)
        return -1;
    snprintf(doc->links[doc->link_count], MAX_URL, "%s", resolved);
    return doc->link_count++;
}

static void add_image(browser_document_t *doc, const char *src, const char *alt) {
    document_item_t *item;
    browser_image_t *image;
    char decoded[MAX_URL];
    char resolved[MAX_URL];

    if(doc->image_count >= MAX_IMAGES) {
        add_notice(doc, "[Additional images omitted: page image limit reached]");
        return;
    }
    normalize_text(src, strlen(src), decoded, sizeof(decoded), 1);
    if(resolve_url(doc->base_url, decoded, resolved, sizeof(resolved)) < 0) return;
    vertical_space(5);
    image = &doc->images[doc->image_count];
    memset(image, 0, sizeof(*image));
    snprintf(image->url, sizeof(image->url), "%s", resolved);
    snprintf(image->alt, sizeof(image->alt), "%s", alt[0] ? alt : "image");
    item = new_item(doc);
    if(!item) return;
    item->type = ITEM_IMAGE;
    item->x = PAGE_MARGIN;
    item->y = line_y;
    item->width = PAGE_WIDTH;
    item->height = 72;
    item->image_id = doc->image_count++;
    line_y += item->height + 7;
}

static void add_notice(browser_document_t *doc, const char *message) {
    document_item_t *item = new_item(doc);
    if(!item) return;
    item->type = ITEM_NOTICE;
    item->style = TEXT_MUTED;
    item->x = PAGE_MARGIN;
    item->y = line_y;
    item->height = 24;
    snprintf(item->text, sizeof(item->text), "%s", message);
    line_y += 27;
}

void document_init(browser_document_t *doc, const char *base_url) {
    memset(doc, 0, sizeof(*doc));
    snprintf(doc->base_url, sizeof(doc->base_url), "%s", base_url ? base_url : "");
    snprintf(doc->title, sizeof(doc->title), "Untitled page");
}

void document_free(browser_document_t *doc) {
    int i;
    for(i = 0; i < doc->image_count; ++i) {
        free(doc->images[i].pixels);
        doc->images[i].pixels = NULL;
    }
}

void document_make_error(browser_document_t *doc, const char *title, const char *message) {
    document_init(doc, "");
    snprintf(doc->title, sizeof(doc->title), "%s", title);
    line_y = 12;
    add_line(doc, title, TEXT_HEADING, -1);
    vertical_space(8);
    wrap_text(doc, message, TEXT_NORMAL, -1, 0);
    doc->height = line_y + 20;
}

void document_parse_html(browser_document_t *doc, const char *html, size_t size) {
    const char *p = html;
    const char *end = html + size;
    int skip_depth = 0;
    int in_head = 0;
    int in_title = 0;
    int in_pre = 0;
    int current_link = -1;
    text_style_t style = TEXT_NORMAL;

    line_y = 12;
    pending_space = 0;
    while(p < end && !doc->truncated) {
        if(*p != '<') {
            const char *next = memchr(p, '<', (size_t)(end - p));
            char normalized[1024];
            size_t count = next ? (size_t)(next - p) : (size_t)(end - p);
            if(!skip_depth && (!in_head || in_title)) {
                normalize_text(p, count, normalized, sizeof(normalized), in_pre);
                if(in_title && normalized[0])
                    snprintf(doc->title, sizeof(doc->title), "%.95s", normalized);
                else if(!in_head && normalized[0])
                    wrap_text(doc, normalized, style, current_link, in_pre);
            }
            p += count;
            continue;
        }

        {
            const char *close = memchr(p, '>', (size_t)(end - p));
            char tag[768];
            char name[32];
            char attr[MAX_URL];
            char alt[64] = {0};
            const char *q;
            size_t len;
            int closing = 0;
            int ni = 0;
            if(!close) break;
            len = (size_t)(close - p - 1);
            if(len >= sizeof(tag)) len = sizeof(tag) - 1;
            memcpy(tag, p + 1, len);
            tag[len] = 0;
            q = tag;
            while(*q && isspace((unsigned char)*q)) q++;
            if(*q == '!') { p = close + 1; continue; }
            if(*q == '/') { closing = 1; q++; }
            while(*q && isspace((unsigned char)*q)) q++;
            while(*q && !isspace((unsigned char)*q) && *q != '/' && ni < 31)
                name[ni++] = (char)tolower((unsigned char)*q++);
            name[ni] = 0;

            if(!strcmp(name, "script") || !strcmp(name, "style") ||
               !strcmp(name, "svg") || !strcmp(name, "canvas") ||
               !strcmp(name, "noscript")) {
                if(closing && skip_depth) skip_depth--;
                else if(!closing) skip_depth++;
                p = close + 1;
                continue;
            }
            if(skip_depth) { p = close + 1; continue; }

            if(!strcmp(name, "head")) in_head = !closing;
            else if(!strcmp(name, "title")) in_title = !closing;
            else if(!strcmp(name, "pre")) {
                vertical_space(closing ? 5 : 8);
                in_pre = !closing;
                style = closing ? TEXT_NORMAL : TEXT_CODE;
            } else if(!strcmp(name, "a")) {
                if(closing) current_link = -1;
                else if(attr_value(q, "href", attr, sizeof(attr))) current_link = add_link(doc, attr);
            } else if(!strcmp(name, "img") && !closing && !in_head) {
                if(attr_value(q, "src", attr, sizeof(attr))) {
                    attr_value(q, "alt", alt, sizeof(alt));
                    add_image(doc, attr, alt);
                }
            } else if(!strcmp(name, "h1") || !strcmp(name, "h2") || !strcmp(name, "h3")) {
                vertical_space(closing ? 7 : 10);
                style = closing ? TEXT_NORMAL : TEXT_HEADING;
            } else if(!strcmp(name, "p") || !strcmp(name, "div") ||
                      !strcmp(name, "section") || !strcmp(name, "article") ||
                      !strcmp(name, "header") || !strcmp(name, "footer")) {
                vertical_space(closing ? 7 : 4);
            } else if(!strcmp(name, "br")) {
                vertical_space(25);
            } else if(!strcmp(name, "li") && !closing) {
                vertical_space(3);
                wrap_text(doc, "* ", style, current_link, 0);
            } else if(!strcmp(name, "hr")) {
                document_item_t *item = new_item(doc);
                vertical_space(5);
                if(item) {
                    item->type = ITEM_RULE;
                    item->x = PAGE_MARGIN;
                    item->y = line_y;
                    item->width = PAGE_WIDTH;
                    item->height = 1;
                    line_y += 10;
                }
            } else if(strcmp(name, "html") && strcmp(name, "body") &&
                      strcmp(name, "main") && strcmp(name, "span") &&
                      strcmp(name, "b") && strcmp(name, "strong") &&
                      strcmp(name, "i") && strcmp(name, "em") &&
                      strcmp(name, "ul") && strcmp(name, "ol") &&
                      strcmp(name, "meta") && strcmp(name, "link") &&
                      strcmp(name, "base") && strcmp(name, "input") &&
                      strcmp(name, "form") && strcmp(name, "label") &&
                      strcmp(name, "button")) {
                doc->unsupported_count++;
            }
            p = close + 1;
        }
    }

    if(doc->truncated)
        add_notice(doc, "[Page shortened: document layout limit reached]");
    if(doc->unsupported_count)
        printf("browser: ignored %d unsupported HTML elements\n", doc->unsupported_count);
    doc->height = line_y + 20;
}

void document_reflow(browser_document_t *doc) {
    int i;
    int y = 12;
    for(i = 0; i < doc->item_count; ++i) {
        document_item_t *item = &doc->items[i];
        item->y = y;
        if(item->type == ITEM_IMAGE && item->image_id >= 0) {
            browser_image_t *image = &doc->images[item->image_id];
            item->width = image->loaded > 0 ? image->width : PAGE_WIDTH;
            item->height = image->loaded > 0 ? image->height : 72;
            y += item->height + 7;
        } else if(item->type == ITEM_RULE) y += 10;
        else y += item->style == TEXT_HEADING ? 31 : 25;
    }
    doc->height = y + 20;
}
