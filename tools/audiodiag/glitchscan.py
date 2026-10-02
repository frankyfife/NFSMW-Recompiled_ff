"""Counts one-frame glitches in a screen recording (autonav.ps1 -Record):
frames with pixels brighter than in BOTH neighbours while the neighbours
agree - wrong geometry or a frame without the colour grading, not motion.
Usage: python glitchscan.py <video> [start_seconds]
(start after the loading screens, whose dialogs blink.)"""
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


def read():
    b = p.stdout.read(fs)
    if len(b) < fs:
        return None
    return np.frombuffer(b, np.uint8).reshape(H, W, 3).astype(np.int16).mean(2)


a, b = read(), read()
i = 1
events = []
while True:
    c = read()
    if c is None:
        break
    # Brighter than both: the wrong geometry lays bright polygons over the
    # picture (a frame of another moment differs in both directions).
    m = (b - a > 50) & (b - c > 50) & (np.abs(a - c) < 20)
    n = int(m.sum())
    if n > 1000:
        events.append((i, n))
    a, b = b, c
    i += 1
# Runs of three or more flagged frames in a row are motion that jitters
# (a frame shown twice, capture beat), not a wrong frame.
flagged = {f for f, n in events}
isolated = [(f, n) for f, n in events
            if not ((f - 1 in flagged and f + 1 in flagged) or (f - 1 in flagged and f - 2 in flagged)
                    or (f + 1 in flagged and f + 2 in flagged)) and n > 1000]
print(f'{i + 1} frames, {len(isolated)} one-frame glitches ({len(events)} flagged frames)')
for f, n in isolated[:30]:
    print(f'  frame {f} ({f / 60:.2f} s): {n} pixels')
