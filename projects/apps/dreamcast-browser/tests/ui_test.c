#include "browser.h"
#include "test.h"

#include <stdlib.h>

static void test_osk_layout(void) {
    char seen[128] = {0};
    int shift, row, column, c;
    int missing = 0;
    osk_key_t key;

    for(shift = 0; shift < 2; ++shift)
        for(row = 0; row < OSK_ROWS - 1; ++row)
            for(column = 0; column < osk_columns(row); ++column) {
                osk_key(row, column, shift, &key);
                CHECK(key.action == OSK_CHAR && key.label[0] == key.ch && !key.label[1]);
                seen[(unsigned char)key.ch] = 1;
            }
    for(c = 33; c < 127; ++c) if(!seen[c]) missing++;
    CHECK(missing == 0);
    osk_key(OSK_ROWS - 1, 3, 0, &key);
    CHECK(key.action == OSK_TEXT && !strcmp(key.text, ".com"));
    osk_key(OSK_ROWS - 1, 5, 1, &key);
    CHECK(key.action == OSK_DONE);
}

static void test_osk_navigation(void) {
    int row = 0, column = 0;
    osk_move(&row, &column, 0, -1);
    CHECK(row == 0 && column == 11);
    osk_move(&row, &column, -1, 0);
    CHECK(row == OSK_ROWS - 1 && column == 5);
    osk_move(&row, &column, 1, 0);
    CHECK(row == 0 && column == 11);
    row = 3;
    column = 6;
    osk_move(&row, &column, 1, 0);
    CHECK(row == 4 && column == 3);
    osk_move(&row, &column, 0, 3);
    CHECK(column == 0);
}

static void test_osk_geometry(void) {
    int row, column, hit_row, hit_column;
    for(row = 0; row < OSK_ROWS; ++row)
        for(column = 0; column < osk_columns(row); ++column) {
            int x, y, w, h;
            osk_key_rect(row, column, &x, &y, &w, &h);
            CHECK(x >= 0 && x + w <= SCREEN_W && y >= OSK_TOP && y + h <= SCREEN_H);
            CHECK(osk_hit(x + w / 2, y + h / 2, &hit_row, &hit_column) &&
                  hit_row == row && hit_column == column);
        }
    CHECK(!osk_hit(10, 10, &hit_row, &hit_column));
}

static void test_address(void) {
    static const char search[] = "https://search.test/?q=";
    char url[MAX_URL];

    CHECK(!address_is_search("example.com"));
    CHECK(!address_is_search("  news.ycombinator.com/item?id=1 "));
    CHECK(!address_is_search("localhost:8080/x"));
    CHECK(!address_is_search("192.168.1.10"));
    CHECK(!address_is_search("http://intranet"));
    CHECK(!address_is_search("about:bookmarks"));
    CHECK(address_is_search("dreamcast homebrew"));
    CHECK(address_is_search("dreamcast"));
    CHECK(address_is_search("3.14"));
    CHECK(address_is_search("?example.com"));
    CHECK(address_is_search("example.com:abc"));

    CHECK(!address_resolve("example.com/a b", search, url, sizeof(url)) &&
          !strcmp(url, "https://search.test/?q=example.com%2Fa+b"));
    CHECK(!address_resolve(" example.com ", search, url, sizeof(url)));
    CHECK_STR(url, "https://example.com");
    CHECK(!address_resolve("http://x.org/", search, url, sizeof(url)));
    CHECK_STR(url, "http://x.org/");
    CHECK(!address_resolve("C++ & caf\xe9?", search, url, sizeof(url)));
    CHECK_STR(url, "https://search.test/?q=C%2B%2B+%26+caf%C3%A9%3F");
    CHECK(!address_resolve("? example.com", search, url, sizeof(url)));
    CHECK_STR(url, "https://search.test/?q=example.com");
    CHECK(address_resolve("   ", search, url, sizeof(url)) < 0);
    CHECK(address_resolve("?", search, url, sizeof(url)) < 0);
    CHECK(address_resolve("a very long query", search, url, 30) < 0);
}

static void test_bookmark_list(void) {
    static bookmark_list_t list;
    char url[64];
    int i;

    memset(&list, 0, sizeof(list));
    CHECK(bookmarks_add(&list, "https://a.test/", "A\ttab\nline") == 0);
    CHECK_STR(list.items[0].title, "A tab line");
    CHECK(bookmarks_add(&list, "https://a.test/", "dup") == 0 && list.count == 1);
    CHECK(bookmarks_add(&list, "about:bookmarks", "x") == -2);
    CHECK(bookmarks_add(&list, "https://b.test/", "") == 1);
    CHECK_STR(list.items[1].title, "https://b.test/");
    for(i = 2; i < MAX_BOOKMARKS; ++i) {
        snprintf(url, sizeof(url), "https://%d.test/", i);
        CHECK(bookmarks_add(&list, url, url) == i);
    }
    CHECK(bookmarks_add(&list, "https://full.test/", "x") == -1);
    CHECK(!bookmarks_remove(&list, 0) && list.count == MAX_BOOKMARKS - 1);
    CHECK_STR(list.items[0].url, "https://b.test/");
    CHECK(bookmarks_remove(&list, 99) < 0);
    CHECK(bookmarks_find(&list, "https://5.test/") == 4);
}

