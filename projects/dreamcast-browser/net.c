#include "browser.h"

#include <curl/curl.h>
#include <dc/asic.h>
#include <kos/irq.h>
#include <kos/net.h>
#include <kos/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t limit;
    int full;
} receive_buffer_t;

static network_progress_callback_t progress_callback;
static void *progress_userdata;
static int gate_bba_irq;
static volatile int bba_poll_running;
static volatile int bba_transfer_active;
static kthread_t *bba_poll_thread;

/* Flycast's BBA can re-assert IRQ9 while KOS is still dispatching the first
   event, which KOS correctly reports as a double fault. Keep that event gated
   and drain the adapter from a normal KOS worker instead. This also works on
   real BBA hardware and leaves unrelated ASIC events alone. */
static void disable_bba_irq(void) {
    uint32_t old_irq;

    if(!gate_bba_irq) return;
    old_irq = irq_disable();
    asic_evt_disable(ASIC_EVT_EXP_PCI, ASIC_IRQ_DEFAULT);
    irq_restore(old_irq);
}

static void poll_bba_once(void) {
    if(gate_bba_irq && net_default_dev && net_default_dev->if_rx_poll)
        net_default_dev->if_rx_poll(net_default_dev);
}

static void *bba_poll_worker(void *unused) {
    (void)unused;
    while(bba_poll_running) {
        poll_bba_once();
        thd_sleep(bba_transfer_active ? 4 : 16);
    }
    return NULL;
}

static int transfer_progress(void *userdata, curl_off_t download_total,
                             curl_off_t downloaded, curl_off_t upload_total,
                             curl_off_t uploaded) {
    (void)userdata;
    (void)upload_total;
    (void)uploaded;
    if(!progress_callback) return 0;
    return progress_callback(downloaded > 0 ? (uint64_t)downloaded : 0,
                             download_total > 0 ? (uint64_t)download_total : 0,
                             progress_userdata);
}

void network_set_progress_callback(network_progress_callback_t callback,
                                   void *userdata) {
    progress_callback = callback;
    progress_userdata = userdata;
}

static size_t receive_data(char *ptr, size_t size, size_t count, void *userdata) {
    receive_buffer_t *buffer = userdata;
    size_t bytes = size * count;
    size_t available;
    unsigned char *grown;

    if(bytes == 0) return 0;
    if(buffer->size >= buffer->limit) {
        buffer->full = 1;
        return 0;
    }

    available = buffer->limit - buffer->size;
    if(bytes > available) {
        bytes = available;
        buffer->full = 1;
    }

    if(buffer->size + bytes + 1 > buffer->capacity) {
        size_t next = buffer->capacity ? buffer->capacity * 2 : 16384;
        while(next < buffer->size + bytes + 1) next *= 2;
        if(next > buffer->limit + 1) next = buffer->limit + 1;
        grown = realloc(buffer->data, next);
        if(!grown) return 0;
        buffer->data = grown;
        buffer->capacity = next;
    }

    memcpy(buffer->data + buffer->size, ptr, bytes);
    buffer->size += bytes;
    buffer->data[buffer->size] = 0;
    return buffer->full ? 0 : size * count;
}

int network_init(void) {
    CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if(code != CURLE_OK) {
        printf("browser: curl_global_init failed: %s\n", curl_easy_strerror(code));
        return -1;
    }
    gate_bba_irq = net_default_dev && !strcmp(net_default_dev->name, "bba");
    if(gate_bba_irq) {
        disable_bba_irq();
        bba_poll_running = 1;
        bba_poll_thread = thd_create(0, bba_poll_worker, NULL);
        if(!bba_poll_thread) {
            bba_poll_running = 0;
            printf("browser: could not start BBA receive worker\n");
            return -1;
        }
        printf("browser: BBA receive worker uses polling (Flycast IRQ safety)\n");
    }
    return 0;
}

void network_shutdown(void) {
    if(bba_poll_thread) {
        bba_poll_running = 0;
        thd_join(bba_poll_thread, NULL);
        bba_poll_thread = NULL;
    }
    disable_bba_irq();
    curl_global_cleanup();
}

void fetch_result_free(fetch_result_t *result) {
    if(!result) return;
    free(result->data);
    memset(result, 0, sizeof(*result));
}

