#include "browser.h"

#include <curl/curl.h>
#include <dc/asic.h>
#include <kos/irq.h>
#include <kos/net.h>
#include <kos/thread.h>
#include <arch/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
    size_t limit;
    int full;
    int out_of_memory;
} receive_buffer_t;

static network_progress_callback_t progress_callback;
static void *progress_userdata;
static int gate_bba_irq;
static CURLSH *cookie_share;
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

/* Only one transfer runs at a time. Curl and TLS live on this worker;
   the calling UI thread retains ownership of rendering and Maple input. */
typedef struct {
    const char *url, *body;
    size_t limit;
    unsigned timeout_ms;
    fetch_result_t *result;
    atomic_uint received, total;
    atomic_int cancel, done;
    int code;
} transfer_job_t;

static int transfer_progress(void *userdata, curl_off_t download_total,
                             curl_off_t downloaded, curl_off_t upload_total,
                             curl_off_t uploaded) {
    transfer_job_t *job = userdata;
    int cancel;
    (void)upload_total;
    (void)uploaded;
    atomic_store_explicit(&job->received, downloaded > 0 ? (uint32_t)downloaded : 0, memory_order_relaxed);
    atomic_store_explicit(&job->total, download_total > 0 ? (uint32_t)download_total : 0, memory_order_relaxed);
    cancel = atomic_load_explicit(&job->cancel, memory_order_relaxed);
    return cancel;
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
        if(!grown) { buffer->out_of_memory = 1; return 0; }
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
    cookie_share = curl_share_init();
    if(!cookie_share) return -1;
    curl_share_setopt(cookie_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
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
    if(cookie_share) { curl_share_cleanup(cookie_share); cookie_share=NULL; }
    curl_global_cleanup();
}

void fetch_result_free(fetch_result_t *result) {
    if(!result) return;
    free(result->data);
    memset(result, 0, sizeof(*result));
}

static int network_request_worker(const char *url, const char *body, size_t limit,
                                  fetch_result_t *out, transfer_job_t *job,
                                  uint64_t deadline) {
    CURL *curl;
    CURLcode code;
    receive_buffer_t buffer = {0};
    char error[CURL_ERROR_SIZE] = {0};
    char *content_type = NULL;
    char *effective_url = NULL;
    curl_off_t declared_size = -1;
    long probe_status = 0;
    uint64_t now = timer_ms_gettime64();
    long transfer_timeout = (long)(deadline > now ? deadline - now : 1);
    long connect_timeout = limit <= MAX_IMAGE_BYTES ? 4000L : 15000L;

    memset(out, 0, sizeof(*out));
    buffer.limit = limit;
    curl = curl_easy_init();
    if(!curl) {
        out->out_of_memory = 1;
        snprintf(out->error, sizeof(out->error), "Could not create HTTP client");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_SHARE, cookie_share);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    if(body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, body ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, !strncmp(url,"https://",8) ? "https" : "http,https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DreamcastBrowser/0.1 (KallistiOS)");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip,deflate");
    /* Keep bursts modest for the Dreamcast BBA and reject known-oversized
       responses from their headers before downloading their bodies. */
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                     (curl_off_t)(192 * 1024));
    /* Documents can be safely shortened by receive_data(). Images must be
       rejected before any oversized body reaches the constrained decoder. */
    if(limit <= MAX_IMAGE_BYTES)
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
            out->out_of_memory = code == CURLE_OUT_OF_MEMORY;
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
        /* HEAD and GET share one deadline, rather than each spending the
           entire image allowance. */
        now = timer_ms_gettime64();
        transfer_timeout = (long)(deadline > now ? deadline - now : 1);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, transfer_timeout);
    }

    printf("browser: %s request (limit %lu bytes)\n", body?"POST":"GET", (unsigned long)limit);
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
        out->size = buffer.size; /* Charge failed image downloads to the budget. */
        out->out_of_memory = buffer.out_of_memory || code == CURLE_OUT_OF_MEMORY;
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
    if(body && out->status == 303) {
        char *next=NULL;
        char target[MAX_URL];
        curl_easy_getinfo(curl,CURLINFO_REDIRECT_URL,&next);
        if(next && strlen(next)<sizeof(target) && network_same_origin(url,next)) {
            snprintf(target,sizeof(target),"%s",next);
            fetch_result_free(out);
            curl_easy_cleanup(curl);
            return network_request_worker(target,NULL,limit,out,job,deadline);
        }
    }
    curl_easy_cleanup(curl);
    return 0;
}

static void *transfer_worker(void *userdata) {
    transfer_job_t *job = userdata;
    int code = network_request_worker(job->url, job->body, job->limit,
                                     job->result, job,
                                     timer_ms_gettime64() + job->timeout_ms);
    job->code = code;
    atomic_store_explicit(&job->done, 1, memory_order_release);
    return NULL;
}