static void test_bookmark_record(void) {
    static bookmark_list_t list, loaded;
    static unsigned char record[BOOKMARKS_MAX_BYTES];
    size_t size;
    int i;

    memset(&list, 0, sizeof(list));
    bookmarks_add(&list, "https://example.com/", "Caf\xe9 & <home>");
    bookmarks_add(&list, "http://old.test/page?a=1&b=2", "Old");
    size = bookmarks_serialize(&list, record, sizeof(record));
    CHECK(size > BOOKMARKS_HEADER_BYTES);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(!bookmarks_deserialize(&loaded, record, size));
    CHECK(loaded.count == 2);
    CHECK_STR(loaded.items[0].title, "Caf\xe9 & <home>");
    CHECK_STR(loaded.items[1].url, "http://old.test/page?a=1&b=2");

    record[BOOKMARKS_HEADER_BYTES + 3] ^= 1;
    CHECK(bookmarks_deserialize(&loaded, record, size) < 0);
    CHECK(loaded.count == 2);
    record[BOOKMARKS_HEADER_BYTES + 3] ^= 1;
    CHECK(bookmarks_deserialize(&loaded, record, size - 1) < 0);
    CHECK(bookmarks_deserialize(&loaded, record, 4) < 0);

    /* The largest possible list fits the declared record size. */
    memset(&list, 0, sizeof(list));
    for(i = 0; i < MAX_BOOKMARKS; ++i) {
        bookmark_t *item = &list.items[list.count++];
        memset(item->url, 'u', MAX_URL - 1);
        memcpy(item->url, "https://", 8);
        item->url[MAX_URL - 1] = 0;
        item->url[MAX_URL - 2] = (char)('a' + i);
        memset(item->title, 't', BOOKMARK_TITLE - 1);
        item->title[BOOKMARK_TITLE - 1] = 0;
    }
    size = bookmarks_serialize(&list, record, sizeof(record));
    CHECK(size == BOOKMARKS_MAX_BYTES - 1);
    CHECK(!bookmarks_deserialize(&loaded, record, size) && loaded.count == MAX_BOOKMARKS);
    CHECK(!bookmarks_serialize(&list, record, sizeof(record) - 1));
}

static void test_bookmark_page(void) {
    static bookmark_list_t list;
    static browser_document_t page;
    static char html[16384];
    int i;
    int remove_links = 0;
    int add_links = 0;

    memset(&list, 0, sizeof(list));
    bookmarks_add(&list, "https://example.com/?a=1&b=\"2\"", "Caf\xe9 <b>&</b>");
    bookmarks_page_html(&list, "https://new.test/", "New page", "https://home.test/",
                        "Saved to VMU A1.", html, sizeof(html));
    document_init(&page, "about:bookmarks");
    document_parse_html(&page, html, strlen(html), "text/html; charset=windows-1252");
    CHECK_STR(page.title, "Bookmarks");
    for(i = 0; i < page.link_count; ++i) {
        if(!strcmp(page.links[i], "about:bookmark-add")) add_links++;
        if(!strncmp(page.links[i], "about:bookmark-remove?", 22)) remove_links++;
    }
    CHECK(add_links == 1 && remove_links == 1);
    CHECK(page.link_count >= 4);
    CHECK_STR(page.links[1], "https://example.com/?a=1&b=%222%22");
    for(i = 0; i < page.item_count; ++i)
        if(strstr(page.items[i].text, "Caf\xe9 <b>&</b>")) break;
    CHECK(i < page.item_count);

    bookmarks_page_html(&list, "https://example.com/?a=1&b=\"2\"", "Dup",
                        "https://home.test/", "", html, sizeof(html));
    CHECK(strstr(html, "Already bookmarked") != NULL);
    CHECK(strstr(html, "about:bookmark-add") == NULL);
    CHECK(bookmarks_page_html(&list, NULL, NULL, "https://home.test/", "", html, 40) == 39);
}

void ui_tests(void) {
    test_osk_layout();
    test_osk_navigation();
    test_osk_geometry();
    test_address();
    test_bookmark_list();
    test_bookmark_record();
    test_bookmark_page();
}