int network_fetch(const char *url, size_t limit, fetch_result_t *out) {
    CURL *curl;
    CURLcode code;
    receive_buffer_t buffer = {0};
    char error[CURL_ERROR_SIZE] = {0};
    char *content_type = NULL;
    char *effective_url = NULL;
    curl_off_t declared_size = -1;
    long probe_status = 0;
    long transfer_timeout = limit <= MAX_IMAGE_BYTES ? 8000L : 45000L;
    long connect_timeout = limit <= MAX_IMAGE_BYTES ? 4000L : 15000L;

    memset(out, 0, sizeof(*out));
    buffer.limit = limit;
    curl = curl_easy_init();
    if(!curl) {
        snprintf(out->error, sizeof(out->error), "Could not create HTTP client");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DreamcastBrowser/0.1 (KallistiOS)");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip,deflate");
    /* Keep bursts modest for the Dreamcast BBA and reject known-oversized
       responses from their headers before downloading their bodies. */
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                     (curl_off_t)(192 * 1024));
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)limit);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, transfer_timeout);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 64L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/rd/cacert.pem");
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    /* A BBA can receive part of a response body before libcurl applies its
       maximum-file-size check. For small assets, perform a header-only probe
       and refuse unknown or oversized bodies before a risky download starts. */
    if(limit <= MAX_IMAGE_BYTES) {
        printf("browser: HEAD %s (limit %lu bytes)\n",
               url, (unsigned long)limit);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        bba_transfer_active = 1;
        code = curl_easy_perform(curl);
        bba_transfer_active = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &probe_status);
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                          &declared_size);
        if(code != CURLE_OK || probe_status < 200 || probe_status >= 400 ||
           declared_size < 0 || declared_size > (curl_off_t)limit) {
            if(code == CURLE_ABORTED_BY_CALLBACK) {
                out->cancelled = 1;
                snprintf(out->error, sizeof(out->error), "Canceled");
            } else if(code == CURLE_FILESIZE_EXCEEDED ||
               declared_size > (curl_off_t)limit)
                snprintf(out->error, sizeof(out->error),
                         "Response exceeds the %lu KiB safety limit",
                         (unsigned long)(limit / 1024));
            else if(code != CURLE_OK)
                snprintf(out->error, sizeof(out->error), "%s",
                         error[0] ? error : curl_easy_strerror(code));
            else if(declared_size < 0)
                snprintf(out->error, sizeof(out->error),
                         "Image size was not declared; skipped safely");
            else
                snprintf(out->error, sizeof(out->error),
                         "Image probe returned HTTP %ld", probe_status);
            printf("browser: request skipped after headers: %s\n", out->error);
            curl_easy_cleanup(curl);
            return -1;
        }
        curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        error[0] = 0;
    }

    printf("browser: GET %s (limit %lu bytes)\n", url, (unsigned long)limit);
    bba_transfer_active = 1;
    code = curl_easy_perform(curl);
    bba_transfer_active = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);

    if(content_type)
        snprintf(out->content_type, sizeof(out->content_type), "%s", content_type);
    if(effective_url)
        snprintf(out->effective_url, sizeof(out->effective_url), "%s", effective_url);
    else
        snprintf(out->effective_url, sizeof(out->effective_url), "%s", url);

    if(code != CURLE_OK && !buffer.full) {
        if(code == CURLE_ABORTED_BY_CALLBACK) {
            out->cancelled = 1;
            snprintf(out->error, sizeof(out->error), "Canceled");
        } else if(code == CURLE_FILESIZE_EXCEEDED)
            snprintf(out->error, sizeof(out->error),
                     "Response exceeds the %lu KiB safety limit",
                     (unsigned long)(limit / 1024));
        else
            snprintf(out->error, sizeof(out->error), "%s",
                     error[0] ? error : curl_easy_strerror(code));
        printf("browser: request failed: %s\n", out->error);
        free(buffer.data);
        curl_easy_cleanup(curl);
        return -1;
    }

    out->data = buffer.data;
    out->size = buffer.size;
    out->truncated = buffer.full;
    printf("browser: HTTP %ld, %lu bytes%s, type=%s\n", out->status,
           (unsigned long)out->size, out->truncated ? " (truncated)" : "",
           out->content_type[0] ? out->content_type : "unknown");
    curl_easy_cleanup(curl);
    return 0;
}

int resolve_url(const char *base, const char *reference, char *out, size_t out_size) {
    CURLU *url;
    CURLUcode code;
    char *resolved = NULL;

    if(!reference || !reference[0] || reference[0] == '#') return -1;
    if(!strncmp(reference, "javascript:", 11) || !strncmp(reference, "data:", 5) ||
       !strncmp(reference, "mailto:", 7)) return -1;

    url = curl_url();
    if(!url) return -1;
    code = curl_url_set(url, CURLUPART_URL, base, 0);
    if(code == CURLUE_OK)
        code = curl_url_set(url, CURLUPART_URL, reference, 0);
    if(code == CURLUE_OK)
        code = curl_url_get(url, CURLUPART_URL, &resolved, CURLU_NO_DEFAULT_PORT);
    if(code != CURLUE_OK || !resolved) {
        curl_url_cleanup(url);
        return -1;
    }

    if(strncmp(resolved, "http://", 7) && strncmp(resolved, "https://", 8)) {
        curl_free(resolved);
        curl_url_cleanup(url);
        return -1;
    }

    snprintf(out, out_size, "%s", resolved);
    curl_free(resolved);
    curl_url_cleanup(url);
    return 0;
}
