"""Asset packs and generated headers for the games' build tools.

DreamKarts' and Ridgeline's tools/build_audio.py and tools/build_textures.py
render their sounds and textures, then hand them here to be packed (sfx.pak,
textures.pak: 32-byte aligned entries) and described to the C side
(sound_assets.h, texture_assets.h, font_metrics.h). The games differ in their
symbol prefix and a few header comments, which the writers take as arguments.
"""

from __future__ import annotations

import wave
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np

if TYPE_CHECKING:
    from PIL import Image

# The sound tools run with numpy and scipy, the texture tools with numpy and
# Pillow, so each half imports its own dependency where it is used.

ALIGN = 32
LOOP_GUARD = 16   # samples repeated after a loop


def _align(pak: bytearray) -> None:
    while len(pak) % ALIGN:
        pak.append(0)


# ---------------------------------------------------------------------------
# Sound

def write_wav(path: Path, data, rate: int, channels: int = 1) -> None:
    from synthkit import to_pcm16
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(to_pcm16(np.asarray(data)).tobytes())


def loop_seam(x):
    """A loop's wrap discontinuity against its own sample-to-sample motion:
    (seam, p99.9 of |first difference|, curvature seam, p99.9 of |second
    difference|)."""
    seam = abs(x[0] - x[-1])
    seam2 = abs(x[1] - 2 * x[0] + x[-1])
    return (seam, np.percentile(np.abs(np.diff(x)), 99.9),
            seam2, np.percentile(np.abs(np.diff(x, 2)), 99.9))


def write_sfx_pak(out: Path, previews: Path, sounds, *, max_bytes: int | None = None,
                  curvature: bool = False) -> list[tuple[str, int, int, int, int]]:
    """Pack finished effects into out/sfx.pak and write a listening copy of each.

    sounds: (name, x, loop, rate) per effect, x being float samples in [-1, 1]
    with fades and level already applied. Entries are 16-bit mono PCM at
    32-byte offsets. A loop is followed by its first samples again, outside
    its byte count, so interpolation that peeks past the loop end reads
    continuous data. Each effect is checked for non-finite samples, the AICA
    length limit and, for loops, a wrap discontinuity (a seam larger than the
    loop's own motion; `curvature` also checks the second difference). Prints
    a row per effect; raises SystemExit on a failed check or a pak over
    `max_bytes`. Returns (name, offset, bytes, loop, rate) per effect.
    """
    from synthkit import MAX_SFX_SAMPLES, to_pcm16
    pak = bytearray()
    entries = []
    print(f"{'effect':16s} {'secs':>5s} {'rate':>5s} {'KiB':>5s} {'peak':>6s} {'rms':>6s} {'dc':>8s}  notes")
    for name, x, loop, rate in sounds:
        x = np.asarray(x, dtype=np.float64)
        if not np.all(np.isfinite(x)):
            raise SystemExit(f"{name}: non-finite samples")
        if len(x) > MAX_SFX_SAMPLES:
            raise SystemExit(f"{name}: {len(x)} samples exceeds AICA limit")
        pcm = to_pcm16(x)
        _align(pak)
        off = len(pak)
        pak += pcm.tobytes()
        if loop:
            pak += pcm[:LOOP_GUARD].tobytes()
        entries.append((name, off, len(pcm) * 2, int(loop), rate))
        peak = 20 * np.log10(np.max(np.abs(x)))
        rms = 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)
        if loop:
            seam, p999, seam2, p999_2 = loop_seam(x)
            note = f"seam {seam:.4f} (p99.9 {p999:.4f})"
            if curvature:
                note += f", curve {seam2:.4f} (p99.9 {p999_2:.4f})"
            if seam > p999 or (curvature and seam2 > p999_2):
                raise SystemExit(f"{name}: loop seam discontinuity")
            preview = np.tile(x, max(2, int(np.ceil(2.0 * rate / len(x)))))
        else:
            note = f"edges {abs(x[0]):.4f}/{np.max(np.abs(x[-32:])):.4f}"
            preview = x
        print(f"{name:16s} {len(x) / rate:5.2f} {rate:5d} {len(pcm) * 2 / 1024:5.1f} {peak:6.1f} "
              f"{rms:6.1f} {np.mean(x):+8.5f}  {note}")
        write_wav(previews / f"{name.lower()}.wav", preview, rate)
    _align(pak)
    if max_bytes is not None and len(pak) > max_bytes:
        raise SystemExit(f"sfx.pak is {len(pak)} bytes, over the {max_bytes} budget")
    (out / "sfx.pak").write_bytes(bytes(pak))
    print(f"sfx.pak: {len(pak)} bytes ({len(pak) / 1024:.0f} KiB), {len(entries)} effects")
    return entries


