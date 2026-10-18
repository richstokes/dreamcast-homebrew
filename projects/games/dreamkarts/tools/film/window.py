"""window.py <video> <out.jpg> <start> <end> [step]: labelled frames of one stretch,
for timing a cut. Needs pillow and matplotlib (uv run --with ...)."""
import subprocess, sys, tempfile
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
src, out, a, b = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
step = float(sys.argv[5]) if len(sys.argv) > 5 else 0.5
d = Path(tempfile.mkdtemp())
subprocess.run(['nice', '-n', '19', 'ffmpeg', '-v', 'error', '-threads', '3', '-ss', str(a), '-t', str(b - a), '-i', src,
                '-vf', f'fps=1/{step},crop=1440:1080:240:0,scale=320:240', '-q:v', '4', str(d / '%04d.jpg')], check=True)
fs = sorted(d.glob('*.jpg'))
cols = 6
rows = (len(fs) + cols - 1) // cols
sheet = Image.new('RGB', (cols * 320, rows * 240))
dr = ImageDraw.Draw(sheet)
import matplotlib
font = ImageFont.truetype(str(Path(matplotlib.get_data_path()) / 'fonts/ttf/DejaVuSans-Bold.ttf'), 22)
for j, f in enumerate(fs):
    x, y = (j % cols) * 320, (j // cols) * 240
    sheet.paste(Image.open(f), (x, y))
    dr.text((x + 6, y + 4), f'{a + j * step:.1f}', font=font, fill=(255, 255, 0), stroke_width=2, stroke_fill=(0, 0, 0))
sheet.save(out, quality=80)
