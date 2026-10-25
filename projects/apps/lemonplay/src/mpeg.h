/* pl_mpeg (third_party/pl_mpeg), configured for KOS. LemonPlay uses only
   its MPEG-1 video decoder, which writes each picture in the PowerVR YUV
   converter's macroblock order (plm_frame_t.display). */
#ifndef LEMONPLAY_MPEG_H
#define LEMONPLAY_MPEG_H

#include <kos.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#ifdef LP_POISON
/* Test builds: fill every decoder allocation with a pattern, so output that
   depends on memory the decoder never wrote shows up as a hash change. */
static inline void *lp_poisoned(void *p, size_t sz) {
    if(p) memset(p, 0x5a, sz);
    return p;
}
#define PLM_MALLOC(sz) lp_poisoned(malloc(sz), (sz))
#else
#define PLM_MALLOC(sz) malloc(sz)
#endif
#define PLM_FREE(p) free(p)
#define PLM_REALLOC(p, sz) realloc((p), (sz))
#define PLM_MEMZERO(p, sz) memset((p), 0, (sz))
#ifdef LP_POISON
#define PLM_MEMALIGN(a, sz) lp_poisoned(memalign((a), (sz)), (sz))
#else
#define PLM_MEMALIGN(a, sz) memalign((a), (sz))
#endif

#define PLM_FILE_TYPE file_t
#define PLM_FILE_INVALID_HANDLE FILEHND_INVALID
#define PLM_FILE_OPEN(fn) fs_open((fn), O_RDONLY)
#define PLM_FILE_CLOSE(fh) fs_close((fh))
#define PLM_FILE_SEEK(fh, off, st) fs_seek((fh), (off), (st))
#define PLM_FILE_READ(fh, buf, size) fs_read((fh), (buf), (size))
#define PLM_FILE_TELL(fh) fs_tell((fh))

#include "pl_mpeg.h"

/* Make the next decoded picture write its macroblocks to display (which
   must hold mb_width * mb_height * 384 bytes, 32-byte aligned). */
void lp_video_set_output(plm_video_t *video, uint32_t *display);

#endif
