"""Per H/J press: onset, duration until the sound drops below 0.001 for good,
peak, and the level in the 5 ms right before it ends (a cut ends loud)."""
import os
import sys
import numpy as np

d = os.path.join(os.environ["TEMP"], "claude")
name = sys.argv[1]
raw = np.fromfile(os.path.join(d, f"audio_{name}.raw"), dtype=">f4")
ts = np.fromfile(os.path.join(d, f"audio_{name}.raw.ts"), dtype="<u8").astype(np.float64)
n = min(len(raw) // 1536, len(ts))
samples = raw[: n * 1536].reshape(n, 6, 256).astype(np.float32).transpose(1, 0, 2).reshape(6, -1)
env = np.abs(samples).max(axis=0)
step = 240  # 5 ms
for line in open(os.path.join(d, f"nav_{name}_keys.txt")):
    q, _, k = line.split()
    if k not in "HJ":
        continue
    fi = np.searchsorted(ts, float(q))
    seg = env[fi * 256: fi * 256 + 48000]
    blocks = np.array([seg[j:j + step].max() for j in range(0, len(seg) - step, step)])
    base = np.median(blocks[:10])
    loud = np.nonzero(blocks > max(base * 4, 0.003))[0]
    if not len(loud):
        print(f"{k} frame {fi}: nothing")
        continue
    on, off = loud[0], loud[-1]
    print(f"{k} frame {fi:6d}: onset {on*5:4d} ms, lasts {(off-on+1)*5:4d} ms, peak {blocks[on:off+1].max():.3f}, "
          f"last 5 ms {blocks[off]:.4f}, next 5 ms {blocks[off+1] if off+1 < len(blocks) else 0:.4f}, base {base:.4f}")
