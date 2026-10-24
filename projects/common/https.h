/* HTTPS through the kos-ports curl and Mbed TLS: TLS 1.2 or later, the
   peer's chain and host name checked against a CA bundle, and every
   certificate's dates checked against the console clock. The kos-ports
   Mbed TLS is built without MBEDTLS_HAVE_TIME_DATE, so without this check
   expired and not-yet-valid certificates are accepted.

   The Dreamcast's clock holds whatever local time was set in the BIOS and
   KOS reads it as UTC, so the dates get 14 hours' leeway either side (the
   widest time zone). A clock earlier than the year this was built in is
   taken as unset and fails every certificate: it would accept certificates
   long expired.

   The CA bundle is projects/common/cacert.pem (curl's export of Mozilla's
   roots, refreshed by update-cacert.sh); common.mk copies it into a
   project's romdisk. Build with common.mk and link -lcurl -lmbedtls
   -lmbedx509 -lmbedcrypto. */
#ifndef DC_HTTPS_H
#define DC_HTTPS_H

#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set curl's TLS options, after any curl_easy_reset. ca_file is the CA
   bundle's path, such as "/rd/cacert.pem". Returns CURLE_OK, or
   CURLE_NOT_BUILT_IN when curl was not built with Mbed TLS: don't make the
   request then, as nothing would check the dates. */
CURLcode https_setup(CURL *curl, const char *ca_file);

/* Nonzero when the console clock looks set. When it does not, a failed
   certificate check (CURLE_PEER_FAILED_VERIFICATION) is the clock's
   fault, and setting it in the BIOS is the fix to suggest. */
int https_clock_ok(void);

#ifdef __cplusplus
}
#endif

#endif
