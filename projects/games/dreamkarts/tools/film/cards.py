"""Overlay PNGs (1920x1080, transparent) in DreamKarts' own lettering style.

usage: uv run --with pillow --with numpy --with matplotlib tools/film/cards.py <outdir>
Writes title.png, outro.png and cap-<slug>.png for every caption below."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import build_textures as bt  # noqa: E402

W, H = 1920, 1080
FONT = bt.font_path('DejaVuSans-BoldOblique.ttf')
WARM = [(0.0, (255, 252, 190)), (0.38, (255, 214, 40)), (0.62, (255, 150, 20)), (1.0, (230, 70, 20))]
COOL = [(0.0, (255, 255, 255)), (1.0, (180, 240, 255))]
NAVY = (20, 60, 190)


def text(size, s, stops, outline=NAVY, ow=None, box=None):
    f = ImageFont.truetype(FONT, size)
    l, t, r, b = f.getbbox(s)
    ow = ow or max(3, size // 14)
    bw, bh = box or (r - l + 4 * ow + 40, b - t + 4 * ow + 40)
    return bt.gradient_text_layer((bw, bh), s, f, stops, outline, ow)


def plain(size, s, fill=(255, 255, 255), stroke=(16, 22, 48), sw=None):
    f = ImageFont.truetype(FONT, size)
    l, t, r, b = f.getbbox(s)
    sw = sw if sw is not None else max(2, size // 12)
    img = Image.new('RGBA', (r - l + 2 * sw + 8, b - t + 2 * sw + 8), (0, 0, 0, 0))
    ImageDraw.Draw(img).text((sw + 4 - l, sw + 4 - t), s, font=f, fill=fill, stroke_width=sw, stroke_fill=stroke)
    return img


def logo(size):
    """"DreamKarts" with the title screen's letters: capitals a size up."""
    big = ImageFont.truetype(FONT, int(size * 112 / 96))
    small = ImageFont.truetype(FONT, size)
    cell = int(size * 1.6)
    glyphs = []
    for ch in 'DreamKarts':
        f = big if ch.isupper() else small
        glyphs.append((bt.gradient_text_layer((cell, cell), ch, f, WARM, NAVY, max(4, size // 13)), f.getlength(ch)))
    gap = size * 0.02
    total = sum(w for _, w in glyphs) + gap * (len(glyphs) - 1)
    img = Image.new('RGBA', (int(total + cell), cell), (0, 0, 0, 0))
    pen = cell / 2
    for layer, w in glyphs:
        # Each glyph is centred in its cell: put that centre mid-advance.
        img.alpha_composite(layer, (int(pen + w / 2 - cell / 2), 0))
        pen += w + gap
    return img


def centred(canvas, layer, cy):
    canvas.alpha_composite(layer, ((W - layer.width) // 2, int(cy - layer.height / 2)))


def shade(canvas, y0, y1, alpha=150):
    """A soft dark band so text reads over bright footage."""
    band = Image.new('L', (W, H), 0)
    ImageDraw.Draw(band).rectangle([0, y0, W, y1], fill=alpha)
    band = band.filter(ImageFilter.GaussianBlur(40))
    dark = Image.new('RGBA', (W, H), (6, 10, 30, 0))
    dark.putalpha(band)
    canvas.alpha_composite(dark)


def title(out, subtitle):
    c = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    shade(c, 330, 760, 140)
    centred(c, logo(175), 470)
    centred(c, plain(54, subtitle), 680)
    c.save(out / 'title.png')


def outro(out, lines):
    c = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    shade(c, 180, 900, 190)
    centred(c, logo(170), 360)
    y = 560
    for s, size, col in lines:
        layer = plain(size, s, fill=col)
        centred(c, layer, y)
        y += int(size * 1.6)
    c.save(out / 'outro.png')


def caption(out, slug, top, bottom=None):
    """A lower-left banner like the HUD's panels: yellow heading, white line."""
    c = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    a = text(64, top, WARM, ow=6)
    b = text(42, bottom, COOL, ow=4) if bottom else None
    pw = max(a.width, b.width if b else 0) + 50
    ph = a.height + (b.height - 30 if b else 0) + 10
    x0, y0 = 250, H - 175 - ph   # clear of the game's own lower-left panel
    panel = Image.new('RGBA', (pw, ph), (0, 0, 0, 0))
    d = ImageDraw.Draw(panel)
    for y in range(ph):
        t = y / max(ph - 1, 1)
        d.line([(0, y), (pw, y)], fill=(int(20 + 30 * t), int(70 + 60 * t), int(190 + 40 * t), 215))
    d.rectangle([0, 0, pw, 4], fill=(255, 255, 255, 235))
    d.rectangle([0, ph - 5, pw, ph], fill=(255, 255, 255, 235))
    c.alpha_composite(panel, (x0, y0))
    c.alpha_composite(a, (x0 + 10, y0 - 8))
    if b:
        c.alpha_composite(b, (x0 + 14, y0 + a.height - 38))
    c.save(out / f'cap-{slug}.png')


CAPTIONS = {
    'spectate': ('ONLINE SPECTATOR MODE', 'A TV director cuts between trackside cameras'),
    'demo-lobby': ('DEMO LOBBY', 'Six bots replay real CPU races, round the clock'),
    'beach': ('EMERALD BEACH', 'Boardwalk, rock arch and lighthouse'),
    'figure8': ('CORAL CROSSING', 'A figure 8 with a jump over the crossing'),
    'harbour': ('PALM HARBOUR', 'Chicanes and hairpins by the sea'),
    'temple': ('TEMPLE HILL', '18 m of climb, a summit jump and a viaduct'),
    'skyway': ('NEON SKYWAY', 'A night run over a cyberpunk city, with a loop'),
    'cyberstone': ('CYBERSTONE', 'Neon hills and a banked helix, after Silverstone'),
    'spiralcanyon': ('SPIRAL CANYON', 'Corkscrews up two rock towers, through checkered loops'),
    'drift': ('DRIFT AND MINI-TURBO', 'Blue, orange, then purple sparks'),
    'split': ('TWO-PLAYER SPLIT SCREEN', 'Grand Prix or head-to-head Versus'),
    'setup': ('RACE SETUP', 'Eight Chao colours, 50 / 100 / 150cc'),
    'tracks': ('TRACK SELECT', 'Six courses'),
    'single': ('GRAND PRIX', 'Three laps against five CPU Chao'),
    'online': ('ONLINE MULTIPLAYER', 'Up to six Dreamcasts, by broadband or dial-up'),
}

if __name__ == '__main__':
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    title(out, 'A kart racer for the Sega Dreamcast')
    outro(out, [('Free homebrew for the Sega Dreamcast', 50, (255, 255, 255)),
                ('Single player  ·  split screen  ·  online', 44, (180, 240, 255)),
                ('github.com/richstokes/dreamcast-homebrew', 46, (255, 214, 40))])
    for slug, (a, b) in CAPTIONS.items():
        caption(out, slug, a, b)
    print('ok')
