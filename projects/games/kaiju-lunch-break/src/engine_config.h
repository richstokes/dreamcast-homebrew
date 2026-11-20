/* Kaiju Lunch Break's settings for the shared engine (projects/common/engine). */
#ifndef KL_ENGINE_CONFIG_H
#define KL_ENGINE_CONFIG_H

#define ENG_NAME "Kaiju Lunch Break"
#define ENG_QA_TAG "KL"
#define ENG_DT   (1.0f / 60.0f)

#ifdef KL_AUTOTEST
#define ENG_INPUT_IGNORE_HOST 1
#endif

/* City blocks and the ground are big strips: send those across the
   screen's edge whole when they are clear of the near plane (render.h). */
#define ENG_CROSS_EMIT 1

/* The Tile Accelerator's vertex buffer: a skyline is a lot of walls. */
#define ENG_VTX_BUF_KIB 1536

#endif
