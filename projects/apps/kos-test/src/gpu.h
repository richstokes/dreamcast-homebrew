/* Shared by the GPU benchmarks and the GPU test pages. */
#ifndef KT_GPU_H
#define KT_GPU_H

#include <dc/pvr.h>
#include <stdbool.h>

/* 256x256 twiddled RGB565 gradient and checker, made on first use. */
pvr_ptr_t gpu_test_texture(void);

/* A lit torus (2304 triangles) submitted straight to the opaque list:
   pass torus_submit to gfx_direct. */
void torus_set(float angle, float x, float y, float dist, bool textured);
/* Screen centre of the projection (320, 240 unless rendering to a texture). */
void torus_view(float cx, float cy);
void torus_submit(void *unused);
int torus_triangles(void);

#endif
