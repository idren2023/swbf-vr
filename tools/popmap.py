import subprocess, sys
import numpy as np
from PIL import Image

clip, out = sys.argv[1], sys.argv[2]
W, H = map(int, subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'stream=width,height', '-of', 'csv=p=0', clip], capture_output=True, text=True).stdout.strip().split(','))
raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', clip, '-f', 'rawvideo', '-pix_fmt', 'gray', '-'],
                     capture_output=True).stdout
f = np.frombuffer(raw, np.uint8).reshape(-1, H, W).astype(np.int16)
n = len(f)
# block means (16x16) to drop blade-level sway; look for blocks that switch between two levels
B = 16
b = f[:, :H // B * B, :W // B * B].reshape(n, H // B, B, W // B, B).mean(axis=(2, 4))
rng = b.max(0) - b.min(0)
d = np.abs(np.diff(b, axis=0))
jumps = (d > 6).sum(0)  # how many frame-to-frame steps over 6 levels per block
ys, xs = np.unravel_index(np.argsort(jumps.ravel())[::-1][:12], jumps.shape)
for y, x in zip(ys, xs):
    series = b[:, y, x]
    steps = np.nonzero(d[:, y, x] > 6)[0]
    print(f'block x{x*B} y{y*B}: range {rng[y, x]:.0f}, jumps {jumps[y, x]}, at frames {steps[:20].tolist()}')
img = np.clip(jumps * 25, 0, 255).astype(np.uint8)
Image.fromarray(img).resize((W // 2, H // 2), Image.NEAREST).save(out)
mean = f.mean(0).astype(np.uint8)
Image.fromarray(mean).resize((W // 2, H // 2)).save(out.replace('.png', '-mean.png'))
print(f'score: blocks with 10+ jumps {int((jumps >= 10).sum())}, max jumps {int(jumps.max())}')
