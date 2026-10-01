"""Is the stray sound before a click a repeating buffer? Autocorrelation of
channel 0 between two offsets after a key press.

    python fragment.py <name> <press index> <from ms> <to ms>
"""
import os
import sys
import numpy as np

d = os.path.join(os.environ["TEMP"], "claude")
name, idx, a, b = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
raw = np.fromfile(os.path.join(d, f"audio_{name}.raw"), dtype=">f4")
ts = np.fromfile(os.path.join(d, f"audio_{name}.raw.ts"), dtype="<u8").astype(np.float64)
n = min(len(raw) // 1536, len(ts))
samples = raw[: n * 1536].reshape(n, 6, 256).astype(np.float32).transpose(1, 0, 2).reshape(6, -1)
q = float(open(os.path.join(d, f"nav_{name}_keys.txt")).read().split("\n")[idx].split()[0])
fi = np.searchsorted(ts, q)
s0 = fi * 256 + int(a * 48)
s1 = fi * 256 + int(b * 48)
for ch in (0, 4):
    x = samples[ch, s0:s1].astype(np.float64)
    x = x - x.mean()
    ac = np.correlate(x, x, "full")[len(x) - 1:]
    ac /= ac[0] if ac[0] else 1
    lag = np.argmax(ac[48:]) + 48  # skip lags under 1 ms
    print(f"ch{ch}: {len(x)} samples, best repeat {lag} samples ({lag/48:.2f} ms), corr {ac[lag]:.3f}")
    print("   first 24 samples:", " ".join(f"{v:+.3f}" for v in samples[ch, s0:s0 + 24]))
