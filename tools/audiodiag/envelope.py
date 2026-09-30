"""Envelope (5 ms steps, per channel peak) after chosen key presses.

    python envelope.py <name> <press indices comma> [ms]
"""
import os
import sys
import numpy as np

d = os.path.join(os.environ["TEMP"], "claude")
name = sys.argv[1]
which = [int(x) for x in sys.argv[2].split(",")]
span = float(sys.argv[3]) / 1000 if len(sys.argv) > 3 else 0.5
raw = np.fromfile(os.path.join(d, f"audio_{name}.raw"), dtype=">f4")
ts = np.fromfile(os.path.join(d, f"audio_{name}.raw.ts"), dtype="<u8").astype(np.float64)
n = min(len(raw) // 1536, len(ts))
frames = raw[: n * 1536].reshape(n, 6, 256).astype(np.float32)
samples = frames.transpose(1, 0, 2).reshape(6, -1)  # (6, n*256)
t0 = ts[0]
freq = 1e7
presses = [(float(l.split()[0]), l.split()[2]) for l in open(os.path.join(d, f"nav_{name}_keys.txt"))]
for idx in which:
    q, k = presses[idx]
    fi = np.searchsorted(ts, q)
    s0 = fi * 256
    seg = samples[:, s0: s0 + int(span * 48000)]
    step = 240
    print(f"press {idx} ({k}) at frame {fi}:")
    for j in range(0, seg.shape[1], step):
        peaks = np.abs(seg[:, j: j + step]).max(axis=1)
        if peaks.max() > 1e-4:
            print(f"  +{j/48:5.0f} ms  " + " ".join(f"{p:.4f}" for p in peaks))
