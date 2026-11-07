/* Ridgeline's textures and PVR settings for the engine renderer
   (projects/common/engine/render.c reads this). */
#ifndef RL_ENGINE_ASSETS_H
#define RL_ENGINE_ASSETS_H

#include "texture_assets.h"

#define ENG_TEX_COUNT TEX_COUNT
#define ENG_TEX_ASSET_T rl_tex_asset_t
#define ENG_TEX_ASSETS rl_tex_assets
#define ENG_TEX_PAK rl_textures_pak
#define ENG_TEXFMT_ARGB4444 RL_TEXFMT_ARGB4444
#define ENG_TEX_MIP_BIAS(tex) PVR_MIPBIAS_NORMAL

/* OP, OP modifier (unused), TR, TR modifier, PT. */
#define ENG_OPB_SIZES {PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16}
/* The autumn sky's haze: the clear colour, and the fog the hills fade into
   (world.c sets the fog's range per scene). */
#define ENG_BG_COLOR 0.84f, 0.89f, 0.93f
#define ENG_PT_ALPHA_REF 0x60
#define ENG_FOG_COLOR 1.0f, 0.84f, 0.89f, 0.93f
#define ENG_FOG_LINEAR 140.0f, 760.0f
#define ENG_SHADOW_SCALE 0.7f

#endif
