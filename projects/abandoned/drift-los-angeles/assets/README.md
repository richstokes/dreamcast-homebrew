# Art pipeline

`assets/source/` holds the original PNG atlases, skyline and title art plus
the crops and sprites derived from them. `tools/build_textures.py` converts
them to pre-twiddled RGB565/ARGB4444 arrays, with mip chains where the spec
asks, in `assets/generated/texture_assets.c`. The generated C and header are
checked in, so a normal Dreamcast build needs neither Python nor Pillow.
Preview PNGs are written locally and ignored.

```sh
make textures    # also runs make architecture and make effects
```

| Script | Produces |
| --- | --- |
| `tools/build_architecture.py` | Upper-wall, storefront and cornice modules cropped from the source paintings |
| `tools/build_effect_textures.py` | Deterministic smoke, shadow, headlight and palm-frond RGBA sprites |
| `tools/build_textures.py` | The PVR texture arrays, through `tools/pvr_texture.py` |
| `tools/test_pvr_texture.py` | Checks the packed layout against the KOS and Flycast twiddle and mip offsets |

Gotchas:

- The arrays are already twiddled. The game uploads them with `pvr_txr_load`; `pvr_txr_load_ex` would twiddle them again
- Keep car geometry changes in `tools/build_car_blender.py` and rerun `make model`. The exporter enforces the renderer's 4,096-vertex limit after splitting at UV, normal and material seams. Blender material settings do not change the shipped shading, which lives in the PVR renderer
- The brake overlay reuses the exporter's brake-lamp triangle indices, and the lamp swatches share the existing 128-pixel lamp atlas. Lamp edits in `tools/build_textures.py` need `make textures` afterwards
- There is no fixed art-memory allowance. Textures, framebuffers, polygon buffers and tile bins share 8 MiB of VRAM; `make qa-benchmark` logs what is free after allocation
