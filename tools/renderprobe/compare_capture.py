"""Compares one frame captured on both sides (stage 1, docs/NATIVE_RENDERER.md).

    python compare_capture.py <dir with capture_game_N.bin and capture_cp_N.bin> N

Game side: every D3D call of the frame; at draws, the register shadow of the
device. GPU side: the register file at every draw packet. Draws are matched
in order by their register state; the report says how many CP draws each
game draw became and which registers differ.
"""
import collections
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
d, frame = sys.argv[1], int(sys.argv[2])
RANGES = [(0x2000, 16), (0x2100, 21), (0x2180, 5), (0x2200, 12), (0x2280, 21), (0x2300, 38),
          (0x2380, 8), (0x4000, 1024), (0x4400, 1024), (0x4800, 192), (0x4900, 40)]
REGS = np.array([first + i for first, n in RANGES for i in range(n)])
NREG = len(REGS)

layer = json.load(open(os.path.join(HERE, "d3d_layer.json")))
names = [e[0] for e in layer["entries"]]


def read_game(path):
    raw = open(path, "rb").read()
    off = 0
    events, draws = [], []
    while off < len(raw):
        magic, seq, entry, *args = struct.unpack_from("<9I", raw, off)
        off += 36
        rec = {"seq": seq, "name": names[entry], "args": args}
        if magic == 0x57415244:
            (n,) = struct.unpack_from("<I", raw, off)
            off += 4
            rec["regs"] = np.frombuffer(raw, "<u4", n, off)
            off += 4 * n
            draws.append(rec)
        events.append(rec)
    return events, draws


def read_cp(path):
    raw = open(path, "rb").read()
    rec_size = 40 + 4 * NREG
    draws = []
    for off in range(0, len(raw) - rec_size + 1, rec_size):
        h = struct.unpack_from("<10I", raw, off)
        draws.append({"seq": h[1], "prim": h[2], "count": h[3], "src": h[4], "indexed": h[5],
                      "dma": h[6], "killed": h[7], "regs": np.frombuffer(raw, "<u4", NREG, off + 40)})
    return draws


events, gdraws = read_game(os.path.join(d, f"capture_game_{frame}.bin"))
cdraws = read_cp(os.path.join(d, f"capture_cp_{frame}.bin"))
print(f"game: {len(events)} calls, {len(gdraws)} draws | GPU: {len(cdraws)} draws")

# A draw entry can call another one (DrawIndexedVerticesUP -> DrawVerticesUP).
# The wrappers record after the original returns, so the inner draw comes
# right before the outer one: keep only the outer.
graph = json.load(open(os.path.join(HERE, "callgraph.json")))
kept = []
for i, g in enumerate(gdraws):
    nxt = gdraws[i + 1] if i + 1 < len(gdraws) else None
    if nxt is not None and g["name"] in graph.get(nxt["name"], {}).get("calls", {}):
        continue
    kept.append(g)
print(f"{len(gdraws) - len(kept)} inner draws (called by another draw entry) dropped")
gdraws = kept
print("game calls by entry:", collections.Counter(e["name"] for e in events).most_common(12))

# Registers the draw packet itself writes or that the CP changes on its own.
ignore = {0x21FC}  # VGT_DRAW_INITIATOR
mask = np.array([r not in ignore for r in REGS])

# Draw parameters of a game draw: (prim, count candidates).
def game_key(g):
    a = g["args"]
    if g["name"] == "sub_82593C50":  # DrawIndexedVertices(dev, prim, base, start, count)
        return a[1], {a[4]}
    return a[1], set(a[2:])  # other draw variants: count is one of the arguments


