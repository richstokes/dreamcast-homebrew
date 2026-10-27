/* What this ELF was built from, for the results it reports. The Makefile
   rebuilds this file every time with the current commits. */
#include "kt.h"

#ifndef KT_KOS_COMMIT
#define KT_KOS_COMMIT "unknown"
#endif
#ifndef KT_KOS_PATCHES
#define KT_KOS_PATCHES "unknown"
#endif
#ifndef KT_TEST_COMMIT
#define KT_TEST_COMMIT "unknown"
#endif

const char *const kt_build_date = __DATE__ " " __TIME__;
const char *const kt_kos_commit = KT_KOS_COMMIT;
const char *const kt_kos_patches = KT_KOS_PATCHES;
const char *const kt_test_commit = KT_TEST_COMMIT;
