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

static void parse_reader(const char *html, int requested) {
    document_free(&doc);
    document_init(&doc, "https://example.com/dir/page.html");
    document_parse_html_mode(&doc, html, strlen(html), NULL, requested);
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

static void test_document_generations(void) {
    browser_document_t *first = malloc(sizeof(*first));
    browser_document_t *second = malloc(sizeof(*second));
    unsigned initial, changed, replacement;
    CHECK(first != NULL && second != NULL);
    if(!first || !second) {
        free(first);
        free(second);
        return;
    }

    /* Initializing an allocation must not depend on its previous contents.
       Equal nonzero poison makes accidental per-object serials collide. */
    memset(first, 0xa5, sizeof(*first));
    memset(second, 0xa5, sizeof(*second));
    document_init(first, "https://first.test/");
    initial = first->generation;
    CHECK(initial != 0 && first->item_count == 0 && first->image_count == 0);
    CHECK_STR(first->base_url, "https://first.test/");
    document_touch(first);
    changed = first->generation;
    CHECK(changed != initial);

    /* Independent documents must not collide after a mutation, and reuse
       of the same object must invalidate its previous rendered page. */
    document_init(second, "https://second.test/");
    CHECK(second->generation != initial && second->generation != changed);
    first->height = 1000;
    document_init(first, "https://replacement.test/");
    replacement = first->generation;
    CHECK(replacement != initial && replacement != changed &&
          replacement != second->generation);
    CHECK(first->height == 0);
    document_free(first);
    CHECK(first->generation != replacement && first->generation != second->generation);
    document_free(second);
    free(first);
    free(second);
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

static void test_reader_and_hidden(void) {
    const char *html = "<head><title>A useful article</title><base href='/articles/'></head>"
        "<header>Site header<nav><a href='/menu'>Menu</a></nav></header>"
        "<main id=content><header><h1>Heading</h1></header><nav>Tools</nav>"
        "<p>Article <b>body</b> text.</p><aside>Related</aside>"
        "<form action=search><input name=q value=sega><button>Search</button></form>"
        "<footer>Article links</footer></main><footer>Site footer</footer>";
    parse_reader(html, 1);
    CHECK(doc.reader_available && doc.reader_active);
    CHECK_STR(doc.title, "A useful article");
    CHECK(strstr(page_text(), "Heading") && strstr(page_text(), "Article body text."));
    CHECK(!strstr(page_text(), "Menu") && !strstr(page_text(), "Tools") &&
          !strstr(page_text(), "Related") && !strstr(page_text(), "footer"));
    CHECK(doc.form_count == 1 && doc.forms[0].valid && doc.field_count == 2);
    CHECK_STR(doc.forms[0].action, "https://example.com/articles/search");
    CHECK(document_anchor_y(&doc, "content") == doc.items[0].y);
    parse_reader(html, 0);
    CHECK(doc.reader_available && !doc.reader_active);
    CHECK(strstr(page_text(), "Site header") && strstr(page_text(), "Related") &&
          strstr(page_text(), "Site footer"));
    parse_reader("<p>No semantic region</p>", 1);
    CHECK(!doc.reader_available && !doc.reader_active);
    CHECK_STR(page_text(), "No semantic region");
    parse_reader("<article>Short</article><article><h2>Main story</h2>"
                 "A much longer article goes here.</article>", 1);
    CHECK(doc.reader_active && !strstr(page_text(), "Short") &&
          strstr(page_text(), "Main story"));
    parse_reader("<div role=main><p>Useful</p></div><p>Other</p>", 1);
    CHECK_STR(page_text(), "Useful");
    parse_reader("<main hidden>Invisible</main><main>Real</main>", 1);
    CHECK_STR(page_text(), "Real");
    parse_reader("<main><header>Language menu</header><div class='article-body-menu'>Tools</div>"
                 "<article><div itemprop='articleBody'><h1>Story</h1><p>Article text</p>"
                 "<div role=navigation>Links</div><table class=infobox><tr><td>Specs</td>"
                 "</tr></table></div></article><aside>Related</aside></main>", 1);
    CHECK_STR(page_text(), "Story\nArticle text");
    parse_reader("<main><p>Index</p><article>First story</article>"
                 "<article>Second story</article></main>", 1);
    CHECK_STR(page_text(), "Index\nFirst story\nSecond story");
    parse("<div hidden><p>Hidden <br><img src=x></p><div>Nested</div></div>"
          "<p>Before <span style='color:red; DISPLAY : none !important'>Gone</span>after</p>"
          "<input hidden><svg/><p title='quoted > marker'>Visible</p>"
          "<p style='--display:none'>Kept</p><noscript>Fallback</noscript>"
          "<script>const x = '<main>'; const y = '<';</script><p>End</p>");
    CHECK_STR(page_text(), "Before after\nVisible\nKept\nFallback\nEnd");
    CHECK(doc.image_count == 0 && !doc.reader_available);
    parse("<form action=/f><div hidden><input name=token value=secret>"
          "<select name=choice><option selected>one</option></select>"
          "<textarea name=note>retained</textarea></div>"
          "<input style='display:none' name=trap value=''><button>Send</button></form>");
    CHECK(doc.form_count == 1 && doc.forms[0].valid && doc.field_count == 5);
    CHECK_STR(field_named("token")->value, "secret");
    CHECK_STR(field_named("choice")->value, "one");
    CHECK_STR(field_named("note")->value, "retained");
    CHECK(field_named("token")->item == -1 && field_named("choice")->item == -1 &&
          field_named("trap")->item == -1);
    CHECK_STR(page_text(), "[ Send ]");
    parse("<div><div><div><p>One</p></div></div></div><div><p>Two</p></div>");
    CHECK(doc.item_count == 2 && doc.items[1].y - doc.items[0].y <= 32);
}

static void test_anchors(void) {
    int y;
    parse("<a href='#chapter'>Skip</a><p>Intro</p><img src=a.png height=20>"
          "<h2 id=chapter>Chapter</h2><a name='old'>Legacy</a>"
          "<p id='caf&#233;'>Unicode</p><p id='space here'>Space</p>"
          "<p id=chapter>Duplicate</p><div hidden id=gone>Hidden</div>");
    CHECK_STR(doc.links[0], "https://example.com/dir/page.html#chapter");
    CHECK(doc.anchor_count == 4);
    y = document_anchor_y(&doc, "#chapter");
    CHECK(y > 0 && document_anchor_y(&doc, "old") > y);
    CHECK(document_anchor_y(&doc, "caf%C3%A9") > y);
    CHECK(document_anchor_y(&doc, "space%20here") > y);
    CHECK(document_anchor_y(&doc, "gone") == -1 && document_anchor_y(&doc, "missing") == -1);
    CHECK(document_anchor_y(&doc, "#") == 0 && document_anchor_y(&doc, "top") == 0);
    doc.images[0].height += 80;
    document_reflow(&doc);
    CHECK(document_anchor_y(&doc, "chapter") == y + 80);
}

static void test_reader_image_placeholders(void) {
    const char *html = "<main><p>Before</p><img src=large.png width=400 height=200>"
                       "<p id=after>After image</p><img src=other.png width=120 height=180>"
                       "<p id=end>End</p></main>";
    document_item_t *first = NULL, *second = NULL;
    int initial_after, initial_end, initial_height, i;
    parse_reader(html, 1);
    for(i = 0; i < doc.item_count; ++i) {
        if(doc.items[i].image_id == 0) first = &doc.items[i];
        if(doc.items[i].image_id == 1) second = &doc.items[i];
    }
    CHECK(first && second);
    if(!first || !second) return;
    CHECK(first->width == PAGE_WIDTH && first->height == 32 &&
          second->width == PAGE_WIDTH && second->height == 32);
    CHECK(doc.images[0].width == 400 && doc.images[0].height == 200 &&
          doc.images[1].width == 120 && doc.images[1].height == 180);
    initial_after = document_anchor_y(&doc, "after");
    initial_end = document_anchor_y(&doc, "end");
    initial_height = doc.height;

    /* A decoded image grows its row; all following text and section targets
       move by exactly that delta, while an unsuccessful image stays small. */
    doc.images[0].loaded = 1;
    doc.images[0].width = 320;
    doc.images[0].height = 120;
    doc.images[1].loaded = -1;
    document_reflow(&doc);
    CHECK(first->width == 320 && first->height == 120);
    CHECK(second->width == PAGE_WIDTH && second->height == 32);
    CHECK(document_anchor_y(&doc, "after") == initial_after + 88 &&
          document_anchor_y(&doc, "end") == initial_end + 88 &&
          doc.height == initial_height + 88);
    document_reflow(&doc);
    CHECK(document_anchor_y(&doc, "end") == initial_end + 88 &&
          doc.height == initial_height + 88);

    parse_reader(html, 0);
    doc.images[0].loaded = -1;
    document_reflow(&doc);
    for(i = 0; i < doc.item_count; ++i) {
        if(doc.items[i].image_id == 0)
            CHECK(doc.items[i].width == 400 && doc.items[i].height == 200);
        if(doc.items[i].image_id == 1)
            CHECK(doc.items[i].width == 120 && doc.items[i].height == 180);
    }
}

static void test_inline_wrapping(void) {
    int i;
    parse("<p>Words words words words words words words words the <b>Sega</b> "
          "Dreamcast is a <a href='/first'>first</a> generation <em>console</em>.</p>");
    CHECK(strstr(page_text(), "the Sega") || strstr(page_text(), "the\nSega"));
    CHECK(strstr(page_text(), "first generation") || strstr(page_text(), "first\ngeneration"));
    CHECK(strstr(page_text(), "console."));
    parse("<p>123456789012345678901234567890123456789012345 <b>wholeword</b></p>");
    CHECK_STR(page_text(), "123456789012345678901234567890123456789012345\nwholeword");
    for(i = 0; i < doc.item_count; ++i)
        CHECK(doc.items[i].x + doc.items[i].width <= PAGE_MARGIN + PAGE_WIDTH);
}

static void test_capacity_and_notices(void) {
    static char html[250000];
    size_t n = 0;
    int i;
    for(i = 0; i < 200; ++i)
        n += (size_t)snprintf(html + n, sizeof(html) - n,
                             "<p><a href='/story/%d'>Story %d</a></p>", i, i);
    snprintf(html + n, sizeof(html) - n,
             "<a href='/next'>More</a><form action='/search'><input name=q></form>");
    parse(html);
    CHECK(!doc.truncated && !doc.limit_flags && doc.link_count == 202);
    CHECK_STR(doc.links[200], "https://example.com/next");
    CHECK(doc.field_count == 1 && doc.fields[0].link >= 0 && doc.forms[0].valid);
    parse("<a href='/same'>One</a><a href='/same'>Two</a>");
    CHECK(doc.link_count == 2 && doc.items[0].link_id != doc.items[1].link_id);

    n = 0;
    for(i = 0; i < MAX_LINKS + 4; ++i)
        n += (size_t)snprintf(html + n, sizeof(html) - n,
                             "<a href='/link/%d'>Link</a><br>", i);
    parse(html);
    CHECK(doc.link_count == MAX_LINKS && (doc.limit_flags & DOCUMENT_LIMIT_LINKS));
    CHECK(strstr(page_text(), "some links are unavailable") != NULL);

    n = 0;
    for(i = 0; i < MAX_ITEMS + 10; ++i)
        n += (size_t)snprintf(html + n, sizeof(html) - n, "<p>item</p>");
    parse(html);
    CHECK(doc.truncated && (doc.limit_flags & DOCUMENT_LIMIT_LAYOUT));
    CHECK(doc.item_count <= MAX_ITEMS && strstr(page_text(), "layout limit reached"));
    document_mark_shortened(&doc, "[Response shortened: download size limit reached]");
    CHECK(strstr(page_text(), "download size limit reached") != NULL);
    for(i = 0; i < doc.item_count; ++i)
        CHECK(doc.items[i].x + doc.items[i].width <= PAGE_MARGIN + PAGE_WIDTH);

    n = 0;
    for(i = 0; i < MAX_ANCHORS + 2; ++i)
        n += (size_t)snprintf(html + n, sizeof(html) - n, "<p id=a%d>Anchor</p>", i);
    parse(html);
    CHECK(doc.anchor_count == MAX_ANCHORS && (doc.limit_flags & DOCUMENT_LIMIT_ANCHORS));
    CHECK(strstr(page_text(), "section targets") != NULL);

    n = (size_t)snprintf(html, sizeof(html), "<a href='#late-note'>Note</a>");
    for(i = 0; i < MAX_ANCHORS + 2; ++i)
        n += (size_t)snprintf(html + n, sizeof(html) - n, "<p id=incidental%d>Text</p>", i);
    snprintf(html + n, sizeof(html) - n,
             "<p id=late-note>Footnote</p><h2 id=late-heading>Section</h2>");
    parse(html);
    CHECK(document_anchor_y(&doc, "late-note") > 0);
    CHECK(document_anchor_y(&doc, "late-heading") > document_anchor_y(&doc, "late-note"));
    CHECK(!doc.truncated && doc.anchor_count == MAX_ANCHORS);
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
    test_document_generations();
    test_entities();
    test_utf8();
    test_charset();
    test_plain_text();
    test_line_endings();
    test_long_text();
    test_attributes();
    test_urls();
    test_structure();
    test_reader_and_hidden();
    test_anchors();
    test_reader_image_placeholders();
    test_inline_wrapping();
    test_capacity_and_notices();
    test_checkbox_and_radio();
    test_buttons();
    test_textarea();
    test_select();
    test_input_types();
    test_random_documents();
    document_free(&doc);
}
