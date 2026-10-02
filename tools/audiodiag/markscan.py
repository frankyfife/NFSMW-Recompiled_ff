"""For a NATIVE_MARK_MISMATCH recording: frames with the magenta square and
whether each one is also a one-frame glitch (pixels brighter than both
neighbours, square area excluded).
Usage: python markscan.py <video> [start_seconds]"""
import subprocess
import sys

import numpy as np

FF = r'D:\Program Files (x86)\ffmpeg\bin\ffmpeg.exe'
W, H = 480, 270
video = sys.argv[1]
start = sys.argv[2] if len(sys.argv) > 2 else '0'
p = subprocess.Popen([FF, '-v', 'error', '-ss', start, '-i', video, '-vf', f'scale={W}:{H}',
                      '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], stdout=subprocess.PIPE)
fs = W * H * 3
frames = []
while True:
    b = p.stdout.read(fs)
    if len(b) < fs:
        break
    frames.append(np.frombuffer(b, np.uint8).reshape(H, W, 3))
print(len(frames), 'frames')


def marked(f):
    sq = f[2:14, 2:14].astype(int)
    return (sq[..., 0] > 180).mean() > 0.8 and (sq[..., 1] < 80).mean() > 0.8 and (sq[..., 2] > 180).mean() > 0.8


lum = [f.astype(np.int16).mean(2) for f in frames]
mask = np.ones((H, W), bool)
mask[:30, :30] = False
marks = [i for i in range(1, len(frames) - 1) if marked(frames[i])]
print(len(marks), 'marked frames')
glitchy = 0
for i in marks:
    a, b, c = lum[i - 1], lum[i], lum[i + 1]
    n = int((((b - a) > 50) & ((b - c) > 50) & (np.abs(a - c) < 20) & mask).sum())
    big = n > 1000
    glitchy += big
    print(f'  frame {i} ({i / 60:.2f} s): {n} pixels brighter than both neighbours',
          '<- glitch' if big else '')
print(f'{glitchy} of {len(marks)} marked frames are glitches')