# Match in order: a game draw takes the next CP draw (within a window) with the
# same primitive type and count, the one with the fewest differing registers
# among them. Skipped CP draws are either replays of the previous match (same
# prim/count/index base: predicated tiling) or D3D-internal (clear, resolve).
ci = 0
matches = []
diff_regs = collections.Counter()
unmatched = []
skipped = []
WINDOW = 64
for gi, g in enumerate(gdraws):
    prim, counts = game_key(g)
    best = None
    for k in range(ci, min(ci + WINDOW, len(cdraws))):
        c = cdraws[k]
        if c["prim"] != prim or c["count"] not in counts:
            continue
        nd = int(np.count_nonzero((c["regs"] != g["regs"]) & mask))
        # The earliest candidate, unless one of the next few is identical (a
        # replayed tile of the previous draw can have the same parameters).
        if best is None:
            best = (k, nd)
            if nd == 0:
                break
        elif nd == 0 and k - best[0] <= 4:
            best = (k, nd)
            break
        elif k - best[0] > 4:
            break
    if best is None:
        unmatched.append(gi)
        continue
    k, nd = best
    prev = cdraws[matches[-1][1]] if matches else None
    for s in range(ci, k):
        c = cdraws[s]
        replay = prev is not None and (c["prim"], c["count"], c["dma"]) == (prev["prim"], prev["count"], prev["dma"])
        skipped.append((s, "replay" if replay else "internal"))
    matches.append((gi, k, nd))
    for r in REGS[(cdraws[k]["regs"] != g["regs"]) & mask]:
        diff_regs[int(r)] += 1
    ci = k + 1
for s in range(ci, len(cdraws)):
    skipped.append((s, "after last game draw"))

exact = sum(1 for m in matches if m[2] == 0)
print(f"matched {len(matches)} of {len(gdraws)} game draws, {exact} with identical registers; "
      f"unmatched game draws {len(unmatched)} (first {unmatched[:5]})")
print("CP draws without a game draw:", collections.Counter(s[1] for s in skipped))
internal = [cdraws[s] for s, kind in skipped if kind == "internal"]
print("  internal by (prim, count):", collections.Counter((c["prim"], c["count"]) for c in internal).most_common(6))
print("registers that differ (register: draws):")
for r, n in diff_regs.most_common(24):
    print(f"  0x{r:04X}: {n}")
by_entry = collections.Counter((gdraws[gi]["name"], nd == 0) for gi, k, nd in matches)
print("by draw entry (entry, identical):", sorted(by_entry.items()))

if len(sys.argv) > 3:
    lo = int(sys.argv[3])
    for m in matches:
        if lo <= m[0] < lo + 12:
            print('match', m, gdraws[m[0]]['name'], game_key(gdraws[m[0]])[0], sorted(game_key(gdraws[m[0]])[1])[:3], cdraws[m[1]]['prim'], cdraws[m[1]]['count'])

if os.environ.get("TAIL"):
    # Do the trailing CP draws replay earlier ones (a second predicated tile)?
    sig = lambda c: (c["prim"], c["count"], c["dma"])
    first_seen = {}
    for k, c in enumerate(cdraws[:ci]):
        first_seen.setdefault(sig(c), k)
    tail = cdraws[ci:]
    hits = [first_seen.get(sig(c)) for c in tail]
    print(f"trailing CP draws {len(tail)}: {sum(h is not None for h in hits)} have the parameters of an earlier draw")
    print("first trailing ->", hits[:12], "... last ->", hits[-12:])
    regdiff = [int(np.count_nonzero((tail[i]["regs"] != cdraws[h]["regs"]) & mask)) for i, h in enumerate(hits[:400]) if h is not None]
    print("registers differing from the earlier copy:", collections.Counter(regdiff).most_common(6))
    i0 = next(i for i, h in enumerate(hits) if h is not None)
    h0 = hits[i0]
    d = np.nonzero((tail[i0]["regs"] != cdraws[h0]["regs"]) & mask)[0]
    print("example differing registers:", [f"0x{REGS[j]:04X}:{cdraws[h0]['regs'][j]:08X}->{tail[i0]['regs'][j]:08X}" for j in d[:10]])
    print("remaining game draws:", [(g["name"], game_key(g)[0], sorted(game_key(g)[1])[:2]) for g in gdraws[len(matches):len(matches) + 6]])

if os.environ.get("EVENTS"):
    last = gdraws[len(matches) - 1]
    i = next(j for j, e in enumerate(events) if e is last)
    for e in events[i - 2:i + 40]:
        print(e["seq"], e["name"], " ".join(f"{a:08X}" for a in e["args"]), "DRAW" if "regs" in e else "")
