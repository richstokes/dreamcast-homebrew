#define PL_MPEG_IMPLEMENTATION
#include "mpeg.h"

/* The picture being decoded is frame_current; with no B-frames it comes
   back from plm_video_decode with this display pointer. Skipped macroblocks
   are predicted into it too, so every macroblock is written. */
void lp_video_set_output(plm_video_t *video, uint32_t *display) {
    video->frame_current.display = display;
}
