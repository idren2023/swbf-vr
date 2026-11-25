import subprocess, sys
import numpy as np

clip = sys.argv[1]
y0, y1 = (float(v) for v in sys.argv[2:4]) if len(sys.argv) > 3 else (0.0, 1.0)
W, H = map(int, subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'stream=width,height', '-of', 'csv=p=0', clip],
                               capture_output=True, text=True).stdout.strip().split(','))
raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', clip, '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], capture_output=True).stdout
f = np.frombuffer(raw, np.uint8).reshape(-1, H, W).astype(np.float32)[:, int(H * y0):int(H * y1), :]
win = np.outer(np.hanning(f.shape[1]), np.hanning(f.shape[2]))
ref = np.fft.fft2((f[0] - f[0].mean()) * win)
out = []
for g in f:
    G = np.fft.fft2((g - g.mean()) * win)
    r = np.fft.ifft2(G * np.conj(ref) / (np.abs(G * np.conj(ref)) + 1e-6)).real
    dy, dx = np.unravel_index(r.argmax(), r.shape)
    dy = dy - r.shape[0] if dy > r.shape[0] // 2 else dy
    dx = dx - r.shape[1] if dx > r.shape[1] // 2 else dx
    out.append((dx, dy))
    ref = G
print('shift against the previous frame (dx,dy px):')
print(' '.join(f'{dx},{dy}' for dx, dy in out))