def write_sound_header(out: Path, prefix: str, entries, notes, names, *, struct_comment: str,
                       table_comment: str | None = None, names_symbol: str, names_comment: str,
                       tail=()) -> None:
    """Write out/sound_assets.h: <P>_SFX_RATE, the <p>_sfx_asset_t table
    indexed by the <p>_sfx_id_t enum, and a display-name array.

    entries: as write_sfx_pak returns them. notes: the trailing comment on
    each table row. names: each effect's display string, stored in
    `names_symbol`. tail: lines before the closing #endif.
    """
    from synthkit import SFX_RATE
    p, P = prefix.lower(), prefix.upper()
    h = ["/* Generated by tools/build_audio.py. Do not edit by hand. */",
         f"#ifndef {P}_SOUND_ASSETS_H", f"#define {P}_SOUND_ASSETS_H", "",
         "#include <stdint.h>", "",
         f"#define {P}_SFX_RATE {SFX_RATE}", "",
         f"/* {struct_comment} */",
         f"typedef struct {{ uint32_t offset, bytes, loop, rate; }} {p}_sfx_asset_t;", "",
         "typedef enum {"]
    h += [f"    {P}_SFX_{name}," for name, *_ in entries]
    h += [f"    {P}_SFX_COUNT", f"}} {p}_sfx_id_t;", ""]
    if table_comment:
        h.append(f"/* {table_comment} */")
    h.append(f"static const {p}_sfx_asset_t {p}_sfx_assets[{P}_SFX_COUNT] __attribute__((unused)) = {{")
    for (name, off, size, loop, rate), note in zip(entries, notes):
        h.append(f"    {{ {off}u, {size}u, {loop}u, {rate}u }}, /* {note} */")
    h += ["};", "", f"/* {names_comment} */",
          f"static const char *const {names_symbol}[{P}_SFX_COUNT] __attribute__((unused)) = {{"]
    h += [f'    "{n}",' for n in names]
    h += ["};", "", *tail, f"#endif /* {P}_SOUND_ASSETS_H */", ""]
    (out / "sound_assets.h").write_text("\n".join(h))


# ---------------------------------------------------------------------------
# Textures

@dataclass
class TexSpec:
    name: str
    fmt: str          # RGB565 or ARGB4444
    mip: bool
    wrap: bool


def to_image(rgb: np.ndarray, alpha: np.ndarray | None = None) -> Image.Image:
    from PIL import Image
    rgb = np.clip(rgb, 0, 255).astype(np.uint8)
    if alpha is None:
        return Image.fromarray(rgb, "RGB")
    a = np.clip(alpha, 0, 255).astype(np.uint8)
    return Image.fromarray(np.dstack([rgb, a]), "RGBA")


