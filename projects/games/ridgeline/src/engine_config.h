/* Ridgeline's settings for the shared engine (projects/common/engine). */
#ifndef RL_ENGINE_CONFIG_H
#define RL_ENGINE_CONFIG_H

#define ENG_NAME "Ridgeline"
#define ENG_DT   (1.0f / 60.0f)

#ifdef RL_CAPTURE
#define ENG_INPUT_TAPS 1
#endif
#ifdef RL_AUTOTEST
#define ENG_PROF 1
#define ENG_PROF_TAG "RL PC"
#endif
#if defined(RL_AUTOTEST) && !defined(RL_HW)
#define ENG_INPUT_IGNORE_HOST 1
#endif

/* The Tile Accelerator's vertex buffer: open hillsides show a lot. */
#define ENG_VTX_BUF_KIB 1280

#endif
