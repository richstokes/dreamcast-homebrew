/* HTTPS through curl and Mbed TLS with certificate dates checked (see
   https.h). */

/* For mbedtls_ssl_config's verify callback, so the date check can run
   before curl's own. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "https.h"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LEEWAY_SECONDS (14 * 60 * 60)

/* __DATE__ is "Mmm dd yyyy". Tests can pretend it is another year. */
#ifndef HTTPS_BUILD_YEAR
#define HTTPS_BUILD_YEAR atoi(__DATE__ + 7)
#endif

typedef int (*verify_callback)(void *, mbedtls_x509_crt *, int, uint32_t *);

/* curl's mbedTLS backend gives every connection the same verify callback:
   it applies CURLOPT_SSL_VERIFYPEER/VERIFYHOST and words the failure for
   CURLOPT_ERRORBUFFER. */
static verify_callback curl_verify;

static int64_t days_from_civil(int year, int month, int day) {
    int64_t era;
    int year_of_era, day_of_year, day_of_era;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = (int)(year - era * 400);
    day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static int64_t seconds(const mbedtls_x509_time *t) {
    return days_from_civil(t->year, t->mon, t->day) * 86400 +
           t->hour * 3600 + t->min * 60 + t->sec;
}

static int clock_set(int64_t now) {
    return now + LEEWAY_SECONDS >= days_from_civil(HTTPS_BUILD_YEAR, 1, 1) * 86400;
}

int https_clock_ok(void) {
    return clock_set((int64_t)time(NULL));
}

/* Called for every certificate in the chain, the root included. */
static int check_dates(void *context, mbedtls_x509_crt *cert, int depth,
                       uint32_t *flags) {
    int64_t now = (int64_t)time(NULL);

    if(!clock_set(now)) {
        *flags |= MBEDTLS_X509_BADCERT_OTHER;
    }
    else {
        if(now + LEEWAY_SECONDS < seconds(&cert->valid_from))
            *flags |= MBEDTLS_X509_BADCERT_FUTURE;
        if(now - LEEWAY_SECONDS > seconds(&cert->valid_to))
            *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    }
    return curl_verify ? curl_verify(context, cert, depth, flags) : 0;
}

static CURLcode add_date_check(CURL *curl, void *ssl_config, void *unused) {
    mbedtls_ssl_config *config = (mbedtls_ssl_config *)ssl_config;

    (void)curl;
    (void)unused;
    if(config->f_vrfy != check_dates)
        curl_verify = config->f_vrfy;
    mbedtls_ssl_conf_verify(config, check_dates, config->p_vrfy);
    return CURLE_OK;
}

CURLcode https_setup(CURL *curl, const char *ca_file) {
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);

    /* The date check hooks Mbed TLS's verify callback. */
    if(!version->ssl_version || strncmp(version->ssl_version, "mbedTLS/", 8))
        return CURLE_NOT_BUILT_IN;
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    return curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, add_date_check);
}
