#include "browser.h"

#include <curl/curl.h>
#include <dc/asic.h>
#include <kos/irq.h>
#include <kos/net.h>
#include <kos/sem.h>
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

/* Only one transfer runs at a time, on one long-lived worker that owns the
   HTTP client. Keeping a single client keeps its connections, TLS sessions,
   DNS answers and cookies, so following a link on the same site does not pay
   for another handshake. The UI thread starts a transfer, polls it once per
   frame, and keeps ownership of rendering and Maple input throughout. */
enum { JOB_IDLE, JOB_RUNNING, JOB_DONE };
#define MAX_POST_REDIRECTS 5

static struct {
    char url[MAX_URL];
    char *body;
    fetch_kind_t kind;
    size_t limit;
    unsigned timeout_ms;
    fetch_process_t process;
    void *userdata;
    fetch_result_t result;
    int code;
    atomic_uint received, total;
    atomic_int cancel, state;
} job;

static CURL *client;
static kthread_t *transfer_thread;
static semaphore_t transfer_wake;
static volatile int transfer_quit;

static int transfer_progress(void *userdata, curl_off_t download_total,
                             curl_off_t downloaded, curl_off_t upload_total,
                             curl_off_t uploaded) {
    (void)userdata;
    (void)upload_total;
    (void)uploaded;
    atomic_store_explicit(&job.received, downloaded > 0 ? (uint32_t)downloaded : 0,
                          memory_order_relaxed);
    atomic_store_explicit(&job.total, download_total > 0 ? (uint32_t)download_total : 0,
                          memory_order_relaxed);
    return atomic_load_explicit(&job.cancel, memory_order_relaxed);
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

/* A dialled connection is some fifty times slower than Ethernet. */
int network_slow_link(void) {
    return net_default_dev && !strcmp(net_default_dev->name, "ppp");
}

static int is_https(const char *url) {
    return !strncmp(url, "https://", 8);
}

/* Form data may only travel over HTTPS. The loading self-test also posts
   to the /post addresses of its plain-HTTP fixture server on the local
   network; no release build defines BROWSER_FIXTURE_BASE. */
static int post_allowed(const char *url) {
#ifdef BROWSER_FIXTURE_BASE
    if(!strncmp(url, BROWSER_FIXTURE_BASE "/post", sizeof(BROWSER_FIXTURE_BASE "/post") - 1))
        return 1;
#endif
    return is_https(url);
}

static void fail(fetch_result_t *out, const char *message) {
    snprintf(out->error, sizeof(out->error), "%s", message);
    printf("browser: request failed: %s\n", out->error);
}

/* One request on the shared client. Returns 0 with the response in out, or
   -1 with out->error set. */
static int perform(const char *url, const char *body, fetch_result_t *out,
                   uint64_t deadline, char *redirect, size_t redirect_size) {
    CURLcode code;
    receive_buffer_t buffer = {0};
    struct curl_slist *headers = NULL;
    char error[CURL_ERROR_SIZE] = {0};
    char *content_type = NULL;
    char *effective_url = NULL;
    char *next = NULL;
    int image = job.kind == FETCH_IMAGE;
    int slow = network_slow_link();
    uint64_t now = timer_ms_gettime64();
    long transfer_timeout = (long)(deadline > now ? deadline - now : 1);
    long connect_timeout = (image ? 6000L : 15000L) * (slow ? 3 : 1);

    memset(out, 0, sizeof(*out));
    redirect[0] = 0;
    buffer.limit = job.limit;
    if(!client) client = curl_easy_init();
    if(!client) {
        out->out_of_memory = 1;
        fail(out, "Could not create HTTP client");
        return -1;
    }
    /* Resetting keeps the connections, TLS sessions, DNS cache and cookies. */
    curl_easy_reset(client);
    headers = curl_slist_append(headers, image ?
        "Accept: image/png,image/jpeg,image/gif,image/bmp;q=0.9" :
        "Accept: text/html,application/xhtml+xml,text/plain;q=0.8,*/*;q=0.1");

    curl_easy_setopt(client, CURLOPT_URL, url);
    curl_easy_setopt(client, CURLOPT_COOKIEFILE, "");
    if(headers) curl_easy_setopt(client, CURLOPT_HTTPHEADER, headers);
    if(body) curl_easy_setopt(client, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(client, CURLOPT_WRITEFUNCTION, receive_data);
    curl_easy_setopt(client, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(client, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(client, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(client, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(client, CURLOPT_ERRORBUFFER, error);
    /* A form's redirect is followed by hand, so its data is never resent
       to an address the page did not name. */
    curl_easy_setopt(client, CURLOPT_FOLLOWLOCATION, body ? 0L : 1L);
    curl_easy_setopt(client, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(client, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(client, CURLOPT_REDIR_PROTOCOLS_STR,
                     is_https(url) ? "https" : "http,https");
    curl_easy_setopt(client, CURLOPT_USERAGENT, "DreamcastBrowser/0.2 (KallistiOS)");
    curl_easy_setopt(client, CURLOPT_ACCEPT_ENCODING, "gzip,deflate");
    /* Idle connections hold TLS buffers: keep only a few, and not for long. */
    curl_easy_setopt(client, CURLOPT_MAXCONNECTS, 3L);
    curl_easy_setopt(client, CURLOPT_MAXAGE_CONN, 60L);
    /* Keep bursts modest for the Dreamcast BBA. */
    curl_easy_setopt(client, CURLOPT_BUFFERSIZE, 8192L);
    curl_easy_setopt(client, CURLOPT_MAX_RECV_SPEED_LARGE,
                     (curl_off_t)(192 * 1024));
    /* Documents can be safely shortened by receive_data(). An image that
       declares its size is refused from its headers; one that does not is
       cut off by receive_data() at the same limit and then discarded. */
    if(image)
        curl_easy_setopt(client, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)job.limit);
    curl_easy_setopt(client, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout);
    curl_easy_setopt(client, CURLOPT_TIMEOUT_MS, transfer_timeout);
    curl_easy_setopt(client, CURLOPT_LOW_SPEED_LIMIT, 64L);
    curl_easy_setopt(client, CURLOPT_LOW_SPEED_TIME, slow ? 30L : 15L);
    curl_easy_setopt(client, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(client, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(client, CURLOPT_CAINFO, "/rd/cacert.pem");
    curl_easy_setopt(client, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    printf("browser: %s request (limit %lu bytes)\n", body ? "POST" : "GET",
           (unsigned long)job.limit);
    bba_transfer_active = 1;
    code = curl_easy_perform(client);
    bba_transfer_active = 0;
    curl_easy_getinfo(client, CURLINFO_RESPONSE_CODE, &out->status);
    curl_easy_getinfo(client, CURLINFO_CONTENT_TYPE, &content_type);
    curl_easy_getinfo(client, CURLINFO_EFFECTIVE_URL, &effective_url);
    curl_easy_getinfo(client, CURLINFO_REDIRECT_URL, &next);

    if(content_type)
        snprintf(out->content_type, sizeof(out->content_type), "%s", content_type);
    snprintf(out->effective_url, sizeof(out->effective_url), "%s",
             effective_url ? effective_url : url);
    if(next && strlen(next) < redirect_size) snprintf(redirect, redirect_size, "%s", next);
    /* The header list and the strings above belong to this request. */
    curl_easy_setopt(client, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(client, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(client, CURLOPT_ERRORBUFFER, NULL);
    curl_slist_free_all(headers);

    if(code != CURLE_OK && !buffer.full) {
        out->size = buffer.size; /* Charge failed image downloads to the budget. */
        out->out_of_memory = buffer.out_of_memory || code == CURLE_OUT_OF_MEMORY;
        if(code == CURLE_ABORTED_BY_CALLBACK) {
            out->cancelled = 1;
            snprintf(out->error, sizeof(out->error), "Canceled");
        } else if(code == CURLE_FILESIZE_EXCEEDED)
            snprintf(out->error, sizeof(out->error),
                     "Response exceeds the %lu KiB safety limit",
                     (unsigned long)(job.limit / 1024));
        else
            snprintf(out->error, sizeof(out->error), "%s",
                     error[0] ? error : curl_easy_strerror(code));
        printf("browser: request failed: %s\n", out->error);
        free(buffer.data);
        return -1;
    }

    out->data = buffer.data;
    out->size = buffer.size;
    out->truncated = buffer.full;
    printf("browser: HTTP %ld, %lu bytes%s, type=%s\n", out->status,
           (unsigned long)out->size, out->truncated ? " (truncated)" : "",
           out->content_type[0] ? out->content_type : "unknown");
    return 0;
}

/* Runs the job, following a form's redirects the way browsers do: 301, 302
   and 303 are fetched with GET and no form data; 307 and 308 repeat the
   submission, which is allowed only on the site the form was sent to. */
static int run_job(void) {
    fetch_result_t *out = &job.result;
    uint64_t deadline = timer_ms_gettime64() + job.timeout_ms;
    char url[MAX_URL], redirect[MAX_URL];
    const char *body = job.body;
    int hops = 0;

    snprintf(url, sizeof(url), "%s", job.url);
    for(;;) {
        int repeat;
        if(perform(url, body, out, deadline, redirect, sizeof(redirect)) < 0) return -1;
        if(!body || out->status < 300 || out->status >= 400 || out->status == 304 ||
           !redirect[0])
            break;
        repeat = out->status == 307 || out->status == 308;
        fetch_result_free(out);
        if(++hops > MAX_POST_REDIRECTS) {
            fail(out, "The form was redirected too many times");
            return -1;
        }
        if(!post_allowed(redirect) && (repeat || is_https(url))) {
            fail(out, "The form was redirected to an insecure address");
            return -1;
        }
        if(repeat && !network_same_origin(url, redirect)) {
            fail(out, "The form was redirected to resend its data to another site");
            return -1;
        }
        printf("browser: form redirect %d followed with %s\n", hops,
               repeat ? "POST" : "GET");
        if(!repeat) body = NULL;
        snprintf(url, sizeof(url), "%s", redirect);
    }
    if(job.process && !atomic_load_explicit(&job.cancel, memory_order_relaxed))
        job.process(out, job.userdata);
    return 0;
}

static void *transfer_worker(void *unused) {
    (void)unused;
    for(;;) {
        sem_wait(&transfer_wake);
        if(transfer_quit) break;
        if(atomic_load_explicit(&job.state, memory_order_acquire) != JOB_RUNNING)
            continue;
        job.code = run_job();
        atomic_store_explicit(&job.state, JOB_DONE, memory_order_release);
    }
    if(client) {
        curl_easy_cleanup(client);
        client = NULL;
    }
    return NULL;
}

static void release_body(void) {
    if(!job.body) return;
    /* Form data may hold a password: leave no copy behind. */
    memset(job.body, 0, strlen(job.body));
    free(job.body);
    job.body = NULL;
}

int network_init(void) {
    kthread_attr_t attr = { .stack_size = 64 * 1024, .label = "browser HTTP" };
    CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if(code != CURLE_OK) {
        printf("browser: curl_global_init failed: %s\n", curl_easy_strerror(code));
        return -1;
    }
    sem_init(&transfer_wake, 0);
    transfer_quit = 0;
    atomic_store(&job.state, JOB_IDLE);
    transfer_thread = thd_create_ex(&attr, transfer_worker, NULL);
    if(!transfer_thread) {
        printf("browser: could not start the HTTP worker\n");
        sem_destroy(&transfer_wake);
        curl_global_cleanup();
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
            network_shutdown();
            return -1;
        }
        printf("browser: BBA receive worker uses polling (Flycast IRQ safety)\n");
    }
    return 0;
}

void network_shutdown(void) {
    if(transfer_thread) {
        atomic_store_explicit(&job.cancel, 1, memory_order_relaxed);
        transfer_quit = 1;
        sem_signal(&transfer_wake);
        thd_join(transfer_thread, NULL);
        transfer_thread = NULL;
        sem_destroy(&transfer_wake);
        release_body();
        fetch_result_free(&job.result);
        atomic_store(&job.state, JOB_IDLE);
        curl_global_cleanup();
    }
    if(bba_poll_thread) {
        bba_poll_running = 0;
        thd_join(bba_poll_thread, NULL);
        bba_poll_thread = NULL;
    }
    disable_bba_irq();
}

void fetch_result_free(fetch_result_t *result) {
    if(!result) return;
    free(result->data);
    memset(result, 0, sizeof(*result));
}

int network_active(void) {
    return atomic_load_explicit(&job.state, memory_order_acquire) != JOB_IDLE;
}

int network_start(const fetch_request_t *request, char *error, size_t error_size) {
    if(!transfer_thread) {
        snprintf(error, error_size, "The network is not connected");
        return -1;
    }
    if(network_active()) {
        snprintf(error, error_size, "Another transfer is still running");
        return -1;
    }
    if(strlen(request->url) >= sizeof(job.url)) {
        snprintf(error, error_size, "That address is too long");
        return -1;
    }
    if(request->body && !post_allowed(request->url)) {
        snprintf(error, error_size, "Forms require HTTPS");
        return -1;
    }
    /* The caller's copy may be edited or cleared while this one is sent. */
    job.body = request->body ? strdup(request->body) : NULL;
    if(request->body && !job.body) {
        snprintf(error, error_size, "Not enough memory for form data");
        return -1;
    }
    snprintf(job.url, sizeof(job.url), "%s", request->url);
    job.kind = request->kind;
    job.limit = request->limit;
    job.timeout_ms = request->timeout_ms ? request->timeout_ms : 1;
    job.process = request->process;
    job.userdata = request->userdata;
    job.code = -1;
    memset(&job.result, 0, sizeof(job.result));
    atomic_store_explicit(&job.received, 0, memory_order_relaxed);
    atomic_store_explicit(&job.total, 0, memory_order_relaxed);
    atomic_store_explicit(&job.cancel, 0, memory_order_relaxed);
    atomic_store_explicit(&job.state, JOB_RUNNING, memory_order_release);
    sem_signal(&transfer_wake);
    return 0;
}

void network_cancel(void) {
    if(network_active())
        atomic_store_explicit(&job.cancel, 1, memory_order_relaxed);
}

void network_progress(uint32_t *received, uint32_t *total) {
    *received = atomic_load_explicit(&job.received, memory_order_relaxed);
    *total = atomic_load_explicit(&job.total, memory_order_relaxed);
}

/* Returns 0 while the transfer runs. Once it has finished, hands its result
   to the caller, stores the transfer's own return code and returns 1. A
   transfer that finished in the same moment it was canceled is reported as
   canceled: whatever asked for it no longer wants the answer. */
int network_poll(fetch_result_t *out, int *code) {
    if(atomic_load_explicit(&job.state, memory_order_acquire) != JOB_DONE) return 0;
    release_body();
    *out = job.result;
    *code = job.code;
    memset(&job.result, 0, sizeof(job.result));
    if(atomic_load_explicit(&job.cancel, memory_order_relaxed) && !out->cancelled) {
        fetch_result_free(out);
        out->cancelled = 1;
        snprintf(out->error, sizeof(out->error), "Canceled");
        *code = -1;
    }
    atomic_store_explicit(&job.state, JOB_IDLE, memory_order_release);
    return 1;
}

/* For a transfer that has outlived its own timeout: says where the worker
   is waiting, and lists every thread, on the serial console. */
void network_report_stall(void) {
    if(!transfer_thread) return;
    printf("browser: HTTP worker state %d, waiting on \"%s\", pc %lx, received %u, "
           "cancel %d\n", transfer_thread->state,
           transfer_thread->wait_msg ? transfer_thread->wait_msg : "nothing",
           (unsigned long)transfer_thread->context.pc,
           (unsigned)atomic_load(&job.received), atomic_load(&job.cancel));
    thd_pslist(printf);
    thd_pslist_queue(printf);
}

unsigned network_page_timeout(void) {
    return network_slow_link() ? 240000 : 45000;
}

/* Waits for a transfer without drawing or reading input. Self-tests use
   this; the browser itself polls from its frame loop. */
int network_fetch_wait(const char *url, const char *body, fetch_kind_t kind,
                       size_t limit, fetch_result_t *out) {
    fetch_request_t request = { .url = url, .body = body, .kind = kind,
                                .limit = limit,
                                .timeout_ms = network_page_timeout() };
    int code = -1;
    memset(out, 0, sizeof(*out));
    if(network_start(&request, out->error, sizeof(out->error)) < 0) return -1;
    while(!network_poll(out, &code)) thd_sleep(10);
    return code;
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
