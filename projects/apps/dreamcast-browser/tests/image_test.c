/* Exercise the real asset loop with deterministic network/decoder failures.
   No KOS, network access, or third-party host decoder installation required. */
#include "browser.h"
#include "test.h"

#include <stdlib.h>

int test_checks, test_failures;
static int fail_pixel_allocation;
static void *image_test_malloc(size_t size) {
    return fail_pixel_allocation ? NULL : malloc(size);
}
static int image_test_log(const char *format, ...) {
    (void)format;
    return 0;
}
#define malloc image_test_malloc
#define printf image_test_log
#include "../images.c"
#undef printf
#undef malloc

typedef struct {
    int failure, cancel, out_of_memory, truncated;
    long status;
    size_t bytes;
    int width, height, info_ok, decode_ok;
    unsigned elapsed;
    const char *reason, *content_type;
} response_t;

static browser_document_t doc;
static response_t responses[MAX_IMAGES];
static int fetches, infos, decodes, decoded_buffers, reflows;
static unsigned last_timeout;
static size_t last_limit;
static uint64_t now;
static const char *failure_reason;

uint64_t timer_ms_gettime64(void) { return now; }
void document_touch(browser_document_t *document) { document->generation++; }
void document_reflow(browser_document_t *document) { (void)document; reflows++; }

int network_fetch_image(const char *url, size_t limit, unsigned timeout_ms,
                        fetch_result_t *result) {
    const char *number = strstr(url, "image-");
    int slot = number ? atoi(number + 6) : 0;
    response_t *response = &responses[slot];
    fetches++;
    last_limit = limit;
    last_timeout = timeout_ms;
    CHECK(limit <= MAX_IMAGE_BYTES);
    CHECK(timeout_ms > 0 && timeout_ms <= IMAGE_TIMEOUT_MS);
    now += response->elapsed;
    memset(result, 0, sizeof(*result));
    result->size = response->bytes;
    result->status = response->status;
    result->cancelled = response->cancel;
    result->out_of_memory = response->out_of_memory;
    result->truncated = response->truncated;
    if(response->content_type)
        snprintf(result->content_type, sizeof(result->content_type), "%s",
                 response->content_type);
    if(response->failure) {
        snprintf(result->error, sizeof(result->error), "simulated HTTP 405/error");
        return -1;
    }
    result->data = malloc(response->bytes ? response->bytes : 1);
    result->data[0] = (unsigned char)slot;
    return 0;
}

void fetch_result_free(fetch_result_t *result) {
    free(result->data);
    memset(result, 0, sizeof(*result));
}

int stbi_info_from_memory(const unsigned char *data, int length,
                          int *width, int *height, int *channels) {
    response_t *response = &responses[data[0]];
    (void)length;
    infos++;
    *width = response->width;
    *height = response->height;
    *channels = 3;
    failure_reason = response->reason;
    return response->info_ok;
}

unsigned char *stbi_load_from_memory(const unsigned char *data, int length,
                                     int *width, int *height, int *channels,
                                     int requested_channels) {
    response_t *response = &responses[data[0]];
    unsigned char *pixels;
    size_t i, bytes = (size_t)response->width * response->height * 3;
    (void)length;
    decodes++;
    *width = response->width;
    *height = response->height;
    *channels = 3;
    CHECK(requested_channels == 3);
    failure_reason = response->reason;
    if(!response->decode_ok) return NULL;
    pixels = malloc(bytes);
    for(i = 0; i < bytes; i += 3) {
        pixels[i] = 255;
        pixels[i + 1] = 128;
        pixels[i + 2] = 0;
    }
    decoded_buffers++;
    return pixels;
}

void stbi_image_free(void *pixels) {
    if(pixels) decoded_buffers--;
    free(pixels);
}
const char *stbi_failure_reason(void) { return failure_reason; }

static void setup(int count) {
    int i;
    for(i = 0; i < doc.image_count; ++i) free(doc.images[i].pixels);
    memset(&doc, 0, sizeof(doc));
    memset(responses, 0, sizeof(responses));
    doc.image_count = count;
    for(i = 0; i < count; ++i) {
        snprintf(doc.images[i].url, sizeof(doc.images[i].url),
                 "https://test.invalid/image-%d.png", i);
        responses[i] = (response_t){.status = 200, .bytes = 128,
            .width = 2, .height = 2, .info_ok = 1, .decode_ok = 1,
            .elapsed = 10, .reason = "malformed image"};
    }
    fetches = infos = decodes = decoded_buffers = reflows = 0;
    now = 100;
    fail_pixel_allocation = 0;
}

