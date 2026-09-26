#ifndef DCVMU_CLIENT_H
#define DCVMU_CLIENT_H
#include <stddef.h>
#include <stdint.h>
#define DCVMU_ICON_FILE "ICONDATA_VMS"
int service_net_init(void);
void service_net_shutdown(void);
int service_modem_init(void);
void service_modem_shutdown(void);
/* Redraw startup and poll cancellation on the UI thread. */
int client_connect_update(const char *message);
int service_login(const char *username, const char *password, char *token, size_t capacity);
int service_resume(const char *token);
int auth_load(char *username, char *token);
int auth_save(const char *username, const char *token);
int auth_forget(void);
int auth_is_save(const char *filename, const void *data, size_t size);
void service_logout(const char *token);
int service_upload(const char *token, const char *name, const char *filename,
                   const char *game, const char *notes, int private_save,
                   const void *data, size_t size, int header_offset, const char *mode, int save_id, int *revision);
int transfer_update(uint64_t done, uint64_t total);
void client_status(const char *message);
typedef struct {
    int id, revision, size, header_offset;
    char filename[13], name[65], game[81], user[25], sha256[65];
} remote_save_t;
/* owner == NULL lists my saves; public browsing requires an exact owner. */
int service_list(const char *token, const char *owner, const char *game_filter, int page,
                 remote_save_t *items, int *count, int *more);
int service_matches(const char *token, const char *filename, int page,
                    remote_save_t *items, int *count, int *more);
int service_rename(const char *token, const remote_save_t *item, const char *title);
int service_download(const char *token, const remote_save_t *item, void **data);
/* Fetch only the VMS header and first icon frame; never writes a VMU. */
int service_icon(const char *token, const remote_save_t *item, unsigned char header[640]);

/* Whole-card archives: one standard 128 KiB bank, login save excluded. */
#define ARCHIVE_IMAGE_SIZE 131072
typedef struct {
    int id, revision, files;
    long created;
    char name[65], source[25], sha256[65];
} remote_archive_t;
struct maple_device;
/* Raw copy of every block; the caller owns a ARCHIVE_IMAGE_SIZE buffer. */
int archive_read(struct maple_device *dev, unsigned char *image);
/* Delete DCVMU login saves and zero unused blocks in place; -1 if not a standard card. */
int archive_scrub(unsigned char *image, int *files);
/* Write every block, then read the card back into readback and compare (-2 on mismatch). */
int archive_write(struct maple_device *dev, const unsigned char *image, unsigned char *readback);
/* Count files on a card, excluding the DCVMU login save; -1 if unreadable. */
int archive_card_files(struct maple_device *dev);
/* UI progress hook for long block transfers. */
void archive_progress(const char *stage, int done, int total);
/* Card id (port*6+unit) holding the remembered login, or -1. */
int auth_location(void);
int service_archive_upload(const char *token, const char *name, const char *source,
                           const void *image, size_t size);
int service_archive_list(const char *token, int page, remote_archive_t *items, int *count, int *more);
int service_archive_download(const char *token, const remote_archive_t *item, unsigned char *image);
#endif
