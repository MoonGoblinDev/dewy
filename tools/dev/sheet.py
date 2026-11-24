"""sheet.py <dir> <out.png> [x0 y0 x1 y1]: tile the burst frames (eye crops) with clip times."""
import sys, glob, json; from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7]) if len(sys.argv) > 6 else (230, 62, 410, 116)
times = json.load(open(d + '/times.json'))
fs = sorted(glob.glob(d + '/b*.png'))
W, H = (x1 - x0) * 2, (y1 - y0) * 2
cols = 6; rows = (len(fs) + cols - 1) // cols
w = Image.new('RGB', (cols * W, rows * (H + 16)), 'white')
for k, p in enumerate(fs):
    im = Image.open(p).convert('RGB').crop((x0, y0, x1, y1)).resize((W, H), Image.LANCZOS)
    x = (k % cols) * W; y = (k // cols) * (H + 16)
    w.paste(im, (x, y + 16)); ImageDraw.Draw(w).text((x + 3, y + 2), f'{times[k] / 1000:.2f}s', fill='black')
w.save(out)
