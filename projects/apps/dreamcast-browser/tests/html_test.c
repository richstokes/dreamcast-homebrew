#include "browser.h"
#include "test.h"

#include <stdlib.h>

static browser_document_t doc;

static void parse_as(const char *html, size_t size, const char *content_type) {
    document_free(&doc);
    document_init(&doc, "https://example.com/dir/page.html");
    document_parse_html(&doc, html, size, content_type);
}

static void parse(const char *html) {
    parse_as(html, strlen(html), NULL);
}

/* The laid-out page as text: runs on one row are joined, rows by '\n'. */
static const char *page_text(void) {
    static char text[65536];
    size_t n = 0;
    int row = -1;
    int i;
    text[0] = 0;
    for(i = 0; i < doc.item_count; ++i) {
        const document_item_t *item = &doc.items[i];
        if(item->type != ITEM_TEXT && item->type != ITEM_NOTICE) continue;
        if(row >= 0 && item->y != row && n + 1 < sizeof(text)) text[n++] = '\n';
        row = item->y;
        n += (size_t)snprintf(text + n, sizeof(text) - n, "%s", item->text);
        if(n >= sizeof(text)) n = sizeof(text) - 1;
    }
    return text;
}

/* A missing field is a failure; an empty placeholder keeps checks safe. */
static browser_field_t *field_named(const char *name) {
    static browser_field_t missing;
    int i;
    for(i = 0; i < doc.field_count; ++i)
        if(!strcmp(doc.fields[i].name, name)) return &doc.fields[i];
    test_checks++;
    test_failures++;
    printf("  FAIL: no field named \"%s\"\n", name);
    memset(&missing, 0, sizeof(missing));
    missing.item = -1;
    return &missing;
}

static const char *field_text(const browser_field_t *field) {
    if(!field || field->item < 0) return "(no item)";
    return doc.items[field->item].text;
}

static void test_entities(void) {
    parse("<p>A&mdash;B &ndash; &lsquo;x&rsquo; &ldquo;y&rdquo; wait&hellip;</p>");
    CHECK_STR(page_text(), "A--B - 'x' \"y\" wait...");
    parse("<p>&#8217;&#x2026;&#x201C;&#150;&#8364;</p>");
    CHECK_STR(page_text(), "'...\"-EUR");
    parse("<p>caf&eacute; &copy; &AElig;&szlig; 5&times;2</p>");
    CHECK_STR(page_text(), "caf\xe9 \xa9 \xc6\xdf 5\xd7" "2");
    parse("<p>&amp;amp; &bogus; AT&T &lt;tag&gt; &nbsp x</p>");
    CHECK_STR(page_text(), "&amp; &bogus; AT&T <tag> x");
    parse("<p>&#0; &#xD800; &#1114112;</p>");
    CHECK_STR(page_text(), "? ? ?");
}

static void test_utf8(void) {
    parse("<p>caf\xc3\xa9 \xe2\x80\x9cq\xe2\x80\x9d it\xe2\x80\x99s \xe2\x80\x94 ok</p>");
    CHECK_STR(page_text(), "caf\xe9 \"q\" it's -- ok");
    parse("<p>\xc5\x81\xc3\xb3\x64\xc5\xba \xc5\x92uvre</p>");
    CHECK_STR(page_text(), "L\xf3" "dz OEuvre");
    /* Each person in a joined emoji is one '?'; joiners and selectors vanish. */
    parse("<p>hi \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xef\xb8\x8f!</p>");
    CHECK_STR(page_text(), "hi ?" "?!");
    parse("<p>zero\xe2\x80\x8bwidth soft\xc2\xadhyphen</p>");
    CHECK_STR(page_text(), "zerowidth softhyphen");
    /* One stray byte proves the page is not UTF-8. */
    parse("<p>\x93quoted\x94 caf\xe9</p>");
    CHECK_STR(page_text(), "\"quoted\" caf\xe9");
}

