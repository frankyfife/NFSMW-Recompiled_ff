"""Per key press: extra sound energy on top of the background after the press.

    python press_sfx.py <name>   (reads audio_<name>.raw, .raw.ts, nav_<name>_keys.txt)
"""
import os
import sys
import numpy as np

d = os.path.join(os.environ["TEMP"], "claude")
name = sys.argv[1]
keys_filter = sys.argv[2] if len(sys.argv) > 2 else "HJ"
raw = np.fromfile(os.path.join(d, f"audio_{name}.raw"), dtype=">f4")
ts = np.fromfile(os.path.join(d, f"audio_{name}.raw.ts"), dtype="<u8").astype(np.float64)
n = min(len(raw) // 1536, len(ts))
frames = raw[: n * 1536].reshape(n, 6, 256).astype(np.float32)
ts = ts[:n]
freq = 10_000_000.0  # QPC on this machine (Stopwatch.Frequency); checked below
# 4 sub-blocks per frame (64 samples), timestamp interpolated.
sub = frames.reshape(n, 6, 4, 64)
rms = np.sqrt((sub ** 2).mean(axis=(1, 3)))  # (n, 4)
rms = rms.reshape(-1)
t_sub = (np.repeat(ts, 4) + np.tile(np.arange(4), n) * (64 / 48000) * freq)
presses = []
for line in open(os.path.join(d, f"nav_{name}_keys.txt")):
    q, _, k = line.split()
    if k in keys_filter:
        presses.append((float(q), k))
print(f"{n} frames, {len(presses)} presses")
dt = 64 / 48000
rows = []
for i, (q, k) in enumerate(presses):
    idx = np.searchsorted(t_sub, q)
    pre = rms[max(0, idx - int(0.4 / dt)): idx]
    post = rms[idx: idx + int(1.3 / dt)]
    if len(pre) < 10 or len(post) < 10:
        continue
    base = np.median(pre)
    excess = np.clip(post - base, 0, None)
    energy = float((excess ** 2).sum() * dt * 1000)
    above = np.nonzero(post > base * 1.6 + 0.004)[0]
    length = (above[-1] - above[0] + 1) * dt * 1000 if len(above) else 0
    onset = above[0] * dt * 1000 if len(above) else -1
    rows.append((i, k, base, energy, onset, length))
    print(f"{i:3d} {k} base={base:.3f} energy={energy:7.2f} onset={onset:5.0f} ms length={length:5.0f} ms")
e = np.array([r[3] for r in rows])
l = np.array([r[5] for r in rows])
half = len(rows) // 2
print(f"first half: energy {e[:half].mean():.2f}, length {l[:half].mean():.0f} ms | "
      f"second half: energy {e[half:].mean():.2f}, length {l[half:].mean():.0f} ms")
