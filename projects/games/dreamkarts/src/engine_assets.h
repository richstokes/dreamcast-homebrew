/* DreamKarts' textures and PVR settings for the engine renderer
   (projects/common/engine/render.c reads this). */
#ifndef DK_ENGINE_ASSETS_H
#define DK_ENGINE_ASSETS_H

#include "texture_assets.h"
#include "track.h"

#define ENG_TEX_COUNT TEX_COUNT
#define ENG_TEX_ASSET_T dk_tex_asset_t
#define ENG_TEX_ASSETS dk_tex_assets
#define ENG_TEX_PAK dk_textures_pak
#define ENG_TEXFMT_ARGB4444 DK_TEXFMT_ARGB4444
/* Each track's own textures share the VRAM pool (track_tex.c). */
#define ENG_TEX_SETS track_textures
#define ENG_TEX_SET_COUNT TRACK_COUNT
#define ENG_TEX_SET_NAME "track"

/* The opaque modifier list holds the kart shadow volumes (r_modvol). */
#define ENG_OPB_SIZES {PVR_BINSIZE_16, PVR_BINSIZE_8, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16}
#define ENG_BG_COLOR 0.55f, 0.82f, 0.98f
#define ENG_PT_ALPHA_REF 0x60
#define ENG_FOG_COLOR 1.0f, 0.74f, 0.89f, 0.98f
#define ENG_FOG_LINEAR 160.0f, 820.0f
/* Inside a kart's shadow volume, the ground keeps this much of its colour. */
#ifndef DK_SHADOW_SCALE
#define DK_SHADOW_SCALE 0.75f
#endif
#define ENG_SHADOW_SCALE DK_SHADOW_SCALE

/* Mipmap D-adjust for the opaque (RGB565) mipmapped textures:
   DK_MIP_ROAD_BIAS for the surfaces driven on, DK_MIP_BIAS for everything
   beside them. The PVR picks a pixel's mip level from the long side of its
   texel footprint, and the chase camera sees the road at about 9 degrees,
   so at the hardware's own pick (1.0) the road's grain, edge lines and
   curbs go soft 10-20 m out. Flycast hides this when its anisotropic
   filtering is on (rend.AnisotropicFiltering).

   Measured on the console from fixed chase views (2026-10-26): 0.5 on
   every opaque texture picks one level sharper (on the road each level
   then reaches ~1.4x as far) for 2-7% more PVR time; the same on the
   ARGB4444 textures (foliage, sprites, effects) sharpens nothing that
   shows and more than doubles the extra time, so they stay at 1.0. 0.25
   aliases the boardwalk's planks and the supersample bit doubles the PVR
   time. The field only steps in quarters. Sharper means more shimmer, and
   road 0.5 with the sides at 1.0 was the best balance by eye in play on
   the console. The asphalt's square texels (world_int.h ROAD_V_M) and the
   Lanczos mips (tools/pvr_texture.py) help at no PVR cost. A compromise we
   may revisit, e.g. sides at 0.75 or a fractional bias baked into the mips. */
#ifndef DK_MIP_BIAS
#define DK_MIP_BIAS PVR_MIPBIAS_1_00
#endif
#ifndef DK_MIP_ROAD_BIAS
#define DK_MIP_ROAD_BIAS PVR_MIPBIAS_0_50
#endif
static int tex_mip_bias(int tex) {
    if(dk_tex_assets[tex].fmt == DK_TEXFMT_ARGB4444) return PVR_MIPBIAS_NORMAL;
    switch(tex) {
        case TEX_ROAD: case TEX_TKROAD: case TEX_CANYONROAD: case TEX_AVROAD: case TEX_BOARDWALK: case TEX_DASH:
        case TEX_CHECKER:
            return DK_MIP_ROAD_BIAS;
        default:
            return tex >= TEX_SKYROAD_0 && tex <= TEX_SKYROAD_15 ? DK_MIP_ROAD_BIAS : DK_MIP_BIAS;
    }
}
#define ENG_TEX_MIP_BIAS(tex) tex_mip_bias(tex)

#endif
