/* Kaiju Lunch Break's textures and PVR settings for the engine renderer
   (projects/common/engine/render.c reads this). */
#ifndef KL_ENGINE_ASSETS_H
#define KL_ENGINE_ASSETS_H

#include "texture_assets.h"

#define ENG_TEX_COUNT TEX_COUNT
#define ENG_TEX_ASSET_T kl_tex_asset_t
#define ENG_TEX_ASSETS kl_tex_assets
#define ENG_TEX_PAK kl_textures_pak
#define ENG_TEXFMT_ARGB4444 KL_TEXFMT_ARGB4444
#define ENG_TEX_MIP_BIAS(tex) PVR_MIPBIAS_NORMAL

/* OP, OP modifier (unused), TR, TR modifier, PT. */
#define ENG_OPB_SIZES {PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16}
/* The horizon's haze: the clear colour, and what the far city fades to. */
#define ENG_BG_COLOR 0.80f, 0.91f, 0.98f
#define ENG_PT_ALPHA_REF 0x80
#define ENG_FOG_COLOR 1.0f, 0.80f, 0.91f, 0.98f
#define ENG_FOG_LINEAR 400.0f, 1250.0f
#define ENG_SHADOW_SCALE 0.7f

#endif