static void test_charset(void) {
    const char legacy[] = "<p>\x93hi\x94 \x85</p>";
    const char utf8[] = "<p>caf\xc3\xa9</p>";
    parse_as(legacy, sizeof(legacy) - 1, "text/html; charset=ISO-8859-1");
    CHECK_STR(page_text(), "\"hi\" ...");
    parse_as(utf8, sizeof(utf8) - 1, "text/html; charset=\"utf-8\"");
    CHECK_STR(page_text(), "caf\xe9");
    parse("<meta charset=windows-1252><p>caf\xc3\xa9</p>");
    CHECK_STR(page_text(), "caf\xc3\xa9");
    parse("<meta http-equiv=Content-Type content='text/html; charset=utf-8'>"
          "<p>\xc3\xa9</p>");
    CHECK_STR(page_text(), "\xe9");
    parse("\xef\xbb\xbf<p>BOM \xc3\xa9</p>");
    CHECK_STR(page_text(), "BOM \xe9");
    /* A document cut mid-sequence at the size limit is still UTF-8. */
    {
        const char cut[] = "<p>caf\xc3\xa9 more \xe2\x80";
        parse_as(cut, sizeof(cut) - 1, NULL);
        CHECK(strstr(page_text(), "caf\xe9") != NULL);
    }
}

static void test_plain_text(void) {
    const char text[] = "<b>not bold</b> &amp;\r\nline two";
    parse_as(text, sizeof(text) - 1, "text/plain; charset=utf-8");
    CHECK_STR(page_text(), "<b>not bold</b> &amp;\nline two");
}

static void test_line_endings(void) {
    parse("<pre>a\r\nb\rc</pre>");
    CHECK_STR(page_text(), "a\nb\nc");
}

static void test_long_text(void) {
    static char html[20000];
    size_t n = 0;
    int i;
    int words = 0;
    const char *text;
    n += (size_t)snprintf(html + n, sizeof(html) - n, "<p>");
    for(i = 0; i < 700; ++i)
        n += (size_t)snprintf(html + n, sizeof(html) - n, "w%03d ", i);
    snprintf(html + n, sizeof(html) - n, "END</p>");
    parse(html);
    text = page_text();
    CHECK(strstr(text, "w000") != NULL);
    CHECK(strstr(text, "w699 END") != NULL || strstr(text, "w699\nEND") != NULL);
    for(i = 0; text[i]; ++i) if(text[i] == 'w') words++;
    CHECK(words == 700);
    for(i = 0; i < doc.item_count; ++i)
        CHECK(doc.items[i].x + doc.items[i].width <= PAGE_MARGIN + PAGE_WIDTH);
}

static void test_attributes(void) {
    parse("<a title='x href=/wrong' href=\"/right\">go</a>");
    CHECK(doc.link_count == 1);
    CHECK_STR(doc.links[0], "https://example.com/right");
    parse("<img alt=\"src=bad.png\" src='good.png' width=10 height=10>");
    CHECK(doc.image_count == 1);
    CHECK_STR(doc.images[0].url, "https://example.com/dir/good.png");
    CHECK_STR(doc.images[0].alt, "src=bad.png");
    parse("<img src=a.png alt=\"Fish &amp; Chips\">");
    CHECK_STR(doc.images[0].alt, "Fish & Chips");
    parse("<img src='http://cdn.test/i.png' alt=x>");
    CHECK_STR(doc.images[0].url, "https://cdn.test/i.png");
    document_free(&doc);
    document_init(&doc, "http://plain.test/");
    document_parse_html(&doc, "<img src='http://cdn.test/i.png' alt=x>", 39, NULL);
    CHECK_STR(doc.images[0].url, "http://cdn.test/i.png");
    {
        /* An upgrade that cannot fit drops the image rather than cut its URL. */
        static char html[MAX_URL + 64];
        char url[MAX_URL];
        memset(url, 'a', sizeof(url));
        memcpy(url, "http://x.test/", 14);
        url[MAX_URL - 1] = 0;
        snprintf(html, sizeof(html), "<img src='%s' alt=x>", url);
        parse(html);
        CHECK(doc.image_count == 0);
        url[MAX_URL - 2] = 0;
        snprintf(html, sizeof(html), "<img src='%s' alt=x>", url);
        parse(html);
        CHECK(doc.image_count == 1 && strlen(doc.images[0].url) == MAX_URL - 1 &&
              !strncmp(doc.images[0].url, "https://x.test/", 15));
    }
}

static void test_urls(void) {
    parse("<a href=\"/search?a=1&amp;b=2\">x</a>"
          "<a href=\"  /caf\xc3\xa9 page\n/x \">y</a>"
          "<a href='/?q=&copy=1'>z</a>");
    CHECK_STR(doc.links[0], "https://example.com/search?a=1&b=2");
    CHECK_STR(doc.links[1], "https://example.com/caf%C3%A9%20page/x");
    CHECK_STR(doc.links[2], "https://example.com/?q=&copy=1");
    parse("<meta charset=iso-8859-1><a href='/caf\xe9'>x</a>");
    CHECK_STR(doc.links[0], "https://example.com/caf%C3%A9");
    parse("<base href='https://cdn.example.org/assets/'><a href='x.html'>x</a>");
    CHECK_STR(doc.links[0], "https://cdn.example.org/assets/x.html");
}

