/* DreamKarts' settings for the shared engine (projects/common/engine). */
#ifndef DK_ENGINE_CONFIG_H
#define DK_ENGINE_CONFIG_H

#define ENG_NAME "DreamKarts"
#define ENG_QA_TAG "DK"
#define ENG_DT   (1.0f / 60.0f)   /* one simulation step a displayed frame */

/* Test builds. */
#if defined(DK_FILM) || defined(DK_CAPTURE)
#define ENG_INPUT_TAPS 1
#endif
#ifdef DK_AUTOTEST
#define ENG_INPUT_FAKE_KEYS 1
#define ENG_PROF 1
#define ENG_PROF_TAG "DK PC"
#endif
#if defined(DK_AUTOTEST) && !defined(DK_HW)
#define ENG_INPUT_IGNORE_HOST 1
#define ENG_TEX_CHECK 1
#endif
#ifdef DK_XF_CHECK
#define ENG_XF_CHECK 1
#endif
#if defined(DK_HW) && defined(DK_CAPTURE)
#define ENG_TA_SINK (dk_probe_off & PROBE_NOTA)
#endif

/* The Tile Accelerator's vertex buffer (pvr_init): a hard ceiling, past
   which the console stalls (Tsudoken Drift's split screen passed 1 MB).
   1536 until the shadow volumes' bins needed the VRAM. */
#define ENG_VTX_BUF_KIB 1472

#endif
