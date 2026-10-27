/* PowerVR drawing for KOS_Test. Each frame collects TA data per list in RAM
   and submits the lists in order at gfx_end, so screens can draw into any
   list in any order. The 2D UI goes in the translucent list, which draws in
   submission order (autosort is off). */
#ifndef KT_GFX_H
#define KT_GFX_H

#include <dc/pvr.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREEN_W 640
#define SCREEN_H 480
#define FONT_W 8        /* small font cell: the BIOS font scaled to 2/3 */
#define FONT_H 16
#define BIG_W 12        /* the BIOS font itself */
#define BIG_H 24

/* VRAM vertex buffer per frame (double-buffered) */
#define GFX_VERTBUF (1024 * 1024)

void gfx_init(void);
void gfx_begin(void);
void gfx_end(void);

/* TA data for a list, in 32-byte units. */
void gfx_push(pvr_list_t list, const void *data, size_t size);
/* Room for size bytes in a list's buffer, filled by the caller. */
void *gfx_alloc(pvr_list_t list, size_t size);
/* A header, skipped when it is the one the list last used. */
void gfx_hdr(pvr_list_t list, const pvr_poly_hdr_t *hdr);
/* Called while list is open in gfx_end, before its buffered data (so UI
   drawn this frame lands on top), to submit straight to the TA with
   pvr_prim or pvr_dr_target. Cleared every frame. */
void gfx_direct(pvr_list_t list, void (*fn)(void *), void *arg);

static inline void gfx_vert(pvr_vertex_t *v, uint32_t flags, float x, float y,
                            float z, float u, float vv, uint32_t argb,
                            uint32_t oargb) {
    v->flags = flags;
    v->x = x;
    v->y = y;
    v->z = z;
    v->u = u;
    v->v = vv;
    v->argb = argb;
    v->oargb = oargb;
}

/* A quad as one strip: corners top-left, top-right, bottom-left,
   bottom-right. */
void gfx_quad(pvr_list_t list, const pvr_poly_hdr_t *hdr, float x0, float y0,
              float x1, float y1, float z, float u0, float v0, float u1,
              float v1, uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11);

/* ---- 2D UI (translucent list) ---- */
void gfx_rect(float x, float y, float w, float h, uint32_t argb);
void gfx_grad(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void gfx_grad4(float x, float y, float w, float h, uint32_t c00, uint32_t c10,
               uint32_t c01, uint32_t c11);
void gfx_box(float x, float y, float w, float h, float t, uint32_t argb);
void gfx_line(float x0, float y0, float x1, float y1, float t, uint32_t argb);
float gfx_text(float x, float y, uint32_t argb, const char *s);
float gfx_textf(float x, float y, uint32_t argb, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
float gfx_text_big(float x, float y, uint32_t argb, const char *s);
void gfx_text_right(float right, float y, uint32_t argb, const char *s);
void gfx_text_center(float cx, float y, uint32_t argb, const char *s, bool big);
/* Text cut to max_chars with an ellipsis. */
float gfx_text_clip(float x, float y, uint32_t argb, const char *s, int max_chars);

/* ---- Opaque list (backgrounds, test patterns) ---- */
void gfx_op_rect(float x, float y, float w, float h, float z, uint32_t argb);
void gfx_op_grad4(float x, float y, float w, float h, float z, uint32_t c00,
                  uint32_t c10, uint32_t c01, uint32_t c11);

/* Shared compiled headers */
extern pvr_poly_hdr_t gfx_hdr_op_flat;   /* opaque, gouraud, depth GEQUAL */
extern pvr_poly_hdr_t gfx_hdr_tr_flat;   /* translucent, gouraud, no depth */

/* ---- Standard screen layout ---- */
#define UI_TOP 40               /* below the header */
#define UI_BOTTOM 448           /* above the footer */
void ui_background(void);
void ui_header(const char *title, const char *right);
void ui_footer(const char *hints);
void ui_panel(float x, float y, float w, float h, const char *title);
/* A list of rows with a highlight; returns the row y spacing used. */
void ui_menu(float x, float y, float w, const char *const *items, int count,
             int selected, int first, int visible);

#endif