static void test_structure(void) {
    parse("<html><head><title>T &amp; U</title><body><p>Visible</p>");
    CHECK_STR(doc.title, "T & U");
    CHECK_STR(page_text(), "Visible");
    parse("<title>  </title><p>x</p>");
    CHECK_STR(doc.title, "Untitled page");
    parse("<h4>Minor</h4><p>After</p>");
    CHECK(doc.item_count == 2 && doc.items[0].style == TEXT_HEADING);
    parse("<template><p>hidden</p></template><p>shown</p>");
    CHECK_STR(page_text(), "shown");
}

static void test_checkbox_and_radio(void) {
    browser_field_t *a;
    browser_field_t *b;
    parse("<form action=/f method=post>"
          "<input type=checkbox name=plain class='checked-box' data-x=checked>"
          "<input type=CHECKBOX name=on checked> Remember me"
          "<input type=radio name=size value=s checked> Small"
          "<input type=radio name=size value=l> Large</form>");
    CHECK(field_named("plain") && !field_named("plain")->checked);
    CHECK(field_named("on") && field_named("on")->checked);
    CHECK_STR(field_text(field_named("on")), "[X]");
    CHECK(strstr(page_text(), "[X] Remember me") != NULL);
    a = &doc.fields[2];
    b = &doc.fields[3];
    CHECK(a->checked && !b->checked);
    document_toggle_field(&doc, 3);
    CHECK(!a->checked && b->checked);
    CHECK_STR(field_text(a), "( )");
    CHECK_STR(field_text(b), "(*)");
    document_toggle_field(&doc, 0);
    CHECK(field_named("plain")->checked);
    CHECK(doc.forms[0].valid);
}

static void test_buttons(void) {
    parse("<form action=/f method=post><input name=q>"
          "<button name=go value=1><img src=i.png alt=icon> Search <b>now</b></button>"
          "<button type=button>Menu</button><input type=reset></form>");
    CHECK(doc.forms[0].valid);
    CHECK(field_named("go") && document_field_is_submit(field_named("go")));
    CHECK_STR(field_named("go")->value, "1");
    CHECK_STR(field_named("go")->label, "icon Search now");
    CHECK_STR(field_text(field_named("go")), "[ icon Search now ]");
    CHECK(doc.image_count == 0);
    CHECK(strstr(page_text(), "Menu") != NULL);
    CHECK(doc.field_count == 2);
    parse("<form><button>Send</button></form>");
    CHECK(doc.field_count == 1 && !strcmp(doc.fields[0].label, "Send"));
    CHECK_STR(doc.forms[0].action, "https://example.com/dir/page.html");
    parse("<form><input type=image name=pic alt='Go now'></form>");
    CHECK(document_field_is_submit(&doc.fields[0]));
    CHECK_STR(field_text(&doc.fields[0]), "[ Go now ]");
}

static void test_textarea(void) {
    browser_field_t *field;
    parse("<form><textarea name=msg maxlength=40>\nHello <b>&amp;\r\nbye</textarea>"
          "<p>after</p></form>");
    field = field_named("msg");
    CHECK(field && document_field_is_text(field));
    CHECK_STR(field ? field->value : NULL, "Hello <b>&\nbye");
    CHECK_STR(field_text(field), "[ Hello <b>&\xb6" "bye ]");
    CHECK(strstr(page_text(), "after") != NULL);
    CHECK(strstr(page_text(), "<b>&\n") == NULL);
    field->caret = 5;
    document_refresh_field(&doc, (int)(field - doc.fields));
    CHECK_STR(field_text(field), "[ Hello| <b>&\xb6" "bye ]");
    parse("<textarea>loose <i>text</i></textarea>");
    CHECK(doc.field_count == 0);
    CHECK_STR(page_text(), "loose text");
}

static void test_select(void) {
    browser_field_t *field;
    int index;
    parse("<form><select name=country><option value=us>United States"
          "<option value=uk selected>United Kingdom</option>"
          "<optgroup label=x><option>France</option></optgroup>"
          "<p>ignored</p></select><p>after</p></form>");
    field = field_named("country");
    CHECK(field && field->option_count == 3 && field->selected == 1);
    CHECK_STR(field ? field->value : NULL, "uk");
    CHECK_STR(field_text(field), "[ United Kingdom v]");
    CHECK(strstr(page_text(), "ignored") == NULL);
    CHECK(strstr(page_text(), "after") != NULL);
    index = (int)(field - doc.fields);
    document_select_option(&doc, index, 2);
    CHECK_STR(field->value, "France");
    CHECK_STR(field_text(field), "[ France v]");
    document_select_option(&doc, index, 99);
    CHECK(field->selected == 2);
    field->caret = 0;
    document_refresh_field(&doc, index);
    CHECK_STR(field_text(field), "[< France >]");
    parse("<form><select name=s><option>One<option>Two</select></form>");
    CHECK(doc.fields[0].selected == 0);
    CHECK_STR(doc.fields[0].value, "One");
}

