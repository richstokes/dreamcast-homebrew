# Art pipeline

The original PNG atlases, skyline and title art are retained as source artwork.
`tools/build_architecture.py` derives separate upper-floor, storefront and
cornice modules from them. Upper floors wrap on architectural boundaries;
storefronts repeat only horizontally. `tools/build_effect_textures.py` creates
deterministic smoke, chassis/tire-shadow, soft headlight and palm-frond RGBA textures.

`tools/build_textures.py` converts these sources to RGB565 or ARGB4444 and emits
32-byte-aligned, pre-twiddled arrays in `assets/generated/texture_assets.c`.
Roads, upper facades and foliage include mip chains with linear-light filtering
and alpha-aware downsampling. The runtime uploads the complete payload with
`pvr_txr_load`; running it through `pvr_txr_load_ex` would twiddle it twice.
`tools/test_pvr_texture.py` verifies the layout against the KOS/Flycast offsets.

Rebuild the embedded textures with:

```sh
make textures
```

The generated C and header are checked in so a normal Dreamcast build does not
need Python or Pillow. Preview PNGs are generated locally and ignored.

The car generator stores actual UV layers and hard-edge corner normals before
export, bakes 32-ray ambient occlusion against the body and wheels, and groups
faces by material. Export splits vertices at UV, normal and material seams and
enforces the renderer's 4,096-vertex limit. Keep authored changes in
`tools/build_car_blender.py`; `make model` rebuilds the source scene. Optional
`--blend` and `--preview-dir` arguments save the source and silhouette previews.
The game's paint/glass response is implemented in the PVR renderer; Blender's
material roughness settings alone do not change the shipped shading.

There is no fixed art-memory allowance. Validate the complete scene with
`make qa-benchmark`: textures, framebuffers, polygon buffers and tile bins all
share 8 MiB of VRAM. The runtime checks each allocation and logs free memory.
