#include "browser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define READER_IMAGE_PLACEHOLDER_HEIGHT 32

typedef enum {
    ENCODING_UTF8,
    ENCODING_WINDOWS_1252
} text_encoding_t;

typedef struct {
    int skip_depth;
    int hidden;
    int in_head;
    int in_title;
    int title_started;
    int title_space;
    int in_pre;
    int in_heading;
    int in_strong;
    int in_emphasis;
    int in_code;
    int current_link;
    int adjacent_link;
    int list_marker_pending;
    int current_form;
    int current_button;
    int current_select;
    int current_option;
    int option_has_value;
    int label_space;
    int base_seen;
} parse_state_t;

static int line_y;
static int line_x;
static int line_height;
static int line_has_content;
static int line_indent;
static int pending_space;
static int pending_gap;
static int merge_barrier;
static uint32_t anchor_targets[MAX_ANCHORS];
static int anchor_target_count;
static text_encoding_t encoding = ENCODING_UTF8;

static void add_notice(browser_document_t *doc, const char *message);

/* ---- Character decoding ------------------------------------------------ */

/* WHATWG maps ISO-8859-1 labels to windows-1252, whose 0x80-0x9f range holds
   typographic punctuation; numeric references in that range use it too. */
static const unsigned short windows_1252_high[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178
};

/* Nearest ASCII letter for U+0100..U+017F (Latin Extended-A). */
static const char latin_extended_a[] =
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
_Static_assert(sizeof(latin_extended_a) == 129, "one letter per code point");

static const char *const latin1_entities[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect",
    "uml", "copy", "ordf", "laquo", "not", "shy", "reg", "macr",
    "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot",
    "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
    "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil",
    "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml",
    "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times",
    "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig",
    "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil",
    "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml",
    "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide",
    "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml"
};

static const struct {
    const char *name;
    unsigned short codepoint;
} named_entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
    { "apos", '\'' }, { "Tab", '\t' }, { "NewLine", '\n' },
    { "OElig", 0x152 }, { "oelig", 0x153 }, { "Scaron", 0x160 },
    { "scaron", 0x161 }, { "Yuml", 0x178 }, { "fnof", 0x192 },
    { "circ", 0x2c6 }, { "tilde", 0x2dc }, { "ensp", 0x2002 },
    { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwnj", 0x200c },
    { "zwj", 0x200d }, { "lrm", 0x200e }, { "rlm", 0x200f },
    { "hyphen", 0x2010 }, { "dash", 0x2010 }, { "ndash", 0x2013 },
    { "mdash", 0x2014 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "sbquo", 0x201a }, { "ldquo", 0x201c }, { "rdquo", 0x201d },
    { "bdquo", 0x201e }, { "dagger", 0x2020 }, { "Dagger", 0x2021 },
    { "bull", 0x2022 }, { "hellip", 0x2026 }, { "permil", 0x2030 },
    { "prime", 0x2032 }, { "Prime", 0x2033 }, { "lsaquo", 0x2039 },
    { "rsaquo", 0x203a }, { "frasl", 0x2044 }, { "euro", 0x20ac },
    { "trade", 0x2122 }, { "larr", 0x2190 }, { "uarr", 0x2191 },
    { "rarr", 0x2192 }, { "darr", 0x2193 }, { "harr", 0x2194 },
    { "minus", 0x2212 }, { "asymp", 0x2248 }, { "ne", 0x2260 },
    { "le", 0x2264 }, { "ge", 0x2265 }
};

static int is_continuation(unsigned char c) {
    return (c & 0xc0) == 0x80;
}