def write_texture_pak(out: Path, previews: Path, items):
    """Pack (TexSpec, image) pairs into out/textures.pak, twiddled with mips
    where asked, each at a 32-byte offset, and save every image as painted to
    previews. The opaque textures' mips keep their mid-sized detail (lanczos):
    with no anisotropic filtering, the console shows roads, curbs and walls at
    small mip levels, and area filtering turned those flat. Returns the table,
    (spec, w, h, offset, bytes) per texture, and the pak's size.
    """
    from pvr_texture import pack_texture
    pak = bytearray()
    table = []
    for spec, img in items:
        img.save(previews / f"tex-{spec.name}.png")
        words = pack_texture(img, spec.fmt, mipmap=spec.mip, wrap=spec.wrap,
                             mip_filter="lanczos" if spec.fmt == "RGB565" else "box")
        data = np.array(words, dtype="<u2").tobytes()
        _align(pak)
        table.append((spec, img.width, img.height, len(pak), len(data)))
        pak += data
        print(f"  {spec.name:10s} {img.width}x{img.height} {spec.fmt}{' mip' if spec.mip else ''} {len(data)} bytes")
    (out / "textures.pak").write_bytes(bytes(pak))
    print(f"textures.pak: {len(pak)} bytes, {len(table)} textures")
    return table, len(pak)


def write_texture_header(out: Path, prefix: str, table, tail=()) -> None:
    """Write out/texture_assets.h: TEX_* ids and the <p>_tex_asset_t table.
    tail: lines before the closing #endif."""
    p, P = prefix.lower(), prefix.upper()
    h = ["/* Generated by tools/build_textures.py. Do not edit. */",
         f"#ifndef {P}_TEXTURE_ASSETS_H", f"#define {P}_TEXTURE_ASSETS_H", "#include <stdint.h>", "",
         f"#define {P}_TEXFMT_RGB565 0", f"#define {P}_TEXFMT_ARGB4444 1", "",
         f"typedef struct {{ uint32_t offset, bytes; uint16_t w, h; uint8_t fmt, mip, pad[2]; }} {p}_tex_asset_t;",
         "", "enum {"]
    h += [f"    TEX_{spec.name.upper()}," for spec, *_ in table]
    h += ["    TEX_COUNT", "};", "", f"static const {p}_tex_asset_t {p}_tex_assets[TEX_COUNT] = {{"]
    for spec, w, hh, off, size in table:
        h.append(f"    {{{off}u, {size}u, {w}, {hh}, {P}_TEXFMT_{spec.fmt}, {int(spec.mip)}, {{0, 0}}}},  /* {spec.name} */")
    h += ["};", "", *tail, "#endif", ""]
    (out / "texture_assets.h").write_text("\n".join(h))


def write_font_metrics(out: Path, prefix: str, font_img: Image.Image, glyphs, cap, row_h: int, *,
                       glyph_type: str | None = None, cap_comment=(), glyph_comment=(), tail=()) -> None:
    """Write out/font_metrics.h: the HUD font atlas's dimensions, the capitals'
    rows and the ASCII 32..127 glyph table (atlas x, row, rect width, rect
    left from the pen, advance), laid out like eng_glyph_t.

    glyph_type names a typedef for the glyph struct; None leaves it anonymous.
    cap_comment and glyph_comment are comment lines above those parts, tail
    the lines before the closing #endif.
    """
    P = prefix.upper()
    m = ["/* Generated by tools/build_textures.py. Do not edit. */",
         f"#ifndef {P}_FONT_METRICS_H", f"#define {P}_FONT_METRICS_H", "#include <stdint.h>", "",
         f"#define FONT_ROW_H {row_h}", f"#define FONT_TEX_W {font_img.width}", f"#define FONT_TEX_H {font_img.height}",
         *cap_comment,
         f"#define FONT_CAP_TOP {cap[0]}", f"#define FONT_CAP_BOTTOM {cap[1]}",
         *glyph_comment]
    fields = "{ uint16_t x; uint8_t row, w; int8_t ox; uint8_t adv; }"
    if glyph_type:
        m += [f"typedef struct {fields} {glyph_type};", f"static const {glyph_type} font_glyphs[96] = {{"]
    else:
        m.append(f"static const struct {fields} font_glyphs[96] = {{")
    for i in range(0, 96, 4):
        m.append("    " + " ".join(f"{{{x}, {r}, {w}, {ox}, {adv}}}," for x, r, w, ox, adv in glyphs[i:i + 4]))
    m += ["};", "", *tail, "#endif", ""]
    (out / "font_metrics.h").write_text("\n".join(m))
