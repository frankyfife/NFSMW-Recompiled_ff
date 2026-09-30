"""Finds sound events in a raw audio dump (frames of 6 x 256 big-endian floats)
and prints their length and loudness, to spot sounds cut short.

    python audio_events.py dump.raw [last_seconds] [period_s]
"""
import sys
import numpy as np

path = sys.argv[1]
last = float(sys.argv[2]) if len(sys.argv) > 2 else 0
raw = np.fromfile(path, dtype=">f4")
frames = raw[: len(raw) // 1536 * 1536].reshape(-1, 6, 256).astype(np.float32)
# 1.33 ms blocks: 64 samples.
blocks = frames.reshape(-1, 6, 4, 64).transpose(0, 2, 1, 3).reshape(-1, 6, 64)
rms = np.sqrt((blocks ** 2).mean(axis=(1, 2)))
dt = 64 / 48000
t = np.arange(len(rms)) * dt
print(f"{len(frames)} frames, {t[-1]:.1f} s, overall rms {rms.mean():.4f}, max {rms.max():.3f}")
if last:
    sel = t >= t[-1] - last
    rms, t = rms[sel], t[sel]
# Background level: 20th percentile over a sliding 2 s window.
floor = np.percentile(rms, 20)
thr = max(floor * 4, 0.002)
on = rms > thr
events = []
i = 0
n = len(on)
while i < n:
    if on[i]:
        j = i
        gap = 0
        while j < n and gap < int(0.03 / dt):
            gap = 0 if on[j] else gap + 1
            j += 1
        end = j - gap
        seg = rms[i:end]
        events.append((t[i], (end - i) * dt, seg.max(), seg.mean()))
        i = j
    else:
        i += 1
print(f"floor {floor:.4f}, threshold {thr:.4f}, {len(events)} events")
for k, (start, length, peak, mean) in enumerate(events):
    print(f"{k:3d} t={start:7.2f}s len={length*1000:6.0f} ms peak={peak:.3f} mean={mean:.3f}")
