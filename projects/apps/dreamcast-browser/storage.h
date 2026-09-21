#ifndef DREAMCAST_BROWSER_STORAGE_H
#define DREAMCAST_BROWSER_STORAGE_H

#include "browser.h"

enum {
    STORAGE_OK = 0,
    STORAGE_NO_VMU = -1,
    STORAGE_ERROR = -2
};

void storage_describe(char *out, size_t size);
int storage_load_bookmarks(bookmark_list_t *list);
int storage_save_bookmarks(const bookmark_list_t *list);

#ifdef BROWSER_HISTORY_SELF_TEST
int storage_remove(void);
#endif

#endif
