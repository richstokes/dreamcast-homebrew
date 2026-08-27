# Visual asset pipeline

The three files in `source/` are the full-resolution production atlases used
for Gravity Wave's terrain, vehicles, structures, foliage, mist, rift energy,
and volcanic effects. They are intentionally kept as source art.

Run the deterministic converter from the project root whenever an atlas is
updated:

```sh
uv run tools/build_textures.py \
  --source-dir assets/source \
  --output-dir assets/generated
```

The tool crops each atlas quadrant, downsamples to a PVR-compatible power of
two, conditions wrapped material edges, applies ordered dithering, emits
RGB565 or ARGB4444 texels, and saves preview PNGs. The generated C data is
aligned for `pvr_txr_load()` and compiled directly into the ELF, so the game
does not require a disc filesystem at runtime.
