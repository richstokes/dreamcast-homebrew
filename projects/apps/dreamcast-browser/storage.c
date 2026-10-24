/* Bookmark persistence on a VMU: a packaged file (../../common/vmusave.h)
   whose payload is the record from bookmarks.c. */

#include "browser.h"
#include "storage.h"
#include "../../common/vmusave.h"

#include <dc/maple.h>
#include <stdio.h>

#define BOOKMARK_FILENAME "DCBROWSE.BMK"

static int vmu_port = -1;
static int vmu_unit = -1;
static unsigned char record[BOOKMARKS_MAX_BYTES];

void storage_describe(char *out, size_t size) {
    maple_device_t *device = vmusave_card(&vmu_port, &vmu_unit);
    if(device)
        snprintf(out, size, "Bookmarks are saved to the VMU in slot %c%d.",
                 'A' + device->port, device->unit);
    else
        snprintf(out, size, "No VMU found: bookmarks last until the browser exits.");
}

/* Loads from the first VMU holding a valid record. Returns STORAGE_OK,
   STORAGE_NO_VMU when no file exists, or STORAGE_ERROR when only invalid
   files were found. */
int storage_load_bookmarks(bookmark_list_t *list) {
    maple_device_t *device;
    int index = 0;
    int invalid = 0;

    while((device = maple_enum_type(index++, MAPLE_FUNC_MEMCARD))) {
        int received;

        if(vmu_port < 0) {
            vmu_port = device->port;
            vmu_unit = device->unit;
        }
        received = vmusave_read(device, BOOKMARK_FILENAME, record, sizeof(record));
        if(received == -1) continue;
        if(received <= 0 || bookmarks_deserialize(list, record, (size_t)received) < 0) {
            printf("browser: ignoring invalid bookmark file on VMU %c%d\n",
                   'A' + device->port, device->unit);
            invalid = 1;
            continue;
        }
        vmu_port = device->port;
        vmu_unit = device->unit;
        printf("browser: loaded %d bookmark(s) from VMU %c%d\n", list->count,
               'A' + device->port, device->unit);
        return STORAGE_OK;
    }
    return invalid ? STORAGE_ERROR : STORAGE_NO_VMU;
}

int storage_save_bookmarks(const bookmark_list_t *list) {
    maple_device_t *device = vmusave_card(&vmu_port, &vmu_unit);
    vmu_pkg_t header;
    size_t size;

    if(!device) return STORAGE_NO_VMU;
    size = bookmarks_serialize(list, record, sizeof(record));
    if(!size) return STORAGE_ERROR;

    vmusave_header(&header, "DCBROWSER", "Browser Marks", "Dreamcast Browser bookmarks");
    if(vmusave_write(device, BOOKMARK_FILENAME, &header, record, size) < 0) {
        printf("browser: bookmark write failed on VMU %c%d\n",
               'A' + device->port, device->unit);
        return STORAGE_ERROR;
    }
    printf("browser: saved %d bookmark(s) to VMU %c%d\n", list->count,
           'A' + device->port, device->unit);
    return STORAGE_OK;
}

#ifdef BROWSER_HISTORY_SELF_TEST
int storage_remove(void) {
    maple_device_t *device = vmusave_card(&vmu_port, &vmu_unit);
    if(!device) return -1;
    return vmufs_delete(device, BOOKMARK_FILENAME);
}
#endif