static int network_request(const char *url, const char *body, size_t limit,
                           unsigned timeout_ms, fetch_result_t *out) {
    transfer_job_t job = { .url = url, .body = body, .limit = limit,
                           .timeout_ms = timeout_ms, .result = out };
    kthread_attr_t attr = { .stack_size = 64 * 1024, .label = "browser HTTP" };
    /* The UI can submit another form while this one is in flight. Curl
       borrows POSTFIELDS, so never lend it the form editor's scratch buffer. */
    char *body_copy = body ? strdup(body) : NULL;
    if(body && !body_copy) {
        memset(out, 0, sizeof(*out));
        out->out_of_memory = 1;
        snprintf(out->error, sizeof(out->error), "Not enough memory for form data");
        return -1;
    }
    job.body = body_copy;
    kthread_t *worker = thd_create_ex(&attr, transfer_worker, &job);
    if(!worker) {
        if(body_copy) { memset(body_copy, 0, strlen(body_copy)); free(body_copy); }
        memset(out, 0, sizeof(*out));
        out->out_of_memory = 1;
        snprintf(out->error, sizeof(out->error), "Not enough memory for HTTP worker");
        return -1;
    }
#ifdef BROWSER_PROFILE
    uint64_t report_at = timer_ms_gettime64() + 10000;
#endif
    for(;;) {
#ifdef BROWSER_PROFILE
        if(timer_ms_gettime64() >= report_at) {
            printf("PROF HTTP worker state %d wait %s pc %lx received %u cancel %d\n",
                   worker->state, worker->wait_msg ? worker->wait_msg : "none",
                   (unsigned long)worker->context.pc, atomic_load(&job.received), atomic_load(&job.cancel));
            report_at = timer_ms_gettime64() + 10000;
        }
#endif
        uint32_t received = atomic_load_explicit(&job.received, memory_order_relaxed);
        uint32_t total = atomic_load_explicit(&job.total, memory_order_relaxed);
        int done = atomic_load_explicit(&job.done, memory_order_acquire);
        if(done) break;
        if(progress_callback && progress_callback(received, total, progress_userdata)) {
            atomic_store_explicit(&job.cancel, 1, memory_order_relaxed);
        }
        thd_sleep(1); /* The UI callback normally waits for the next frame. */
    }
    thd_join(worker, NULL);
    if(body_copy) { memset(body_copy, 0, strlen(body_copy)); free(body_copy); }
    /* A replacement navigation can arrive on the same frame as completion. */
    if(atomic_load_explicit(&job.cancel, memory_order_relaxed) && !out->cancelled) {
        fetch_result_free(out);
        out->cancelled = 1;
        snprintf(out->error, sizeof(out->error), "Canceled");
        return -1;
    }
    return job.code;
}

int network_fetch(const char *url, size_t limit, fetch_result_t *out) {
    return network_request(url, NULL, limit,
                           limit <= MAX_IMAGE_BYTES ? 8000 : 45000, out);
}
int network_fetch_image(const char *url, size_t limit, unsigned timeout_ms,
                        fetch_result_t *out) {
    return network_request(url, NULL, limit, timeout_ms ? timeout_ms : 1, out);
}
int network_post(const char *url, const char *body, size_t limit, fetch_result_t *out) {
    if(strncmp(url,"https://",8)) {
        memset(out,0,sizeof(*out));
        snprintf(out->error,sizeof(out->error),"Forms require HTTPS");
        return -1;
    }
    return network_request(url,body,limit,45000,out);
}
int network_same_origin(const char *a, const char *b) {
    CURLU *ua=curl_url(), *ub=curl_url();
    int same=1;
    CURLUPart parts[]={CURLUPART_SCHEME,CURLUPART_HOST,CURLUPART_PORT};
    if(!ua||!ub||curl_url_set(ua,CURLUPART_URL,a,0)||curl_url_set(ub,CURLUPART_URL,b,0)) same=0;
    for(int i=0;same && i<3;++i) {
        char *va=NULL,*vb=NULL;
        if(curl_url_get(ua,parts[i],&va,CURLU_DEFAULT_PORT)||curl_url_get(ub,parts[i],&vb,CURLU_DEFAULT_PORT)||strcasecmp(va,vb)) same=0;
        curl_free(va);curl_free(vb);
    }
    curl_url_cleanup(ua);curl_url_cleanup(ub);
    return same;
}

int resolve_url(const char *base, const char *reference, char *out, size_t out_size) {
    CURLU *url;
    CURLUcode code;
    char *resolved = NULL;

    if(!reference || !reference[0]) return -1;
    if(!strncmp(reference, "javascript:", 11) || !strncmp(reference, "data:", 5) ||
       !strncmp(reference, "mailto:", 7)) return -1;
    /* Internal pages; their actions are refused unless shown internally. */
    if(!strncmp(reference, "about:", 6)) {
        if(strlen(reference) >= out_size) return -1;
        snprintf(out, out_size, "%s", reference);
        return 0;
    }

    url = curl_url();
    if(!url) return -1;
    /* Internal pages have no web base, so only absolute links resolve. */
    code = strncmp(base, "http://", 7) && strncmp(base, "https://", 8) ?
           CURLUE_OK : curl_url_set(url, CURLUPART_URL, base, 0);
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

    if(strlen(resolved) >= out_size) {
        curl_free(resolved);
        curl_url_cleanup(url);
        return -1;
    }
    snprintf(out, out_size, "%s", resolved);
    curl_free(resolved);
    curl_url_cleanup(url);
    return 0;
}
