/* Bookmark persistence on a VMU, following the DCIRC profile's approach:
   a packaged VMU file whose payload is the record from bookmarks.c. */

#include "browser.h"
#include "storage.h"

#include <dc/fs_vmu.h>
#include <dc/maple.h>
#include <dc/vmu_pkg.h>
#include <errno.h>
#include <fcntl.h>
#include <kos/fs.h>
#include <stdio.h>
#include <string.h>

#define BOOKMARK_FILENAME "DCBROWSE.BMK"

static int vmu_port = -1;
static int vmu_unit = -1;
static unsigned char record[BOOKMARKS_MAX_BYTES];

static void path_for(const maple_device_t *device, char *path, size_t size) {
    snprintf(path, size, "/vmu/%c%d/%s", 'a' + device->port, device->unit,
             BOOKMARK_FILENAME);
}

static maple_device_t *save_device(void) {
    maple_device_t *device = NULL;
    if(vmu_port >= 0) {
        device = maple_enum_dev(vmu_port, vmu_unit);
        if(device && !(device->info.functions & MAPLE_FUNC_MEMCARD)) device = NULL;
    }
    if(!device) {
        device = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
        if(device) {
            vmu_port = device->port;
            vmu_unit = device->unit;
        }
    }
    return device;
}

void storage_describe(char *out, size_t size) {
    maple_device_t *device = save_device();
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
        char path[40];
        file_t file;
        ssize_t received;

        if(vmu_port < 0) {
            vmu_port = device->port;
            vmu_unit = device->unit;
        }
        path_for(device, path, sizeof(path));
        file = fs_open(path, O_RDONLY);
        if(file == FILEHND_INVALID) continue;
        received = fs_read(file, record, sizeof(record));
        fs_close(file);
        if(received <= 0 || bookmarks_deserialize(list, record, (size_t)received) < 0) {
            printf("browser: ignoring invalid bookmark file at %s\n", path);
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
    static uint8_t empty_asset;
    maple_device_t *device = save_device();
    vmu_pkg_t header;
    char path[40];
    file_t file;
    size_t size;
    ssize_t written;
    int header_result;
    int close_result;

    if(!device) return STORAGE_NO_VMU;
    size = bookmarks_serialize(list, record, sizeof(record));
    if(!size) return STORAGE_ERROR;

    memset(&header, 0, sizeof(header));
    snprintf(header.desc_short, sizeof(header.desc_short), "Browser Marks");
    snprintf(header.desc_long, sizeof(header.desc_long), "Dreamcast Browser bookmarks");
    snprintf(header.app_id, sizeof(header.app_id), "DCBROWSER");
    header.icon_cnt = 0;
    header.icon_data = &empty_asset;
    header.eyecatch_type = VMUPKG_EC_NONE;
    header.eyecatch_data = &empty_asset;
    header.data_len = (int)size;
    header.data = record;

    path_for(device, path, sizeof(path));
    file = fs_open(path, O_WRONLY | O_TRUNC);
    if(file == FILEHND_INVALID) {
        printf("browser: cannot open %s for writing: %s\n", path, strerror(errno));
        return STORAGE_ERROR;
    }
    written = fs_write(file, record, size);
    header_result = fs_vmu_set_header(file, &header);
    close_result = fs_close(file);
    if(written != (ssize_t)size || header_result < 0 || close_result < 0) {
        printf("browser: bookmark write failed at %s: %s\n", path, strerror(errno));
        return STORAGE_ERROR;
    }
    printf("browser: saved %d bookmark(s) to VMU %c%d\n", list->count,
           'A' + device->port, device->unit);
    return STORAGE_OK;
}

#ifdef BROWSER_HISTORY_SELF_TEST
int storage_remove(void) {
    maple_device_t *device = save_device();
    char path[40];
    if(!device) return -1;
    path_for(device, path, sizeof(path));
    return fs_unlink(path);
}
#endif
