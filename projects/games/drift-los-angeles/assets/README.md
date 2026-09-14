# Art pipeline

The eight checked-in PNG material atlases, skyline panorama, and title
illustration are high-resolution source art. The converter crops atlas
quadrants or fits whole-image artwork, scales
each asset to a PowerVR-friendly size, cross-blends wrapping edges where
appropriate, applies ordered dithering, and emits aligned RGB565 arrays in
`assets/generated/texture_assets.c`.

Rebuild the embedded textures with:

```sh
make textures
```

The generated C and header are checked in so a normal Dreamcast build does not
need Python or Pillow. Preview PNGs are generated locally and ignored.