static void test_input_types(void) {
    parse("<form><input type=search name=a><input type=tel name=b>"
          "<input type=fancy name=c><input type=hidden name=d value='x  y'>"
          "<input name=e value=' spaced  value '><input name=f disabled></form>");
    CHECK(doc.forms[0].valid);
    CHECK(document_field_is_text(field_named("a")));
    CHECK_STR(field_named("c")->type, "text");
    CHECK_STR(field_named("d")->value, "x  y");
    CHECK(field_named("d")->item < 0);
    CHECK_STR(field_named("e")->value, " spaced  value ");
    CHECK(field_named("f")->disabled && field_named("f")->item < 0);
    parse("<form><input type=file name=upload></form>");
    CHECK(!doc.forms[0].valid);
    parse("<form><input type=password name=p value=secret></form>");
    CHECK_STR(field_text(&doc.fields[0]), "[ ****** ]");
}

/* Stitches random HTML-ish fragments together and checks that parsing
   never breaks the layout invariants the renderer relies on. */
static void test_random_documents(void) {
    static const char *const fragments[] = {
        "<p>", "</p>", "<a href='/x'>", "</a>", "<form action=/f method=post>",
        "</form>", "<input name=a value='&amp;'>", "<input type=radio name=r>",
        "<select name=s>", "<option value=1 selected>", "</option>", "</select>",
        "<button>", "</button>", "<textarea name=t>", "</textarea>", "<pre>",
        "</pre>", "<img src=i.png alt=x width=900 height=900>", "<li>", "<br>",
        "<!--", "-->", "<script>", "</script>", "&", "&#", "&#x2014;", "&mdash",
        "\xe2\x80", "\x99", "\xc3", "word ", "longwordwithoutanyspacesatall",
        "<", ">", "\"", "'", "=", "<title>", "</title>", "<h1>", "</h1>",
        "\r\n", "\n", "<base href=", "<b>", "<table><tr><td>", "\xf0\x9f\x98\x80"
    };
    static char html[8192];
    unsigned seed = 12345;
    int round;

    for(round = 0; round < 3000; ++round) {
        size_t n = 0;
        int pieces = 1 + (int)(seed % 120);
        int i;
        while(pieces--) {
            const char *piece;
            seed = seed * 1103515245u + 12345u;
            piece = fragments[(seed >> 8) % (sizeof(fragments) / sizeof(fragments[0]))];
            if(n + strlen(piece) >= sizeof(html)) break;
            memcpy(html + n, piece, strlen(piece));
            n += strlen(piece);
        }
        parse_as(html, n, (seed & 0x10000) ? "text/html; charset=utf-8" : NULL);
        CHECK(doc.item_count <= MAX_ITEMS && doc.link_count <= MAX_LINKS &&
              doc.field_count <= MAX_FIELDS && doc.option_count <= MAX_OPTIONS);
        for(i = 0; i < doc.item_count; ++i) {
            const document_item_t *item = &doc.items[i];
            if(item->type == ITEM_TEXT && item->link_id < 0)
                CHECK(item->x + item->width <= PAGE_MARGIN + PAGE_WIDTH);
            CHECK(strlen(item->text) < MAX_TEXT && item->y >= 0);
            CHECK(item->link_id < doc.link_count && item->image_id < doc.image_count);
        }
        for(i = 0; i < doc.field_count; ++i) {
            const browser_field_t *field = &doc.fields[i];
            CHECK(field->item < doc.item_count && field->form < doc.form_count &&
                  field->option_first + field->option_count <= doc.option_count);
        }
        if(test_failures) break;
    }
}

void html_tests(void) {
    test_entities();
    test_utf8();
    test_charset();
    test_plain_text();
    test_line_endings();
    test_long_text();
    test_attributes();
    test_urls();
    test_structure();
    test_checkbox_and_radio();
    test_buttons();
    test_textarea();
    test_select();
    test_input_types();
    test_random_documents();
    document_free(&doc);
}
