/* Exercise https.c on SH-4: the kos-ports curl and Mbed TLS against the
   runner's HTTPS servers, one certificate case per port, with the console
   clock faked through time(). */
#define time test_time
#include "../https.c"
#undef time
#include <kos.h>
#include <assert.h>
#include <stdio.h>
#include "config.h"

extern time_t time(time_t *);

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

/* The console clock is local time read as UTC (Flycast's too), so it sits
   a time zone away from the servers' clock; offsets below are from UTC. */
static int64_t skew;
static int64_t offset;      /* from UTC */
static time_t fixed;        /* when set, the clock reads this instead */

time_t test_time(time_t *out) {
    time_t now = fixed ? fixed : (time_t)(time(NULL) - skew + offset);

    if(out)
        *out = now;
    return now;
}

/* Keeps the first few bytes of the body (the servers answer "ok"). */
static size_t keep(char *data, size_t size, size_t count, void *context) {
    char *body = (char *)context;
    size_t room = 15 - strlen(body);

    strncat(body, data, size * count < room ? size * count : room);
    return size * count;
}

static char reason[CURL_ERROR_SIZE];

/* One request on its own handle (or on reuse, kept for the next call).
   Returns curl's result; reason holds curl's error text. */
static CURLcode get(int port, CURL **reuse, long verify_host) {
    CURL *curl = reuse && *reuse ? *reuse : curl_easy_init();
    char url[64], body[16] = "";
    CURLcode result;

    assert(curl);
    curl_easy_reset(curl);
    assert(https_setup(curl, "/rd/test-ca.pem") == CURLE_OK);
    if(verify_host != 2)
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_host);
    snprintf(url, sizeof(url), "https://%s:%d/", HOST, port);
    reason[0] = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, reason);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, keep);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    result = curl_easy_perform(curl);
    printf("HTTPS CHECK port %d clock %+lld s%s: %d %s\n", port, (long long)offset,
           fixed ? " (fixed)" : "", (int)result, result ? reason : body);
    if(result == CURLE_OK)
        assert(!strcmp(body, "ok"));
    if(reuse)
        *reuse = curl;
    else
        curl_easy_cleanup(curl);
    return result;
}

static void refused(int port, const char *why) {
    assert(get(port, NULL, 2) == CURLE_PEER_FAILED_VERIFICATION);
    assert(!why || strstr(reason, why));
}

static time_t at(int year, int month, int day) {
    return (time_t)(days_from_civil(year, month, day) * 86400 + 12 * 3600);
}

int main(void) {
    CURL *shared = NULL;
    long connects = -1;

    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    assert(net_default_dev);
    /* BUILT_AT is UTC a moment before boot; time zones come in quarter hours. */
    skew = (int64_t)time(NULL) - BUILT_AT;
    skew = (skew >= 0 ? (skew + 450) / 900 : -((-skew + 450) / 900)) * 900;
    printf("HTTPS CHECK console clock is %+lld s from UTC\n", (long long)skew);

    /* The real clock. curl's own wording of each failure survives. */
    assert(https_clock_ok());
    assert(get(PORT_GOOD, NULL, 2) == CURLE_OK);
    refused(PORT_EXPIRED, "expired");
    refused(PORT_FUTURE, "future");
    refused(PORT_EXPIRED_INTERMEDIATE, "expired");
    refused(PORT_UNTRUSTED, NULL);
    refused(PORT_WRONG_HOST, NULL);
    /* VERIFYHOST 0 still works, so curl's verify callback still runs. */
    assert(get(PORT_WRONG_HOST, NULL, 0) == CURLE_OK);
    /* Within the 14 hours' leeway. */
    assert(get(PORT_FRESH, NULL, 2) == CURLE_OK);
    assert(get(PORT_RECENTLY_EXPIRED, NULL, 2) == CURLE_OK);

    /* A US console's clock in local time, hours behind UTC, still takes a
       certificate issued minutes ago; 15 hours behind does not. */
    offset = -8 * 3600;
    assert(get(PORT_FRESH, NULL, 2) == CURLE_OK);
    offset = -15 * 3600;
    refused(PORT_FRESH, "future");
    /* A month ahead, the 30-day certificate has expired. */
    offset = 31 * 86400;
    refused(PORT_GOOD, "expired");
    offset = 0;

    /* An unset clock fails every certificate, and says so. */
    fixed = at(1998, 1, 1);
    assert(!https_clock_ok());
    refused(PORT_GOOD, NULL);
    /* Even one whose dates it falls inside (valid 2020 to 2040). */
    fixed = at(HTTPS_BUILD_YEAR - 1, 6, 1);
    assert(!https_clock_ok());
    refused(PORT_LONG, NULL);
    fixed = at(HTTPS_BUILD_YEAR, 6, 1);
    assert(https_clock_ok());
    assert(get(PORT_LONG, NULL, 2) == CURLE_OK);
    fixed = 0;

    /* One handle reuses its connection, and a failure on it does not
       leave the next connection unchecked. */
    assert(get(PORT_GOOD, &shared, 2) == CURLE_OK);
    assert(get(PORT_GOOD, &shared, 2) == CURLE_OK);
    curl_easy_getinfo(shared, CURLINFO_NUM_CONNECTS, &connects);
    assert(connects == 0);
    assert(get(PORT_EXPIRED, &shared, 2) == CURLE_PEER_FAILED_VERIFICATION);
    assert(get(PORT_FUTURE, &shared, 2) == CURLE_PEER_FAILED_VERIFICATION);
    curl_easy_cleanup(shared);

    curl_global_cleanup();
    printf("HTTPS CHECKS PASSED\n");
    while(1)
        thd_sleep(1000);
}
