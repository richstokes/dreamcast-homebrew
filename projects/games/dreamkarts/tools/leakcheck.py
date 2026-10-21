"""Find holes in Spiral Canyon's rock from a leak-test capture.

The capture (VARIANT=capture EXTRA_CFLAGS="-DDK_AT_VIEWS=3 -DDK_ROCK_TINT",
then tools/capture.py) puts cameras inside the rock and takes each shot
twice: "-a" is the real world (rock meshes tinted, everything else dark
grey, the sky black), "-b" is only the reference rock, the height function
itself as a white heightfield. Where the reference shows its surface and
the real shot shows no rock, the rock meshes have a hole. The window's
own borders are found from a reference shot of a normal build (one with
sky all round) and ignored.

Usage: uv run --with pillow --with numpy tools/leakcheck.py captures/leak1 --ref captures/views31/view-10.png
Exits 1 and writes <dir>/leaks.png when anything is flagged.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def content_box(ref: Path):
    a = np.asarray(Image.open(ref).convert("RGB")).max(axis=2)
    cols = np.where((a > 16).mean(axis=0) > 0.5)[0]
    rows = np.where((a > 16).mean(axis=1) > 0.5)[0]
    return cols.min(), rows.min(), cols.max() + 1, rows.max() + 1


def erode(mask: np.ndarray, n: int) -> np.ndarray:
    m = mask.copy()
    for _ in range(n):
        inner = m[1:-1, 1:-1] & m[:-2, 1:-1] & m[2:, 1:-1] & m[1:-1, :-2] & m[1:-1, 2:]
        m = np.zeros_like(m)
        m[1:-1, 1:-1] = inner
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=Path)
    ap.add_argument("--ref", type=Path, required=True)
    ap.add_argument("--thresh", type=float, default=0.003)
    ap.add_argument("--title", type=int, default=40, help="rows of window title bar to skip")
    a = ap.parse_args()
    x0, y0, x1, y1 = content_box(a.ref)
    y0 += a.title
    results = []
    for fa in sorted(a.dir.glob("leak-*-a.png")):
        fb = fa.with_name(fa.name[:-5] + "b.png")
        if not fb.exists():
            continue
        real = np.asarray(Image.open(fa).convert("RGB")).astype(int)[y0:y1, x0:x1]
        refi = np.asarray(Image.open(fb).convert("RGB")).astype(int)[y0:y1, x0:x1]
        real_rock = (real.max(axis=2) - real.min(axis=2)) > 40   # a tint, not grey or black
        ref_rock = refi.min(axis=2) > 150
        # Only holes: where the reference's surface is seen (white) the
        # real meshes must show rock first. The reverse (real rock where
        # the reference is black) is not a defect: black there is as often
        # the inside of the reference's solid (a ray that never crosses the
        # surface) as sky, and a tunnel wall seen from inside the rock is
        # exactly that.
        hole = erode(ref_rock & ~real_rock, 2).mean()
        results.append((hole, fa))
    results.sort(reverse=True)
    flagged = [r for r in results if r[0] > a.thresh]
    print(f"{len(results)} shot pairs, {len(flagged)} with a hole over {a.thresh:.1%}")
    for hole, f in flagged[:80]:
        print(f"  hole {hole:6.2%}  {f.stem[:-2]}")
    if flagged:
        w, h = 320, 240
        n = min(len(flagged), 16)
        sheet = Image.new("RGB", (4 * w, ((n + 1) // 2) * h), "black")
        d = ImageDraw.Draw(sheet)
        for i, (hole, f) in enumerate(flagged[:n]):
            for k, suffix in enumerate(("a", "b")):
                im = Image.open(f.with_name(f.name[:-5] + suffix + ".png")).crop((x0, y0, x1, y1)).resize((w, h))
                sheet.paste(im, (((i % 2) * 2 + k) * w, (i // 2) * h))
            d.text(((i % 2) * 2 * w + 4, (i // 2) * h + 4), f"{f.stem[:-2]} hole {hole:.1%}", fill="yellow")
        sheet.save(a.dir / "leaks.png")
        print("sheet:", a.dir / "leaks.png")
    return 1 if flagged else 0


if __name__ == "__main__":
    raise SystemExit(main())
