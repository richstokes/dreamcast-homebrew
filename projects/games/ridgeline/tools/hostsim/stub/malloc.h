#include <stdlib.h>
static inline void *memalign(size_t align, size_t n) {
    void *p;
    return posix_memalign(&p, align, n) ? NULL : p;
}
