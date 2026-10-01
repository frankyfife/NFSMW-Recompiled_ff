"""Stage 2: resources of one captured frame (docs/NATIVE_RENDERER.md).

    python stage2_inventory.py <build dir> <frame>

Uses the matching of compare_capture.py and reports shaders (game object <->
microcode the GPU thread loaded), textures and vertex buffers (decoded from
the fetch constants), index buffers (game object <-> DMA address), render
target setups (passes) and resolves.
"""
import collections
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
argv = sys.argv[:3]
sys.argv = argv
src = open(os.path.join(HERE, "compare_capture.py"), encoding="utf-8").read()
src = src.split("\nif os.environ.get(\"TAIL\")")[0]
exec(compile(src, "compare_capture.py", "exec"))
objects = read_game.objects
reg_index = {int(r): i for i, r in enumerate(REGS)}


def reg(regs, r):
    return int(regs[reg_index[r]])


print("\n=== shaders")
pairs = {0: collections.defaultdict(collections.Counter), 1: collections.defaultdict(collections.Counter)}
for gi, k, nd in matches:
    g, c = gdraws[gi], cdraws[k]
    pairs[0][g["objects"][0]][c["vs"]] += 1
    pairs[1][g["objects"][1]][c["ps"]] += 1
for kind, name in ((0, "vertex"), (1, "pixel")):
    objs = pairs[kind]
    ambiguous = sum(1 for o in objs.values() if len(o) > 1)
    micro = {m for o in objs.values() for m in o}
    print(f"{name}: {len(objs)} game shader objects, {len(micro)} microcode programs, "
          f"{ambiguous} objects seen with more than one program")
    # Where in the object is the microcode address?
    offsets = collections.Counter()
    for obj, progs in objs.items():
        data = objects.get(obj, {}).get("bytes")
        if not data:
            continue
        (addr, dwords, h), _ = progs.most_common(1)[0]
        for off in range(0, len(data) - 3, 4):
            v = struct.unpack_from(">I", data, off)[0]
            if v and (v == addr or (v & 0x1FFFFFFF) == addr):
                offsets[off] += 1
    print(f"  object offsets holding the microcode address (offset: objects): {offsets.most_common(4)}")
    sizes = collections.Counter(m[1] for m in micro)
    print(f"  microcode sizes in dwords (most common): {sizes.most_common(5)}")

print("\n=== index buffers")
ib_ok = ib_bad = 0
ib_objs = collections.Counter()
for gi, k, nd in matches:
    g, c = gdraws[gi], cdraws[k]
    if not c["indexed"] or g["name"] != "sub_82593C50":
        continue
    ib = g["objects"][3]
    ib_objs[ib] += 1
    data = objects.get(ib, {}).get("bytes")
    if not data:
        continue
    base = struct.unpack_from(">I", data, 12)[0]
    start = g["args"][3]
    expected = (base + 2 * start) & 0x1FFFFFFF
    if expected == c["dma"] or ((base + 4 * start) & 0x1FFFFFFF) == c["dma"]:
        ib_ok += 1
    else:
        ib_bad += 1
print(f"{len(ib_objs)} index buffer objects; DMA address = object+12 + start*index size for "
      f"{ib_ok} draws, different for {ib_bad}")

print("\n=== textures and vertex buffers (fetch constants at the draws)")
textures = {}
vbs = collections.Counter()
formats = collections.Counter()
for gi, k, nd in matches:
    regs = gdraws[gi]["regs"]
    for slot in range(32):
        dw = [reg(regs, 0x4800 + 6 * slot + j) for j in range(6)]
        t = dw[0] & 3
        if t == 2:
            base = (dw[1] >> 12) << 12
            fmt = dw[1] & 0x3F
            w = (dw[2] & 0x1FFF) + 1
            h = ((dw[2] >> 13) & 0x1FFF) + 1
            dim = (dw[5] >> 9) & 3
            tiled = dw[0] >> 31
            key = (base, fmt, w, h, dim)
            if key not in textures:
                textures[key] = tiled
                formats[fmt] += 1
        elif t == 3:
            for j in range(3):
                d0, d1 = dw[2 * j], dw[2 * j + 1]
                if d0 & 3 == 3:
                    vbs[((d0 & ~3) & 0x1FFFFFFF, (d1 >> 2) & 0xFFFFFF)] += 1
FMT = {2: "8", 6: "8_8_8_8", 10: "8_8", 18: "DXT1", 19: "DXT2_3", 20: "DXT4_5", 4: "5_6_5",
       15: "4_4_4_4", 26: "16_16_16_16", 32: "16_16_16_16_FLOAT", 31: "16_16_FLOAT",
       7: "2_10_10_10", 16: "10_11_11", 17: "11_11_10", 22: "24_8", 23: "24_8_FLOAT",
       30: "16_FLOAT", 36: "32_FLOAT", 25: "16_16", 33: "32"}
print(f"{len(textures)} distinct textures referenced by fetch slots in this frame "
      f"({sum(textures.values())} tiled)")
print("  by format:", [(FMT.get(f, f), n) for f, n in formats.most_common(10)])
sizes = collections.Counter((w, h) for (_, _, w, h, _) in textures)
print("  by size:", sizes.most_common(8))
print(f"{len(vbs)} distinct vertex buffer ranges")

print("\n=== render target setups (passes)")
passes = []
for gi, k, nd in matches:
    regs = gdraws[gi]["regs"]
    key = tuple(reg(regs, r) for r in (0x2000, 0x2001, 0x2002, 0x2003))
    if not passes or passes[-1][0] != key:
        passes.append([key, 0])
    passes[-1][1] += 1
print(f"{len(passes)} runs of draws with the same RB_SURFACE_INFO/COLOR_INFO/DEPTH_INFO/COLOR1_INFO")
for key, n in passes[:40]:
    surf, color, depth, color1 = key
    print(f"  {n:5d} draws  pitch {surf & 0x3FFF:5d} msaa {(surf >> 16) & 3}  color base {color & 0xFFF:4d} "
          f"fmt {(color >> 16) & 0xF:2d}  depth base {depth & 0xFFF:4d} fmt {(depth >> 16) & 1}  "
          f"color1 {color1:08X}")

print("\n=== resolves (sub_82592538) and other frame events")
ev = collections.Counter(e["name"] for e in events)
for name in ("sub_82592538", "sub_8259A500", "sub_825991C0", "sub_82597840", "sub_82597DA0",
             "sub_82599680", "sub_825992F0", "sub_82598868"):
    print(f"  {name}: {ev.get(name, 0)}")
for e in [e for e in events if e["name"] == "sub_82592538"][:30]:
    print("   resolve", " ".join(f"{a:08X}" for a in e["args"][1:]))
