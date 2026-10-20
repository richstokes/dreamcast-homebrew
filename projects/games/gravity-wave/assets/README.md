# Visual asset pipeline

The three atlases in `source/` are the full-resolution production art for
Gravity Wave's terrain, vehicles, structures, foliage and effects. Keep them as
source. The derived data in `generated/` is compiled into the ELF, so the game
needs no disc filesystem at runtime.

After changing an atlas, rerun the converter from the project root (or
`make textures`):

```sh
uv run tools/build_textures.py \
  --source-dir assets/source \
  --output-dir assets/generated
```

`tools/build_textures.py` documents how each quadrant becomes a PVR texture;
`generated/previews/` shows the results.
