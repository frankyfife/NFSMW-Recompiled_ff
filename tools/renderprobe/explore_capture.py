"""Details for compare_capture.py: which game draws differ (and what was
called right before them), and where the CP draws without a game draw are."""
import collections
import os
import sys

import numpy as np

sys.argv = sys.argv[:3]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "compare_capture.py")).read().split("# Registers the draw packet")[0])

ignore = {0x21FC}
mask = np.array([r not in ignore for r in REGS])
# Recompute the order matching, keeping which CP draws were used.
used = set()
ci = 0
bad = []
for gi, g in enumerate(gdraws):
    best = None
    for k in range(ci, min(ci + 8, len(cdraws))):
        nd = int(np.count_nonzero((cdraws[k]["regs"] != g["regs"]) & mask))
        if best is None or nd < best[1]:
            best = (k, nd)
    k, nd = best
    used.add(k)
    if nd:
        bad.append((gi, k, nd))
    ci = k + 1

# What precedes a differing draw on the game side.
idx = {id(e): i for i, e in enumerate(events)}
before = collections.Counter()
for gi, k, nd in bad:
    i = idx[id(gdraws[gi])]
    prev = tuple(e["name"] for e in events[max(0, i - 3):i])
    before[(gdraws[gi]["name"],) + prev] += 1
print("differing draws by (draw entry, 3 calls before):")
for key, n in before.most_common(8):
    print(f"  {n:5d}  {key}")

# Constant registers: is the CP value the game value of a nearby earlier draw?
gi, k, nd = bad[len(bad) // 2]
g, c = gdraws[gi]["regs"], cdraws[k]["regs"]
diff = np.nonzero((g != c) & mask)[0]
print(f"\nexample: game draw {gi} vs CP draw {k}, {nd} registers differ:")
for j in diff[:12]:
    print(f"  0x{REGS[j]:04X}: game {g[j]:08X}  cp {c[j]:08X}")

unused = [k for k in range(len(cdraws)) if k not in used]
print(f"\nCP draws without a game draw: {len(unused)}; first at {unused[:10]}")
runs = []
for k in unused:
    if runs and runs[-1][1] == k - 1:
        runs[-1][1] = k
    else:
        runs.append([k, k])
print("runs (first, last):", runs[:15], "...", runs[-5:])
for k in unused[:3] + unused[len(unused) // 2:len(unused) // 2 + 3]:
    c = cdraws[k]
    print(f"  CP {k}: prim {c['prim']} count {c['count']} indexed {c['indexed']} dma {c['dma']:08X}")