/* Returns one UTF-8 sequence's code point, or -1 when it is malformed. */
static long decode_utf8(const unsigned char *s, size_t remain, size_t *used) {
    long cp;
    *used = 1;
    if(s[0] < 0x80) return s[0];
    if(s[0] >= 0xc2 && s[0] <= 0xdf && remain >= 2 && is_continuation(s[1])) {
        *used = 2;
        return ((long)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
    }
    if(s[0] >= 0xe0 && s[0] <= 0xef && remain >= 3 &&
       is_continuation(s[1]) && is_continuation(s[2])) {
        cp = ((long)(s[0] & 0x0f) << 12) | ((long)(s[1] & 0x3f) << 6) |
             (s[2] & 0x3f);
        if(cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff)) return -1;
        *used = 3;
        return cp;
    }
    if(s[0] >= 0xf0 && s[0] <= 0xf4 && remain >= 4 && is_continuation(s[1]) &&
       is_continuation(s[2]) && is_continuation(s[3])) {
        cp = ((long)(s[0] & 0x07) << 18) | ((long)(s[1] & 0x3f) << 12) |
             ((long)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        if(cp < 0x10000 || cp > 0x10ffff) return -1;
        *used = 4;
        return cp;
    }
    return -1;
}

static int utf8_valid(const char *text, size_t size) {
    const unsigned char *s = (const unsigned char *)text;
    size_t i = 0;
    while(i < size) {
        size_t used;
        if(decode_utf8(s + i, size - i, &used) < 0) {
            /* A response cut at the size limit may end mid-sequence. */
            return size - i < 4 && s[i] >= 0xc2 && s[i] <= 0xf4;
        }
        i += used;
    }
    return 1;
}

static unsigned long next_codepoint(const char *src, size_t remain, size_t *used) {
    const unsigned char *s = (const unsigned char *)src;
    long cp;
    if(encoding == ENCODING_WINDOWS_1252) {
        *used = 1;
        return s[0] >= 0x80 && s[0] < 0xa0 ? windows_1252_high[s[0] - 0x80] : s[0];
    }
    cp = decode_utf8(s, remain, used);
    return cp < 0 ? 0xfffd : (unsigned long)cp;
}

/* Decodes a character reference following '&'. Returns the bytes consumed
   after the '&', or 0 when the text is not a reference. */
static size_t decode_entity(const char *src, size_t remain, int in_attribute,
                            unsigned long *codepoint) {
    char name[33];
    size_t n = 0;
    size_t i;
    int terminated;

    while(n < remain && n < 32 &&
          (isalnum((unsigned char)src[n]) || (!n && src[n] == '#'))) {
        name[n] = src[n];
        n++;
    }
    if(!n) return 0;
    name[n] = 0;
    terminated = n < remain && src[n] == ';';

    if(name[0] == '#') {
        int hex = name[1] == 'x' || name[1] == 'X';
        const char *digits = name + (hex ? 2 : 1);
        char *end;
        unsigned long value;
        if(!*digits || (!terminated && in_attribute)) return 0;
        value = strtoul(digits, &end, hex ? 16 : 10);
        if(*end) return 0;
        if(value >= 0x80 && value < 0xa0) value = windows_1252_high[value - 0x80];
        if(!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            value = 0xfffd;
        *codepoint = value;
        return n + (size_t)terminated;
    }
    /* Legacy pages omit ';' after the commonest names; URLs never may. */
    if(!terminated && (in_attribute ||
                       (strcmp(name, "amp") && strcmp(name, "lt") &&
                        strcmp(name, "gt") && strcmp(name, "quot") &&
                        strcmp(name, "nbsp"))))
        return 0;
    for(i = 0; i < 96; ++i) {
        if(!strcmp(name, latin1_entities[i])) {
            *codepoint = 0xa0 + i;
            return n + (size_t)terminated;
        }
    }
    for(i = 0; i < sizeof(named_entities) / sizeof(named_entities[0]); ++i) {
        if(!strcmp(name, named_entities[i].name)) {
            *codepoint = named_entities[i].codepoint;
            return n + (size_t)terminated;
        }
    }
    return 0;
}

/* Folds a code point into the BIOS font's ISO-8859-1 repertoire, using
   ASCII approximations for common typography. Writes at most 3 bytes. */
static size_t fold_codepoint(unsigned long cp, char *out) {
    const char *text = NULL;
    size_t length;

    if(cp < 0x80) {
        out[0] = cp < 0x20 && cp != '\t' && cp != '\n' ? ' ' : (char)cp;
        return 1;
    }
    if(cp == 0xa0) { out[0] = ' '; return 1; }
    if(cp == 0xad) return 0;
    if(cp > 0xa0 && cp <= 0xff) { out[0] = (char)cp; return 1; }
    if(cp == 0x152) text = "OE";
    else if(cp == 0x153) text = "oe";
    else if(cp >= 0x100 && cp <= 0x17f) {
        out[0] = latin_extended_a[cp - 0x100];
        return 1;
    }
    switch(cp) {
    case 0x192: text = "f"; break;
    case 0x2c6: text = "^"; break;
    case 0x2dc: text = "~"; break;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212:
        text = "-"; break;
    case 0x2014: case 0x2015: text = "--"; break;
    case 0x2018: case 0x2019: case 0x201a: case 0x201b: case 0x2032:
        text = "'"; break;
    case 0x201c: case 0x201d: case 0x201e: case 0x201f: case 0x2033:
        text = "\""; break;
    case 0x2020: case 0x2021: text = "+"; break;
    case 0x2022: case 0x2023: case 0x2043: case 0x2219: case 0x25cf:
    case 0x25e6: text = "*"; break;
    case 0x2026: text = "..."; break;
    case 0x2030: text = "%"; break;
    case 0x2039: text = "<"; break;
    case 0x203a: text = ">"; break;
    case 0x2044: text = "/"; break;
    case 0x20ac: text = "EUR"; break;
    case 0x2122: text = "TM"; break;
    case 0x2190: text = "<-"; break;
    case 0x2191: text = "^"; break;
    case 0x2192: text = "->"; break;
    case 0x2193: text = "v"; break;
    case 0x2194: text = "<->"; break;
    case 0x2248: text = "~"; break;
    case 0x2260: text = "!="; break;
    case 0x2264: text = "<="; break;
    case 0x2265: text = ">="; break;
    default: break;
    }
    if(!text && ((cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
                 cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000))
        text = " ";
    /* Joiners, direction marks, BOMs, and emoji modifiers are invisible. */
    if(!text && ((cp >= 0x200b && cp <= 0x200f) ||
                 (cp >= 0x202a && cp <= 0x202e) || cp == 0x2060 ||
                 cp == 0xfeff || (cp >= 0xfe00 && cp <= 0xfe0f) ||
                 (cp >= 0x1f3fb && cp <= 0x1f3ff) ||
                 (cp >= 0xe0000 && cp <= 0xe01ef)))
        return 0;
    if(!text) text = "?";
    length = strlen(text);
    memcpy(out, text, length);
    return length;
}

/* Bounded copy between fields of one document. */
static void copy_text(char *out, size_t size, const char *text) {
    size_t length = strlen(text);
    if(length >= size) length = size - 1;
    memmove(out, text, length);
    out[length] = 0;
}

static size_t encode_utf8(unsigned long cp, unsigned char *out) {
    if(cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
    if(cp < 0x800) {
        out[0] = (unsigned char)(0xc0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if(cp < 0x10000) {
        out[0] = (unsigned char)(0xe0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (unsigned char)(0xf0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
}

/* Converts document text for display, collapsing whitespace unless
   preformatted. Returns the input bytes consumed, which is less than len
   when the output buffer fills. */
static size_t normalize_text_ex(const char *src, size_t len, char *out,
                                size_t out_size, int preformatted,
                                int entities) {
    size_t i = 0, n = 0;
    int was_space = preformatted ? 0 : pending_space;

    while(i < len) {
        unsigned long cp;
        size_t used = 0;
        size_t count;
        size_t k;
        char folded[4];

        if(entities && src[i] == '&' &&
           (used = decode_entity(src + i + 1, len - i - 1, 0, &cp)))
            used++;
        else
            cp = next_codepoint(src + i, len - i, &used);
        if(cp == '\r') {
            if(i + used < len && src[i + used] == '\n') {
                i += used;
                continue;
            }
            cp = '\n';
        }
        count = fold_codepoint(cp, folded);
        /* Room for the folded bytes, one collapsed space, and the NUL. */
        if(n + count + 2 > out_size) break;
        for(k = 0; k < count; ++k) {
            char c = folded[k];
            if(!preformatted && (c == ' ' || c == '\t' || c == '\n')) {
                was_space = 1;
            } else {
                if(!preformatted && was_space) out[n++] = ' ';
                out[n++] = c;
                was_space = 0;
            }
        }
        i += used;
    }
    out[n] = 0;
    if(!preformatted) pending_space = was_space;
    return i;
}

static void normalize_text(const char *src, size_t len, char *out,
                           size_t out_size, int preformatted) {
    normalize_text_ex(src, len, out, out_size, preformatted, 1);
}

/* Decodes references in a URL attribute and percent-encodes everything
   outside printable ASCII as UTF-8, so non-ASCII paths stay valid. */
static void normalize_url(const char *src, size_t len, char *out, size_t out_size) {
    size_t i = 0, n = 0;

    while(len && (unsigned char)*src <= ' ') { src++; len--; }
    while(len && (unsigned char)src[len - 1] <= ' ') len--;
    while(i < len) {
        unsigned long cp;
        size_t used = 0;
        char encoded[16];
        size_t count = 0;

        if(src[i] == '&' && (used = decode_entity(src + i + 1, len - i - 1, 1, &cp)))
            used++;
        else
            cp = next_codepoint(src + i, len - i, &used);
        i += used;
        if(cp == '\t' || cp == '\n' || cp == '\r') continue;
        if(cp > ' ' && cp < 0x7f && !strchr("\"<>`", (int)cp)) {
            encoded[count++] = (char)cp;
        } else {
            unsigned char bytes[4];
            size_t b, total = encode_utf8(cp, bytes);
            for(b = 0; b < total; ++b)
                count += (size_t)snprintf(encoded + count, sizeof(encoded) - count,
                                          "%%%02X", bytes[b]);
        }
        if(n + count + 1 > out_size) break;
        memcpy(out + n, encoded, count);
        n += count;
    }
    out[n] = 0;
}

static const char *find_ci(const char *haystack, size_t length, const char *needle) {
    size_t needle_length = strlen(needle);
    size_t i;
    for(i = 0; i + needle_length <= length; ++i)
        if(!strncasecmp(haystack + i, needle, needle_length)) return haystack + i;
    return NULL;
}

static int charset_label(const char *text, size_t length, text_encoding_t *found) {
    static const char *const latin[] = {
        "iso-8859-1", "iso8859-1", "iso_8859-1", "latin1", "l1", "iso-8859-15",
        "windows-1252", "cp1252", "x-cp1252", "us-ascii", "ascii"
    };
    char label[32];
    size_t n = 0;
    size_t i;

    while(length && (*text == ' ' || *text == '"' || *text == '\'')) {
        text++;
        length--;
    }
    while(n < length && n + 1 < sizeof(label) &&
          (isalnum((unsigned char)text[n]) || text[n] == '-' || text[n] == '_')) {
        label[n] = (char)tolower((unsigned char)text[n]);
        n++;
    }
    label[n] = 0;
    if(!strcmp(label, "utf-8") || !strcmp(label, "utf8")) {
        *found = ENCODING_UTF8;
        return 1;
    }
    for(i = 0; i < sizeof(latin) / sizeof(latin[0]); ++i) {
        if(!strcmp(label, latin[i])) {
            *found = ENCODING_WINDOWS_1252;
            return 1;
        }
    }
    return 0;
}

/* Byte-order mark, then the HTTP header, then an early <meta>, then a
   validity sniff: legacy pages that are not valid UTF-8 are windows-1252. */
static text_encoding_t detect_encoding(const char *html, size_t size,
                                       const char *content_type) {
    const char *label;
    size_t prefix = size < 2048 ? size : 2048;
    text_encoding_t found;

    if(size >= 3 && !memcmp(html, "\xef\xbb\xbf", 3)) return ENCODING_UTF8;
    if(content_type &&
       (label = find_ci(content_type, strlen(content_type), "charset=")) &&
       charset_label(label + 8, strlen(label + 8), &found))
        return found;
    if((label = find_ci(html, prefix, "charset=")) &&
       charset_label(label + 8, (size_t)(html + prefix - label - 8), &found))
        return found;
    return utf8_valid(html, size) ? ENCODING_UTF8 : ENCODING_WINDOWS_1252;
}

/* ---- Attributes -------------------------------------------------------- */

/* Finds an attribute in the text after an element name. Quoted values are
   skipped whole, so attribute-like text inside a value can never match. */
static int find_attribute(const char *p, const char *name, const char **value,
                          size_t *value_length) {
    size_t name_length = strlen(name);
    while(*p) {
        const char *start;
        const char *found_value = "";
        size_t found_length = 0;
        size_t length;

        while(*p && (isspace((unsigned char)*p) || *p == '/')) p++;
        if(!*p) break;
        start = p;
        while(*p && !isspace((unsigned char)*p) && *p != '=' && *p != '/') p++;
        length = (size_t)(p - start);
        while(*p && isspace((unsigned char)*p)) p++;
        if(*p == '=') {
            p++;
            while(*p && isspace((unsigned char)*p)) p++;
            if(*p == '"' || *p == '\'') {
                char quote = *p++;
                found_value = p;
                while(*p && *p != quote) p++;
                found_length = (size_t)(p - found_value);
                if(*p) p++;
            } else {
                found_value = p;
                while(*p && !isspace((unsigned char)*p)) p++;
                found_length = (size_t)(p - found_value);
            }
        } else if(!length) {
            p++;
        }
        if(length == name_length && !strncasecmp(start, name, length)) {
            *value = found_value;
            *value_length = found_length;
            return 1;
        }
    }
    return 0;
}

static int attr_present(const char *tag, const char *name) {
    const char *value;
    size_t length;
    return find_attribute(tag, name, &value, &length);
}

static int attr_value(const char *tag, const char *name, char *out, size_t cap) {
    const char *value;
    size_t length;
    if(!find_attribute(tag, name, &value, &length)) return 0;
    if(length >= cap) length = cap - 1;
    memcpy(out, value, length);
    out[length] = 0;
    return 1;
}

static int attr_lower(const char *tag, const char *name, char *out, size_t cap) {
    int found = attr_value(tag, name, out, cap);
    if(!found) return 0;
    for(; *out; ++out) *out = (char)tolower((unsigned char)*out);
    return found;
}

/* Attribute text for display: references decoded and whitespace folded. */
static int attr_text(const char *tag, const char *name, char *out, size_t cap) {
    const char *value;
    size_t length;
    int saved_space = pending_space;
    if(!find_attribute(tag, name, &value, &length)) return 0;
    pending_space = 0;
    normalize_text(value, length, out, cap, 0);
    pending_space = saved_space;
    return 1;
}

/* Attribute text submitted by forms: references decoded, spacing kept. */
static int attr_form_value(const char *tag, const char *name, char *out, size_t cap) {
    const char *value;
    size_t length;
    if(!find_attribute(tag, name, &value, &length)) return 0;
    normalize_text(value, length, out, cap, 1);
    return 1;
}

static int attr_url(const char *tag, const char *name, char *out, size_t cap) {
    const char *value;
    size_t length;
    if(!find_attribute(tag, name, &value, &length)) return 0;
    normalize_url(value, length, out, cap);
    return 1;
}

/* ---- Layout ------------------------------------------------------------ */

static document_item_t *new_item(browser_document_t *doc) {
    document_item_t *item;
    /* Keep the last eight slots for visible limit notices, including a later
       network-size notice. A full layout must still explain why it ends. */
    if(doc->item_count >= MAX_ITEMS - 8) {
        doc->truncated = 1;
        doc->limit_flags |= DOCUMENT_LIMIT_LAYOUT;
        return NULL;
    }
    item = &doc->items[doc->item_count++];
    memset(item, 0, sizeof(*item));
    item->link_id = -1;
    item->image_id = -1;
    return item;
}

static void reset_line(void) {
    line_x = PAGE_MARGIN + line_indent;
    line_height = 25;
    line_has_content = 0;
}

static void finish_line(int force) {
    if(line_has_content)
        line_y += line_height;
    else if(force)
        line_y += 25;
    reset_line();
    pending_space = 0;
}

static void vertical_space(int amount) {
    finish_line(0);
    /* Nested empty blocks share their largest margin instead of accumulating
       one blank strip per wrapper. Apply it only when content arrives. */
    if(amount > pending_gap) pending_gap = amount;
    pending_space = 0;
}

static void apply_gap(void) {
    line_y += pending_gap;
    pending_gap = 0;
}

static void add_text_run(browser_document_t *doc, const char *text, size_t len,
                         text_style_t style, int link_id) {
    document_item_t *item;
    text_style_t actual_style = link_id >= 0 ? TEXT_LINK : style;
    if(!len) return;
    if(len >= MAX_TEXT) len = MAX_TEXT - 1;
    /* Modern HTML often wraps single words in many spans. Adjacent pieces
       of the same styled run need one item, while anchor boundaries remain
       distinct so their item indices keep pointing at the right text. */
    item = doc->item_count ? &doc->items[doc->item_count - 1] : NULL;
    if(!merge_barrier && item && item->type == ITEM_TEXT &&
       item->style == actual_style && item->link_id == link_id &&
       item->y == line_y + pending_gap && item->x + item->width == line_x) {
        size_t previous = strlen(item->text);
        if(previous + len < sizeof(item->text)) {
            memcpy(item->text + previous, text, len);
            item->text[previous + len] = 0;
            item->width += (int)len * 12;
            line_x += (int)len * 12;
            return;
        }
    }
    item = new_item(doc);
    if(!item) return;
    merge_barrier = 0;
    apply_gap();
    item->type = ITEM_TEXT;
    item->style = actual_style;
    item->x = line_x;
    item->y = line_y;
    item->width = (int)len * 12;
    item->height = 24;
    item->link_id = link_id;
    memcpy(item->text, text, len);
    item->text[len] = 0;
    line_x += item->width;
    if(style == TEXT_HEADING && line_height < 31) line_height = 31;
    line_has_content = 1;
}

static void add_line(browser_document_t *doc, const char *text,
                     text_style_t style, int link_id) {
    finish_line(0);
    add_text_run(doc, text, strlen(text), style, link_id);
    finish_line(0);
}

static void wrap_preformatted(browser_document_t *doc, const char *text,
                              text_style_t style, int link_id) {
    const char *p = text;

    while(*p && !doc->truncated) {
        if(*p == '\n') {
            finish_line(1);
            p++;
            continue;
        }
        {
            int available = (PAGE_MARGIN + PAGE_WIDTH - line_x) / 12;
            const char *newline = strchr(p, '\n');
            size_t len = newline ? (size_t)(newline - p) : strlen(p);
            size_t take;
            if(available <= 0) {
                finish_line(0);
                continue;
            }
            take = len < (size_t)available ? len : (size_t)available;
            add_text_run(doc, p, take, style, link_id);
            p += take;
            if(take < len) finish_line(0);
        }
    }
}

static void wrap_text(browser_document_t *doc, const char *text,
                      text_style_t style, int link_id, int preformatted) {
    const char *p = text;

    if(preformatted) {
        wrap_preformatted(doc, text, style, link_id);
        return;
    }

    while(*p && !doc->truncated) {
        const char *newline;
        size_t len;
        size_t take;
        int available;

        if(*p == '\n') {
            finish_line(1);
            p++;
            continue;
        }
        if(!line_has_content) while(*p == ' ') p++;
        if(!*p) break;
        available = (PAGE_MARGIN + PAGE_WIDTH - line_x) / 12;
        if(available <= 0) {
            finish_line(0);
            continue;
        }
        newline = strchr(p, '\n');
        len = newline ? (size_t)(newline - p) : strlen(p);
        if(len <= (size_t)available) {
            add_text_run(doc, p, len, style, link_id);
            p += len;
            continue;
        }

        take = (size_t)available;
        while(take && p[take] != ' ') take--;
        if(!take) {
            const char *space = memchr(p, ' ', len);
            size_t word = space ? (size_t)(space - p) : len;
            if(line_has_content && word <= (size_t)(PAGE_WIDTH / 12)) {
                finish_line(0);
                continue;
            }
            take = (size_t)available;
        }
        while(take && p[take - 1] == ' ') take--;
        add_text_run(doc, p, take, style, link_id);
        p += take;
        while(*p == ' ') p++;
        finish_line(0);
    }
}

static int add_link(browser_document_t *doc, const char *url) {
    char resolved[MAX_URL];
    if(resolve_url(doc->base_url, url, resolved, sizeof(resolved)) < 0)
        return -1;
    if(doc->link_count >= MAX_LINKS) {
        doc->limit_flags |= DOCUMENT_LIMIT_LINKS;
        return -1;
    }
    snprintf(doc->links[doc->link_count], MAX_URL, "%s", resolved);
    return doc->link_count++;
}

static void add_image(browser_document_t *doc, const char *url, const char *alt,
                      int declared_width, int declared_height) {
    document_item_t *item;
    browser_image_t *image;
    char resolved[MAX_URL];

    if(doc->image_count >= MAX_IMAGES) {
        doc->limit_flags |= DOCUMENT_LIMIT_IMAGES;
        return;
    }
    if(resolve_url(doc->base_url, url, resolved, sizeof(resolved)) < 0) return;
    /* Like desktop browsers, upgrade insecure images on secure pages rather
       than revealing the page's content over plain HTTP. */
    if(!strncmp(doc->base_url, "https://", 8) && !strncmp(resolved, "http://", 7)) {
        size_t length = strlen(resolved);
        if(length + 1 >= sizeof(resolved)) return; /* No room: never truncate a URL. */
        memmove(resolved + 5, resolved + 4, length - 3);
        resolved[4] = 's';
    }
    vertical_space(5);
    apply_gap();
    image = &doc->images[doc->image_count];
    memset(image, 0, sizeof(*image));
    if(declared_width > PAGE_WIDTH) {
        if(declared_height > 0)
            declared_height = declared_height * PAGE_WIDTH / declared_width;
        declared_width = PAGE_WIDTH;
    }
    if(declared_height > 240) {
        if(declared_width > 0)
            declared_width = declared_width * 240 / declared_height;
        declared_height = 240;
    }
    image->width = declared_width > 0 ? declared_width : PAGE_WIDTH;
    image->height = declared_height > 0 ? declared_height : 72;
    snprintf(image->url, sizeof(image->url), "%s", resolved);
    snprintf(image->alt, sizeof(image->alt), "%s", alt[0] ? alt : "image");
    item = new_item(doc);
    if(!item) return;
    item->type = ITEM_IMAGE;
    item->x = PAGE_MARGIN;
    item->y = line_y;
    /* Optional images should not push reader text off-screen before the
       user asks for them. Keep the declared dimensions on the image itself
       for full-page layout and replace this compact row only after decoding. */
    item->width = doc->reader_active ? PAGE_WIDTH : image->width;
    item->height = doc->reader_active ? READER_IMAGE_PLACEHOLDER_HEIGHT : image->height;
    item->image_id = doc->image_count++;
    line_y += item->height + 7;
}

void document_mark_shortened(browser_document_t *doc, const char *message) {
    doc->truncated = 1;
    if(doc->item_count >= MAX_ITEMS) return;
    line_y = doc->height > 20 ? doc->height - 20 : 12;
    line_indent = 0;
    reset_line();
    pending_space = 0;
    pending_gap = 0;
    add_notice(doc, message);
    doc->height = line_y + 20;
    document_touch(doc);
}

static void add_notice(browser_document_t *doc, const char *message) {
    const char *p = message;
    finish_line(0);
    apply_gap();
    while(*p && doc->item_count < MAX_ITEMS) {
        document_item_t *item = &doc->items[doc->item_count++];
        size_t count = strlen(p), take = count;
        if(take > PAGE_WIDTH / 12) {
            take = PAGE_WIDTH / 12;
            while(take && p[take] != ' ') take--;
            if(!take) take = PAGE_WIDTH / 12;
        }
        memset(item, 0, sizeof(*item));
        item->type = ITEM_NOTICE;
        item->style = TEXT_MUTED;
        item->link_id = item->image_id = -1;
        item->x = PAGE_MARGIN;
        item->y = line_y;
        item->height = 24;
        item->width = (int)take * 12;
        memcpy(item->text, p, take);
        item->text[take] = 0;
        line_y += 27;
        p += take;
        while(*p == ' ') p++;
    }
}

/* One serial for all documents and mutations. Initialization must not read
   the old object: callers may hand us fresh, uninitialized malloc memory.
   A shared serial also distinguishes replacement documents in the renderer. */
void document_touch(browser_document_t *doc) {
    static unsigned generation;
    if(++generation == 0) ++generation;
    doc->generation = generation;
}

void document_init(browser_document_t *doc, const char *base_url) {
    memset(doc, 0, sizeof(*doc));
    document_touch(doc);
    snprintf(doc->base_url, sizeof(doc->base_url), "%s", base_url ? base_url : "");
    snprintf(doc->title, sizeof(doc->title), "Untitled page");
}

void document_free(browser_document_t *doc) {
    int i;
    for(i = 0; i < doc->image_count; ++i) {
        free(doc->images[i].pixels);
        doc->images[i].pixels = NULL;
    }
    document_touch(doc);
}

void document_make_error(browser_document_t *doc, const char *title, const char *message) {
    document_init(doc, "");
    snprintf(doc->title, sizeof(doc->title), "%s", title);
    line_y = 12;
    line_indent = 0;
    reset_line();
    pending_space = 0;
    pending_gap = 0;
    add_line(doc, title, TEXT_HEADING, -1);
    vertical_space(8);
    wrap_text(doc, message, TEXT_NORMAL, -1, 0);
    finish_line(0);
    doc->height = line_y + 20;
    document_touch(doc);
}

static text_style_t active_style(const parse_state_t *st) {
    if(st->in_pre || st->in_code) return TEXT_CODE;
    if(st->in_heading) return TEXT_HEADING;
    if(st->in_strong) return TEXT_STRONG;
    if(st->in_emphasis) return TEXT_EMPHASIS;
    return TEXT_NORMAL;
}

/* ---- Form controls ----------------------------------------------------- */

int document_field_is_submit(const browser_field_t *field) {
    return !strcmp(field->type, "submit") || !strcmp(field->type, "image");
}

int document_field_is_text(const browser_field_t *field) {
    return strcmp(field->type, "checkbox") && strcmp(field->type, "radio") &&
           strcmp(field->type, "select") && strcmp(field->type, "hidden") &&
           !document_field_is_submit(field);
}

/* Copies a field value for display: passwords masked, newlines as a
   pilcrow so a textarea stays on one row. */
static void display_value(const browser_field_t *field, size_t start,
                          size_t count, char *out) {
    size_t i;
    int password = !strcmp(field->type, "password");
    for(i = 0; i < count; ++i) {
        char c = field->value[start + i];
        out[i] = password ? '*' : c == '\n' ? '\xb6' : c;
    }
    out[count] = 0;
}

void document_refresh_field(browser_document_t *doc, int index) {
    browser_field_t *field = &doc->fields[index];
    document_item_t *item;
    size_t length = strlen(field->value);

    if(field->item < 0 || field->item >= doc->item_count) return;
    item = &doc->items[field->item];
    if(!strcmp(field->type, "checkbox")) {
        snprintf(item->text, sizeof(item->text), "[%c]", field->checked ? 'X' : ' ');
    } else if(!strcmp(field->type, "radio")) {
        snprintf(item->text, sizeof(item->text), "(%c)", field->checked ? '*' : ' ');
    } else if(document_field_is_submit(field)) {
        snprintf(item->text, sizeof(item->text), "[ %.38s ]",
                 field->label[0] ? field->label :
                 field->value[0] ? field->value : "Submit");
    } else if(!strcmp(field->type, "select")) {
        const char *label = field->selected >= 0 && field->selected < field->option_count ?
                            doc->options[field->option_first + field->selected].label : "";
        snprintf(item->text, sizeof(item->text),
                 field->caret >= 0 ? "[< %.34s >]" : "[ %.34s v]", label);
    } else if(field->caret >= 0) {
        /* Show a 34-column window that keeps the insertion point visible. */
        size_t caret = (size_t)field->caret > length ? length : (size_t)field->caret;
        size_t start = caret > 33 ? caret - 33 : 0;
        size_t shown = length - start > 34 ? 34 : length - start;
        char window[40];
        display_value(field, start, shown, window);
        memmove(window + (caret - start) + 1, window + (caret - start),
                shown - (caret - start) + 1);
        window[caret - start] = '|';
        snprintf(item->text, sizeof(item->text), "[ %s%s%s ]", start ? "..." : "",
                 window, start + shown < length ? "..." : "");
    } else {
        char window[40];
        display_value(field, 0, length > 36 ? 36 : length, window);
        snprintf(item->text, sizeof(item->text), "[ %s%s ]",
                 window[0] ? window : "type here", length > 36 ? "..." : "");
    }
    item->width = (int)strlen(item->text) * 12;
    document_touch(doc);
}

void document_toggle_field(browser_document_t *doc, int index) {
    browser_field_t *field = &doc->fields[index];
    int i;
    if(!strcmp(field->type, "checkbox")) {
        field->checked = !field->checked;
        document_refresh_field(doc, index);
    } else if(!strcmp(field->type, "radio")) {
        for(i = 0; i < doc->field_count; ++i) {
            browser_field_t *other = &doc->fields[i];
            if(i != index && other->form == field->form &&
               !strcmp(other->type, "radio") && !strcmp(other->name, field->name) &&
               other->checked) {
                other->checked = 0;
                document_refresh_field(doc, i);
            }
        }
        field->checked = 1;
        document_refresh_field(doc, index);
    }
}

void document_select_option(browser_document_t *doc, int index, int option) {
    browser_field_t *field = &doc->fields[index];
    if(strcmp(field->type, "select") || !field->option_count) return;
    if(option < 0) option = 0;
    if(option >= field->option_count) option = field->option_count - 1;
    field->selected = option;
    copy_text(field->value, sizeof(field->value),
              doc->options[field->option_first + option].value);
    document_refresh_field(doc, index);
}

static browser_field_t *new_field(browser_document_t *doc, int form,
                                  const char *type, const char *tag, int hidden) {
    browser_field_t *field;
    char maxlength[16];

    if(doc->field_count >= MAX_FIELDS) {
        doc->forms[form].valid = 0;
        doc->limit_flags |= DOCUMENT_LIMIT_FORMS;
        return NULL;
    }
    field = &doc->fields[doc->field_count];
    memset(field, 0, sizeof(*field));
    field->form = form;
    field->item = -1;
    field->link = -1;
    field->caret = -1;
    field->selected = -1;
    field->maxlength = MAX_FIELD_VALUE - 1;
    field->disabled = attr_present(tag, "disabled");
    field->hidden = hidden;
    snprintf(field->type, sizeof(field->type), "%s", type);
    attr_value(tag, "name", field->name, sizeof(field->name));
    if(attr_value(tag, "maxlength", maxlength, sizeof(maxlength))) {
        int value = atoi(maxlength);
        if(value >= 0 && value < field->maxlength) field->maxlength = value;
    }
    return field;
}

/* Lays out a focusable control. Checkboxes and radios sit inline so the
   label text that follows them flows on the same row. */
static void show_control(browser_document_t *doc, int index, int inline_control) {
    browser_field_t *field = &doc->fields[index];

    if(field->disabled || field->hidden) return;
    if(doc->link_count >= MAX_LINKS) {
        doc->forms[field->form].valid = 0;
        doc->limit_flags |= DOCUMENT_LIMIT_LINKS | DOCUMENT_LIMIT_FORMS;
        return;
    }
    field->link = doc->link_count++;
    snprintf(doc->links[field->link], MAX_URL, "form:%d", index);
    field->item = doc->item_count;
    if(inline_control) {
        if(PAGE_MARGIN + PAGE_WIDTH - line_x < 5 * 12) finish_line(0);
        if(line_has_content) line_x += 12;
        add_text_run(doc, "[ ]", 3, TEXT_LINK, field->link);
        pending_space = 1;
    } else {
        vertical_space(3);
        add_line(doc, "[ input ]", TEXT_LINK, field->link);
    }
    if(field->item >= doc->item_count) field->item = -1;
    document_refresh_field(doc, index);
}

/* Accumulates text inside a <button> or <option> without disturbing the
   page's own whitespace state. */
static void append_label(parse_state_t *st, char *label, size_t cap,
                         const char *text, size_t length) {
    size_t used = strlen(label);
    int saved_space = pending_space;
    pending_space = used ? st->label_space : 0;
    normalize_text(text, length, label + used, cap - used, 0);
    if(!used && label[0] == ' ') memmove(label, label + 1, strlen(label));
    st->label_space = pending_space;
    pending_space = saved_space;
}

static void finish_button(browser_document_t *doc, parse_state_t *st) {
    int index = st->current_button;
    st->current_button = -1;
    if(index < 0) return;
    show_control(doc, index, 0);
}

static void finish_option(browser_document_t *doc, parse_state_t *st) {
    browser_option_t *option;
    if(st->current_option < 0) return;
    option = &doc->options[st->current_option];
    if(!st->option_has_value)
        copy_text(option->value, sizeof(option->value), option->label);
    st->current_option = -1;
}

static void finish_select(browser_document_t *doc, parse_state_t *st) {
    int index = st->current_select;
    browser_field_t *field;
    finish_option(doc, st);
    st->current_select = -1;
    if(index < 0) return;
    field = &doc->fields[index];
    if(!field->option_count) return;
    show_control(doc, index, 0);
    document_select_option(doc, index,
                           field->selected >= 0 ? field->selected : 0);
}

static void start_option(browser_document_t *doc, parse_state_t *st, const char *tag) {
    browser_field_t *field = &doc->fields[st->current_select];
    browser_option_t *option;
    finish_option(doc, st);
    if(doc->option_count >= MAX_OPTIONS) {
        doc->limit_flags |= DOCUMENT_LIMIT_FORMS;
        doc->forms[field->form].valid = 0;
        return;
    }
    option = &doc->options[doc->option_count];
    memset(option, 0, sizeof(*option));
    st->option_has_value = attr_form_value(tag, "value", option->value,
                                           sizeof(option->value));
    if(attr_present(tag, "selected")) field->selected = field->option_count;
    st->current_option = doc->option_count++;
    st->label_space = 0;
    field->option_count++;
}

static void handle_input(browser_document_t *doc, parse_state_t *st, const char *tag) {
    browser_field_t *field;
    char type[16] = "text";
    int index;

    attr_lower(tag, "type", type, sizeof(type));
    if(!type[0]) snprintf(type, sizeof(type), "text");
    if(!strcmp(type, "reset") || !strcmp(type, "button")) return;
    if(!strcmp(type, "file")) {
        doc->forms[st->current_form].valid = 0;
        return;
    }
    /* Unknown types are text inputs, as in HTML itself. */
    if(strcmp(type, "hidden") && strcmp(type, "checkbox") && strcmp(type, "radio") &&
       strcmp(type, "submit") && strcmp(type, "image") && strcmp(type, "password") &&
       strcmp(type, "email") && strcmp(type, "search") && strcmp(type, "url") &&
       strcmp(type, "tel") && strcmp(type, "number") && strcmp(type, "date") &&
       strcmp(type, "time") && strcmp(type, "datetime-local") &&
       strcmp(type, "month") && strcmp(type, "week") && strcmp(type, "color") &&
       strcmp(type, "range"))
        snprintf(type, sizeof(type), "text");

    field = new_field(doc, st->current_form, type, tag, st->hidden);
    if(!field) return;
    attr_form_value(tag, "value", field->value, sizeof(field->value));
    if(!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
        field->checked = attr_present(tag, "checked");
        if(!field->value[0]) snprintf(field->value, sizeof(field->value), "on");
    } else if(!strcmp(type, "image")) {
        attr_text(tag, "alt", field->label, sizeof(field->label));
    }
    index = doc->field_count++;
    if(strcmp(type, "hidden"))
        show_control(doc, index, !strcmp(type, "checkbox") || !strcmp(type, "radio"));
}

/* Textarea content is raw text up to its closing tag. Returns the parse
   position after that tag. */
static const char *handle_textarea(browser_document_t *doc, parse_state_t *st,
                                   const char *tag, const char *content,
                                   const char *end) {
    const char *finish = find_ci(content, (size_t)(end - content), "</textarea");
    const char *resume;
    browser_field_t *field;
    size_t length;

    if(!finish) finish = end;
    resume = memchr(finish, '>', (size_t)(end - finish));
    resume = resume ? resume + 1 : end;
    if(st->current_form < 0) return content; /* Parse it as ordinary text. */

    field = new_field(doc, st->current_form, "textarea", tag, st->hidden);
    if(!field) return resume;
    normalize_text_ex(content, (size_t)(finish - content), field->value,
                      sizeof(field->value), 1, 1);
    if(field->value[0] == '\n')
        memmove(field->value, field->value + 1, strlen(field->value));
    length = strlen(field->value);
    if(length > (size_t)field->maxlength) field->value[field->maxlength] = 0;
    show_control(doc, doc->field_count++, 0);
    return resume;
}

/* ---- Parsing ----------------------------------------------------------- */

/* Chooses how much of a text node to normalize at once, preferring to cut at
   whitespace and never inside a UTF-8 sequence or a character reference. */
static size_t text_chunk(const char *p, size_t count) {
    size_t cut = 700;
    size_t amp;
    if(count <= cut) return count;
    while(cut > 100 && !isspace((unsigned char)p[cut])) cut--;
    if(cut <= 100) {
        cut = 700;
        while(cut > 1 && is_continuation((unsigned char)p[cut])) cut--;
    }
    for(amp = cut > 32 ? cut - 32 : 0; amp < cut; ++amp)
        if(p[amp] == '&' && !memchr(p + amp, ';', cut - amp)) return amp ? amp : cut;
    return cut;
}

static void render_text_node(browser_document_t *doc, parse_state_t *st,
                             const char *p, size_t count, int entities) {
    size_t done = 0;

    if(st->skip_depth || (st->in_head && !st->in_title)) return;
    if(st->hidden &&
       !(st->current_select >= 0 && doc->fields[st->current_select].hidden) &&
       !(st->current_button >= 0 && doc->fields[st->current_button].hidden)) return;
    if(st->adjacent_link && !st->in_pre && st->current_select < 0 &&
       st->current_button < 0) {
        size_t i = 0;
        while(i < count && isspace((unsigned char)p[i])) i++;
        if(i < count && (isalnum((unsigned char)p[i]) || (unsigned char)p[i] >= 0x80))
            pending_space = 1;
    }
    while(done < count && !doc->truncated) {
        char normalized[1024];
        size_t chunk = text_chunk(p + done, count - done);
        size_t used;

        if(st->in_title) {
            int saved_space = pending_space;
            size_t length = st->title_started ? strlen(doc->title) : 0;
            pending_space = length ? st->title_space : 0;
            used = normalize_text_ex(p + done, chunk, doc->title + length,
                                     sizeof(doc->title) - length, 0, entities);
            if(doc->title[0]) st->title_started = 1;
            else snprintf(doc->title, sizeof(doc->title), "Untitled page");
            st->title_space = pending_space;
            pending_space = saved_space;
        } else if(st->current_select >= 0) {
            used = chunk;
            if(st->current_option >= 0)
                append_label(st, doc->options[st->current_option].label,
                             sizeof(doc->options[0].label), p + done, chunk);
        } else if(st->current_button >= 0) {
            used = chunk;
            append_label(st, doc->fields[st->current_button].label,
                         sizeof(doc->fields[0].label), p + done, chunk);
        } else {
            used = normalize_text_ex(p + done, chunk, normalized, sizeof(normalized),
                                     st->in_pre, entities);
            if(!st->in_head && normalized[0]) {
                int trailing_space = pending_space;
                if(st->list_marker_pending) {
                    add_text_run(doc, "* ", 2, TEXT_NORMAL, -1);
                    st->list_marker_pending = 0;
                }
                wrap_text(doc, normalized, active_style(st), st->current_link,
                          st->in_pre);
                /* Line wrapping consumes layout whitespace; the original
                   text node's trailing space still separates the next inline
                   element even when this node wrapped onto another row. */
                pending_space = trailing_space;
                st->adjacent_link = 0;
            }
        }
        if(!used) break;
        done += used;
    }
}

static int is_block_tag(const char *name) {
    static const char *const blocks[] = {
        "p", "div", "section", "article", "header", "footer", "nav", "aside",
        "figure", "figcaption", "blockquote", "center", "dl", "dt", "dd",
        "address", "details", "summary", "fieldset", "legend", "main"
    };
    size_t i;
    for(i = 0; i < sizeof(blocks) / sizeof(blocks[0]); ++i)
        if(!strcmp(name, blocks[i])) return 1;
    return 0;
}

static int is_ignored_tag(const char *name) {
    static const char *const ignored[] = {
        "html", "body", "span", "ul", "ol", "table", "tbody", "thead", "tfoot",
        "colgroup", "col", "caption", "font", "small", "time", "abbr", "cite",
        "s", "del", "ins", "sup", "sub", "picture", "source", "meta", "link",
        "base", "label", "button", "u", "mark", "q", "kbd", "samp", "var",
        "big", "tt", "optgroup", "option", "select", "wbr", "nobr"
    };
    size_t i;
    for(i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i)
        if(!strcmp(name, ignored[i])) return 1;
    return 0;
}

/* A tiny bounded tokenizer shared with reader selection and hidden-subtree
   skipping. Quoted '>' must not terminate a tag. */
static const char *tag_end(const char *start, const char *end) {
    char quote = 0;
    for(; start < end; ++start) {
        if(quote) { if(*start == quote) quote = 0; }
        else if(*start == '\'' || *start == '"') quote = *start;
        else if(*start == '>') return start;
    }
    return NULL;
}

static int void_tag(const char *name) {
    return !strcmp(name, "area") || !strcmp(name, "base") ||
           !strcmp(name, "br") || !strcmp(name, "col") ||
           !strcmp(name, "embed") || !strcmp(name, "hr") ||
           !strcmp(name, "img") || !strcmp(name, "input") ||
           !strcmp(name, "link") || !strcmp(name, "meta") ||
           !strcmp(name, "param") || !strcmp(name, "source") ||
           !strcmp(name, "track") || !strcmp(name, "wbr");
}

static const char *tag_name(const char *p, const char *end, char *name,
                             size_t capacity, int *closing) {
    size_t n = 0;
    while(p < end && isspace((unsigned char)*p)) p++;
    *closing = p < end && *p == '/';
    if(*closing) p++;
    while(p < end && isspace((unsigned char)*p)) p++;
    while(p < end && !isspace((unsigned char)*p) && *p != '/' && *p != '>') {
        if(n + 1 < capacity) name[n++] = (char)tolower((unsigned char)*p);
        p++;
    }
    name[n] = 0;
    return p;
}

static int raw_tag(const char *name) {
    return !strcmp(name, "script") || !strcmp(name, "style") ||
           !strcmp(name, "textarea") || !strcmp(name, "title");
}

/* Returns just after the matching close; an incomplete response extends to
   end. Only equal tag names affect nesting, so void tags cannot trap us. */
static const char *subtree_end(const char *p, const char *end, const char *name) {
    unsigned depth = 1;
    int raw = raw_tag(name);
    while(p < end) {
        const char *start = raw ? find_ci(p, (size_t)(end - p), "</") :
                                 memchr(p, '<', (size_t)(end - p));
        const char *close;
        char child[32];
        int closing;
        if(!start) break;
        if(!raw && end - start >= 4 && !memcmp(start, "<!--", 4)) {
            const char *finish = find_ci(start + 4, (size_t)(end - start - 4), "-->");
            p = finish ? finish + 3 : end;
            continue;
        }
        close = tag_end(start + 1, end);
        if(!close) break;
        tag_name(start + 1, close, child, sizeof(child), &closing);
        p = close + 1;
        if(!strcmp(name, child)) {
            if(closing) { if(!--depth) return p; }
            else if(!raw) depth++;
        } else if(!raw && !closing && raw_tag(child)) {
            p = subtree_end(p, end, child);
        }
    }
    return end;
}

/* Honor a small, deterministic subset of visibility without pretending to
   implement a CSS cascade. 'display:none' must be its own declaration. */
static int hidden_element(const char *attributes) {
    char style[512];
    const char *p;
    if(attr_present(attributes, "hidden")) return 1;
    if(!attr_lower(attributes, "style", style, sizeof(style))) return 0;
    p = style;
    while(*p) {
        const char *end = strchr(p, ';');
        const char *colon;
        const char *key_end;
        if(!end) end = p + strlen(p);
        while(p < end && isspace((unsigned char)*p)) p++;
        colon = memchr(p, ':', (size_t)(end - p));
        key_end = colon;
        if(colon) {
            const char *value = colon + 1;
            while(key_end > p && isspace((unsigned char)key_end[-1])) key_end--;
            while(value < end && isspace((unsigned char)*value)) value++;
            if(key_end - p == 7 && !memcmp(p, "display", 7) &&
               end - value >= 4 && !memcmp(value, "none", 4) &&
               (value + 4 == end || isspace((unsigned char)value[4]) || value[4] == '!'))
                return 1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

static int attribute_token(const char *attributes, const char *attribute,
                            const char *token) {
    const char *value, *end;
    size_t length, token_length = strlen(token);
    if(!find_attribute(attributes, attribute, &value, &length)) return 0;
    end = value + length;
    while(value < end) {
        const char *start;
        while(value < end && isspace((unsigned char)*value)) value++;
        start = value;
        while(value < end && !isspace((unsigned char)*value)) value++;
        if((size_t)(value - start) == token_length &&
           !strncasecmp(start, token, token_length)) return 1;
    }
    return 0;
}

static int article_body(const char *attributes) {
    /* These are established semantic/CMS content markers, not URL rules;
       arbitrary class substrings such as "post-content-menu" never match. */
    static const char *const classes[] = {
        "entry-content", "post-content", "article-body", "article__body",
        "mw-parser-output"
    };
    size_t i;
    if(attribute_token(attributes, "itemprop", "articleBody")) return 1;
    for(i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i)
        if(attribute_token(attributes, "class", classes[i])) return 1;
    return 0;
}

static int reader_navigation(const char *name, const char *attributes) {
    return !strcmp(name, "nav") || !strcmp(name, "aside") || !strcmp(name, "footer") ||
           attribute_token(attributes, "role", "navigation") ||
           attribute_token(attributes, "role", "complementary") ||
           attribute_token(attributes, "role", "menu") ||
           attribute_token(attributes, "role", "menubar") ||
           attribute_token(attributes, "role", "toolbar") ||
           attribute_token(attributes, "class", "infobox");
}

static int fragment_decode(const char *text, char *out, size_t capacity);

static uint32_t anchor_hash(const char *text) {
    uint32_t hash = 2166136261u;
    while(*text) hash = (hash ^ (unsigned char)*text++) * 16777619u;
    return hash;
}

/* A small hash set ranks the anchors actually referenced by the document.
   Hash collisions only preserve an extra incidental ID, never misdirect a
   navigation: the final lookup still compares the complete target name. */
static void remember_fragment_target(const char *attributes) {
    char href[MAX_URL], name[MAX_ANCHOR_NAME];
    const char *fragment;
    uint32_t hash;
    int i;
    if(!attr_url(attributes, "href", href, sizeof(href)) ||
       !(fragment = strchr(href, '#')) ||
       !fragment_decode(fragment + 1, name, sizeof(name)) || !name[0]) return;
    hash = anchor_hash(name);
    for(i = 0; i < anchor_target_count; ++i)
        if(anchor_targets[i] == hash) return;
    if(anchor_target_count < MAX_ANCHORS) anchor_targets[anchor_target_count++] = hash;
}

static void reader_region(const char *html, const char *end,
                           const char **begin, const char **finish) {
    const char *p = html;
    const char *main_start = NULL, *main_end = NULL;
    const char *article_start = NULL, *article_end = NULL;
    const char *body_start = NULL, *body_end = NULL;
    size_t article_size = 0;
    size_t body_size = 0;
    int article_count = 0;
    *begin = *finish = NULL;
    while(p < end) {
        const char *start = memchr(p, '<', (size_t)(end - p));
        const char *close, *attrs;
        char name[32], attributes[2048], role[32];
        size_t length;
        int closing;
        if(!start) break;
        if(end - start >= 4 && !memcmp(start, "<!--", 4)) {
            const char *comment = find_ci(start + 4, (size_t)(end - start - 4), "-->");
            p = comment ? comment + 3 : end;
            continue;
        }
        close = tag_end(start + 1, end);
        if(!close) break;
        attrs = tag_name(start + 1, close, name, sizeof(name), &closing);
        p = close + 1;
        if(closing) continue;
        length = (size_t)(close - attrs);
        if(length >= sizeof(attributes)) length = sizeof(attributes) - 1;
        memcpy(attributes, attrs, length);
        attributes[length] = 0;
        if(raw_tag(name) || !strcmp(name, "template") || !strcmp(name, "svg") ||
           !strcmp(name, "canvas") || hidden_element(attributes)) {
            if(!void_tag(name)) p = subtree_end(p, end, name);
            continue;
        }
        if(!strcmp(name, "a")) remember_fragment_target(attributes);
        role[0] = 0;
        attr_lower(attributes, "role", role, sizeof(role));
        if(!strcmp(name, "main") || !strcmp(role, "main")) {
            if(!main_start) {
                main_start = start;
                main_end = subtree_end(p, end, name);
            }
        }
        if(!strcmp(name, "article")) {
            const char *after = subtree_end(p, end, name);
            size_t size = (size_t)(after - start);
            if(size > article_size) {
                article_size = size;
                article_start = start;
                article_end = after;
            }
            article_count++;
        }
        if(article_body(attributes)) {
            const char *after = subtree_end(p, end, name);
            size_t size = (size_t)(after - start);
            if(size > body_size) {
                body_size = size;
                body_start = start;
                body_end = after;
            }
        }
    }
    if(body_start) { *begin = body_start; *finish = body_end; }
    else if(article_start && (!main_start || article_count == 1)) {
        *begin = article_start; *finish = article_end;
    } else if(main_start) { *begin = main_start; *finish = main_end; }
}

static int fragment_decode(const char *text, char *out, size_t capacity) {
    size_t n = 0;
    while(*text) {
        unsigned char ch = (unsigned char)*text++;
        if(ch == '%' && text[0] && text[1] &&
           isxdigit((unsigned char)text[0]) && isxdigit((unsigned char)text[1])) {
            int high = isdigit((unsigned char)text[0]) ? text[0] - '0' :
                       tolower((unsigned char)text[0]) - 'a' + 10;
            int low = isdigit((unsigned char)text[1]) ? text[1] - '0' :
                      tolower((unsigned char)text[1]) - 'a' + 10;
            ch = (unsigned char)(high * 16 + low);
            text += 2;
        }
        if(!ch || n + 1 >= capacity) return 0;
        out[n++] = (char)ch;
    }
    out[n] = 0;
    return 1;
}

static void add_anchor(browser_document_t *doc, const char *attributes,
                        const char *attribute, unsigned priority) {
    char encoded[MAX_URL], name[MAX_ANCHOR_NAME];
    int i, slot;
    uint32_t hash;
    if(!attr_url(attributes, attribute, encoded, sizeof(encoded)) || !encoded[0]) return;
    if(!fragment_decode(encoded, name, sizeof(name))) {
        doc->limit_flags |= DOCUMENT_LIMIT_ANCHORS;
        return;
    }
    for(i = 0; i < doc->anchor_count; ++i)
        if(!strcmp(doc->anchors[i].name, name)) return;
    hash = anchor_hash(name);
    for(i = 0; i < anchor_target_count; ++i)
        if(anchor_targets[i] == hash && priority < 2) { priority = 2; break; }
    slot = doc->anchor_count;
    if(doc->anchor_count >= MAX_ANCHORS) {
        doc->limit_flags |= DOCUMENT_LIMIT_ANCHORS;
        slot = -1;
        for(i = 0; i < doc->anchor_count; ++i) {
            if(doc->anchors[i].priority < priority &&
               (slot < 0 || doc->anchors[i].priority < doc->anchors[slot].priority))
                slot = i;
        }
        if(slot < 0) return;
    } else {
        doc->anchor_count++;
    }
    copy_text(doc->anchors[slot].name, MAX_ANCHOR_NAME, name);
    doc->anchors[slot].item = doc->item_count;
    doc->anchors[slot].priority = priority;
    merge_barrier = 1;
}

int document_anchor_y(const browser_document_t *doc, const char *fragment) {
    char name[MAX_ANCHOR_NAME];
    int i;
    if(!fragment) return -1;
    if(*fragment == '#') fragment++;
    if(!*fragment) return 0;
    if(!fragment_decode(fragment, name, sizeof(name))) return -1;
    for(i = 0; i < doc->anchor_count; ++i) {
        if(!strcmp(doc->anchors[i].name, name)) {
            int item = doc->anchors[i].item;
            return item < doc->item_count ? doc->items[item].y : doc->height;
        }
    }
    return !strcasecmp(name, "top") ? 0 : -1;
}

void document_parse_html(browser_document_t *doc, const char *html, size_t size,
                         const char *content_type) {
    document_parse_html_mode(doc, html, size, content_type, 0);
}

void document_parse_html_mode(browser_document_t *doc, const char *html, size_t size,
                              const char *content_type, int reader_requested) {
    const char *p = html;
    const char *end = html + size;
    const char *reader_start = NULL, *reader_end = NULL;
    const char *hidden_end = NULL;
    parse_state_t st;

    memset(&st, 0, sizeof(st));
    st.current_link = -1;
    st.current_form = -1;
    st.current_button = -1;
    st.current_select = -1;
    st.current_option = -1;
    encoding = detect_encoding(html, size, content_type);
    line_y = 12;
    line_indent = 0;
    reset_line();
    pending_space = 0;
    pending_gap = 0;
    merge_barrier = 0;
    anchor_target_count = 0;

    if(!content_type || strncasecmp(content_type, "text/plain", 10))
        reader_region(html, end, &reader_start, &reader_end);
    doc->reader_available = reader_start != NULL;
    doc->reader_active = reader_requested && doc->reader_available;

    /* Plain text is shown verbatim: no tags, no character references. */
    if(content_type && !strncasecmp(content_type, "text/plain", 10)) {
        st.in_pre = 1;
        render_text_node(doc, &st, html, size, 0);
        p = end;
    }

    while(p < end && !doc->truncated) {
        if(hidden_end && p >= hidden_end) {
            st.hidden = 0;
            hidden_end = NULL;
        }
        if(end - p >= 4 && !memcmp(p, "<!--", 4)) {
            const char *comment_end = p + 4;
            while(end - comment_end >= 3 && memcmp(comment_end, "-->", 3))
                comment_end++;
            p = end - comment_end >= 3 ? comment_end + 3 : end;
            continue;
        }
        if(*p != '<') {
            const char *next = memchr(p, '<', (size_t)(end - p));
            size_t count = next ? (size_t)(next - p) : (size_t)(end - p);
            if(!doc->reader_active || st.in_title ||
               (p >= reader_start && p < reader_end))
                render_text_node(doc, &st, p, count, 1);
            p += count;
            continue;
        }

        {
            const char *start = p;
            const char *close = tag_end(p + 1, end);
            char tag[2048];
            char name[32];
            char attr[MAX_URL];
            const char *q;
            size_t len;
            int closing = 0;
            int self_closing;
            int ni = 0;
            if(!close) break;
            self_closing = close > p && close[-1] == '/';
            len = (size_t)(close - p - 1);
            if(len >= sizeof(tag)) len = sizeof(tag) - 1;
            memcpy(tag, p + 1, len);
            tag[len] = 0;
            q = tag;
            p = close + 1;
            while(*q && isspace((unsigned char)*q)) q++;
            if(*q == '!' || *q == '?') continue;
            if(*q == '/') { closing = 1; q++; }
            while(*q && isspace((unsigned char)*q)) q++;
            while(*q && !isspace((unsigned char)*q) && *q != '/' && ni < 31)
                name[ni++] = (char)tolower((unsigned char)*q++);
            name[ni] = 0;

            if(!closing && (!strcmp(name, "script") ||
               !strcmp(name, "style") || !strcmp(name, "svg") ||
               !strcmp(name, "canvas") || !strcmp(name, "template") ||
               (doc->reader_active && reader_navigation(name, q)))) {
                if(!void_tag(name) && !self_closing) p = subtree_end(p, end, name);
                continue;
            }

            /* Metadata survives selecting only the article/main region. */
            if(!strcmp(name, "head")) { st.in_head = !closing; continue; }
            if(!strcmp(name, "body")) { st.in_head = 0; continue; }
            if(!strcmp(name, "title")) { st.in_title = !closing; continue; }
            if(!strcmp(name, "base") && !closing && !st.base_seen) {
                char resolved[MAX_URL];
                st.base_seen = 1;
                if(attr_url(q, "href", attr, sizeof(attr)) &&
                   !resolve_url(doc->base_url, attr, resolved, sizeof(resolved)))
                    snprintf(doc->base_url, sizeof(doc->base_url), "%s", resolved);
                continue;
            }
            if(doc->reader_active && (start < reader_start || start >= reader_end))
                continue;
            if(!closing && !st.hidden && hidden_element(q)) {
                /* Invisible controls still participate in form submission.
                   Keep parsing form structure while suppressing its layout. */
                hidden_end = void_tag(name) || self_closing ? p : subtree_end(p, end, name);
                st.hidden = 1;
            }
            if(!closing && !st.in_head && !st.hidden) {
                unsigned priority = name[0] == 'h' && name[1] >= '1' &&
                                    name[1] <= '6' && !name[2] ? 3 : 0;
                add_anchor(doc, q, "id", priority);
                if(!strcmp(name, "a")) add_anchor(doc, q, "name", 3);
            }

            /* Inside a <select>, only its options matter. */
            if(st.current_select >= 0) {
                if(!strcmp(name, "option")) {
                    if(closing) finish_option(doc, &st);
                    else start_option(doc, &st, q);
                } else if(!strcmp(name, "select") && closing) {
                    finish_select(doc, &st);
                } else if(!strcmp(name, "form") && closing) {
                    finish_select(doc, &st);
                    st.current_form = -1;
                }
                continue;
            }

            if(!strcmp(name, "form")) {
                if(!st.hidden) vertical_space(5);
                if(st.current_button >= 0) finish_button(doc, &st);
                if(closing) st.current_form = -1;
                else if(doc->form_count < MAX_FORMS) {
                    browser_form_t *form = &doc->forms[doc->form_count];
                    char method[16] = "get";
                    st.current_form = doc->form_count++;
                    attr_lower(q, "method", method, sizeof(method));
                    if(!method[0]) snprintf(method, sizeof(method), "get");
                    form->post = !strcmp(method, "post");
                    if(!attr_url(q, "action", attr, sizeof(attr)) || !attr[0])
                        snprintf(attr, sizeof(attr), "%s", doc->base_url);
                    form->valid = resolve_url(doc->base_url, attr, form->action,
                                              sizeof(form->action)) == 0 &&
                                  (!strcmp(method, "post") || !strcmp(method, "get"));
                } else {
                    st.current_form = -1;
                    doc->limit_flags |= DOCUMENT_LIMIT_FORMS;
                }
                continue;
            }
            if(!strcmp(name, "input") && !closing) {
                if(st.current_form >= 0) handle_input(doc, &st, q);
                continue;
            }
            if(!strcmp(name, "textarea") && !closing) {
                p = handle_textarea(doc, &st, q, p, end);
                continue;
            }
            if(!strcmp(name, "select") && !closing && st.current_form >= 0) {
                browser_field_t *field = new_field(doc, st.current_form, "select", q, st.hidden);
                if(field) {
                    field->option_first = doc->option_count;
                    st.current_select = doc->field_count++;
                }
                continue;
            }
            if(!strcmp(name, "button")) {
                if(closing) {
                    finish_button(doc, &st);
                } else if(st.current_form >= 0 && st.current_button < 0) {
                    char type[16] = "submit";
                    attr_lower(q, "type", type, sizeof(type));
                    if(!type[0] || !strcmp(type, "submit")) {
                        browser_field_t *field = new_field(doc, st.current_form, "submit", q, st.hidden);
                        if(field) {
                            attr_form_value(q, "value", field->value,
                                            sizeof(field->value));
                            st.current_button = doc->field_count++;
                            st.label_space = 0;
                        }
                    }
                }
                continue;
            }

            if(st.hidden) continue;

            if(!strcmp(name, "pre")) {
                vertical_space(closing ? 5 : 8);
                if(closing) { if(st.in_pre) st.in_pre--; }
                else st.in_pre++;
            } else if(!strcmp(name, "code")) {
                if(closing) { if(st.in_code) st.in_code--; }
                else st.in_code++;
            } else if(!strcmp(name, "b") || !strcmp(name, "strong")) {
                if(closing) { if(st.in_strong) st.in_strong--; }
                else st.in_strong++;
            } else if(!strcmp(name, "i") || !strcmp(name, "em")) {
                if(closing) { if(st.in_emphasis) st.in_emphasis--; }
                else st.in_emphasis++;
            } else if(!strcmp(name, "a")) {
                if(closing) {
                    st.current_link = -1;
                    st.adjacent_link = 1;
                } else {
                    if(st.adjacent_link && line_has_content && !pending_space)
                        pending_space = 1;
                    if(attr_url(q, "href", attr, sizeof(attr)))
                        st.current_link = add_link(doc, attr);
                    st.adjacent_link = 0;
                }
            } else if(!strcmp(name, "img") && !closing && !st.in_head) {
                char alt[64] = {0};
                char aria[64] = {0};
                char role[24] = {0};
                char dimension[16];
                int has_alt = attr_text(q, "alt", alt, sizeof(alt));
                int image_width = 0;
                int image_height = 0;
                if(st.current_button >= 0) {
                    /* An icon inside a button labels it only as a fallback. */
                    browser_field_t *button = &doc->fields[st.current_button];
                    if(!button->label[0])
                        copy_text(button->label, sizeof(button->label), alt);
                    continue;
                }
                if(!attr_url(q, "src", attr, sizeof(attr))) continue;
                attr_value(q, "aria-hidden", aria, sizeof(aria));
                attr_value(q, "role", role, sizeof(role));
                if((has_alt && !alt[0]) || !strcasecmp(aria, "true") ||
                   !strcasecmp(role, "presentation") || !strcasecmp(role, "none"))
                    continue;
                if(attr_text(q, "aria-label", aria, sizeof(aria))) {
                    if(!strcasecmp(aria, "image unavailable")) continue;
                    if(!has_alt) snprintf(alt, sizeof(alt), "%s", aria);
                }
                if(attr_value(q, "width", dimension, sizeof(dimension)))
                    image_width = atoi(dimension);
                if(attr_value(q, "height", dimension, sizeof(dimension)))
                    image_height = atoi(dimension);
                add_image(doc, attr, alt, image_width, image_height);
            } else if(name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
                vertical_space(closing ? 7 : 10);
                if(closing) { if(st.in_heading) st.in_heading--; }
                else st.in_heading++;
            } else if(is_block_tag(name)) {
                vertical_space(closing ? 7 : 4);
            } else if(!strcmp(name, "tr")) {
                vertical_space(closing ? 2 : 3);
            } else if(!strcmp(name, "td") || !strcmp(name, "th")) {
                if(!closing && line_has_content && !pending_space)
                    pending_space = 1;
            } else if(!strcmp(name, "br")) {
                finish_line(1);
            } else if(!strcmp(name, "li")) {
                if(closing) {
                    finish_line(0);
                    line_indent = 0;
                    reset_line();
                    st.list_marker_pending = 0;
                } else {
                    vertical_space(3);
                    line_indent = 12;
                    reset_line();
                    st.list_marker_pending = 1;
                }
            } else if(!strcmp(name, "hr")) {
                document_item_t *item = new_item(doc);
                vertical_space(5);
                if(item) {
                    apply_gap();
                    item->type = ITEM_RULE;
                    item->x = PAGE_MARGIN;
                    item->y = line_y;
                    item->width = PAGE_WIDTH;
                    item->height = 1;
                    line_y += 10;
                }
            } else if(!is_ignored_tag(name) && strcmp(name, "input") &&
                      strcmp(name, "textarea") && strcmp(name, "form") &&
                      strcmp(name, "head") && strcmp(name, "title")) {
                doc->unsupported_count++;
            }
        }
    }

    if(st.current_select >= 0) finish_select(doc, &st);
    if(st.current_button >= 0) finish_button(doc, &st);
    if(doc->limit_flags & DOCUMENT_LIMIT_LAYOUT)
        add_notice(doc, "[Page shortened: layout limit reached]");
    if(doc->limit_flags & DOCUMENT_LIMIT_LINKS)
        add_notice(doc, "[Link limit: some links are unavailable]");
    if(doc->limit_flags & DOCUMENT_LIMIT_ANCHORS)
        add_notice(doc, "[Some section targets could not be stored]");
    if(doc->limit_flags & DOCUMENT_LIMIT_FORMS)
        add_notice(doc, "[Form limit: some controls are unavailable]");
    if(doc->limit_flags & DOCUMENT_LIMIT_IMAGES)
        add_notice(doc, "[Additional images omitted: page image limit]");
#ifndef BROWSER_QUIET
    if(doc->unsupported_count)
        printf("browser: ignored %d unsupported HTML elements\n", doc->unsupported_count);
#endif
    finish_line(0);
    doc->height = line_y + 20;
    document_touch(doc);
}

void document_reflow(browser_document_t *doc) {
    int i;
    int shift = 0;
    for(i = 0; i < doc->item_count; ++i) {
        document_item_t *item = &doc->items[i];
        item->y += shift;
        if(item->type == ITEM_IMAGE && item->image_id >= 0) {
            browser_image_t *image = &doc->images[item->image_id];
            int old_height = item->height;
            int compact = doc->reader_active && image->loaded <= 0;
            item->width = compact ? PAGE_WIDTH :
                          image->width > 0 ? image->width : PAGE_WIDTH;
            item->height = compact ? READER_IMAGE_PLACEHOLDER_HEIGHT :
                           image->height > 0 ? image->height : 72;
            shift += item->height - old_height;
        }
    }
    doc->height += shift;
    document_touch(doc);
}
