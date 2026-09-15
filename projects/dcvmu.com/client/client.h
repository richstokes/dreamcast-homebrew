#ifndef DCVMU_CLIENT_H
#define DCVMU_CLIENT_H
#include <stddef.h>
#include <stdint.h>
int service_net_init(void);
void service_net_shutdown(void);
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
#endif