static void test_failure_isolation(void) {
    int i;
    setup(6);
    snprintf(doc.images[0].url, sizeof(doc.images[0].url),
             "https://test.invalid/logo.SVG?size=small");
    responses[1].failure = 1;
    responses[2].info_ok = 0;
    responses[3].decode_ok = 0;
    document_load_images(&doc);
    CHECK(fetches == 5);
    CHECK(infos == 4);
    CHECK(decodes == 3);
    for(i = 0; i < 4; ++i) CHECK(doc.images[i].loaded == -1);
    CHECK(doc.images[4].loaded == 1 && doc.images[5].loaded == 1);
    CHECK(doc.images[4].width == 2 && doc.images[4].height == 2);
    CHECK(doc.images[4].pixels && doc.images[4].pixels[0] == 0xfc00);
    CHECK(decoded_buffers == 0 && reflows == 1);
    document_load_images(&doc);
    CHECK(fetches == 5); /* Repeat must neither leak nor retry failed images. */
}

static void test_decode_limits(void) {
    setup(5);
    responses[0].width = 2048;
    responses[1].width = responses[1].height = 1024;
    responses[2].width = 0;
    responses[3].width = 1024;
    responses[3].height = 512; /* Exactly the decoded-pixel ceiling. */
    document_load_images(&doc);
    CHECK(fetches == 5 && decodes == 2);
    CHECK(doc.images[0].loaded == -1 && doc.images[1].loaded == -1);
    CHECK(doc.images[2].loaded == -1 && doc.images[3].loaded == 1);
    CHECK(doc.images[3].width == 480 && doc.images[3].height == 240);
    CHECK(doc.images[4].loaded == 1 && decoded_buffers == 0);

    setup(2);
    responses[0].content_type = "image/svg+xml; charset=utf-8";
    document_load_images(&doc);
    CHECK(infos == 1 && decodes == 1);
    CHECK(doc.images[0].loaded == -1 && doc.images[1].loaded == 1);
}

static void test_global_stops(void) {
    int i;
    for(i = 0; i < 4; ++i) {
        setup(3);
        if(i < 2) {
            responses[0].failure = 1;
            responses[0].cancel = i == 0;
            responses[0].out_of_memory = i == 1;
        } else {
            responses[0].decode_ok = 0;
            responses[0].info_ok = i != 3;
            responses[0].reason = "outofmem";
        }
        document_load_images(&doc);
        CHECK(fetches == 1);
        CHECK(doc.images[0].loaded == -1 && doc.images[2].loaded == -1);
        CHECK(decoded_buffers == 0);
    }
    setup(2);
    fail_pixel_allocation = 1;
    document_load_images(&doc);
    CHECK(fetches == 1 && decoded_buffers == 0);
    CHECK(doc.images[0].pixels == NULL && doc.images[1].loaded == -1);
}

static void test_invalid_responses(void) {
    setup(4);
    responses[0].status = 404;
    responses[1].truncated = 1;
    responses[2].bytes = 0;
    document_load_images(&doc);
    CHECK(fetches == 4 && infos == 1 && decodes == 1);
    CHECK(doc.images[0].loaded == -1 && doc.images[1].loaded == -1);
    CHECK(doc.images[2].loaded == -1 && doc.images[3].loaded == 1);
}

static void test_budgets(void) {
    setup(6);
    responses[0].bytes = MAX_IMAGE_BYTES;
    responses[1].bytes = MAX_IMAGE_BYTES;
    responses[2].bytes = MAX_PAGE_IMAGE_BYTES - 2 * MAX_IMAGE_BYTES;
    responses[0].failure = 1; /* Failed downloads still count. */
    document_load_images(&doc);
    CHECK(fetches == 3);
    CHECK(last_limit == MAX_PAGE_IMAGE_BYTES - 2 * MAX_IMAGE_BYTES);
    CHECK(doc.images[2].loaded == 1 && doc.images[3].loaded == -1);

    setup(3);
    responses[0].elapsed = 8000;
    responses[1].elapsed = 4000;
    document_load_images(&doc);
    CHECK(fetches == 2 && last_timeout == 4000);
    CHECK(doc.images[1].loaded == 1 && doc.images[2].loaded == -1);
}

int main(void) {
    test_failure_isolation();
    test_decode_limits();
    test_global_stops();
    test_invalid_responses();
    test_budgets();
    setup(0);
    printf("Image tests: %d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
