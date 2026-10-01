"""Compares the native replay's front buffer with a screenshot of the same
frame from the emulation.

    python compare_ref.py <reference screenshot> <replay front buffer png> <out png>

The screenshot is cropped to the game image (the non-black area). The
emulation shows only a centered part of the front buffer (overscan), so the
replay is cropped by the factor that fits best and scaled to the same size.
Writes reference | replay | difference (x4) side by
side and prints the mean absolute difference per channel.
"""
import sys

import numpy as np
from PIL import Image

ref_path, replay_path, out_path = sys.argv[1:4]
replay = Image.open(replay_path).convert("RGB")
ref = Image.open(ref_path).convert("RGB")
a = np.asarray(ref).astype(int)
ys, xs = np.nonzero(a.sum(axis=2) > 30)
ref = ref.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1)).resize(replay.size, Image.LANCZOS)
r = np.asarray(ref).astype(int)
best = None
W, H = replay.size
for f in np.arange(0.80, 0.981, 0.005):
    cw, ch = W * f, H * f
    for dx in (-4, -2, 0, 2, 4):
        for dy in (-4, -2, 0, 2, 4):
            box = ((W - cw) / 2 + dx, (H - ch) / 2 + dy, (W + cw) / 2 + dx, (H + ch) / 2 + dy)
            n = np.asarray(replay.resize(replay.size, Image.BILINEAR, box=box)).astype(int)
            score = np.abs(r - n).mean()
            if best is None or score < best[0]:
                best = (score, f, dx, dy, box)
score, f, dx, dy, box = best
print(f"best fit: replay cropped to {f:.3f} of its size, offset ({dx}, {dy})")
replay = replay.resize(replay.size, Image.LANCZOS, box=box)
n = np.asarray(replay).astype(int)
diff = np.abs(r - n)
print(f"mean absolute difference per channel (0-255): {diff.mean(axis=(0, 1)).round(2)}; "
      f"pixels differing by more than 32: {(diff.max(axis=2) > 32).mean() * 100:.1f} %")
w, h = replay.size
out = Image.new("RGB", (w * 3, h))
out.paste(ref, (0, 0))
out.paste(replay, (w, 0))
out.paste(Image.fromarray(np.clip(diff * 4, 0, 255).astype(np.uint8)), (2 * w, 0))
out.save(out_path)
