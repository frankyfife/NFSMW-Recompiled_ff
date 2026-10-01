"""Stage 3: what the resolves and clears of a frame do (docs/NATIVE_RENDERER.md).

    python resolves.py <build dir> <frame>

Resolves reach the GPU as RECTLIST draws with RB_MODECONTROL = copy. For each
of them this prints the source (EDRAM surface, color or depth, MSAA, sample
select), the clears and the destination (address, pitch, format), and which
draws of the frame later sample the destination as a texture.
"""
import collections
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv = sys.argv[:3]
src = open(os.path.join(HERE, "compare_capture.py"), encoding="utf-8").read()
exec(compile(src.split("\nif os.environ.get(\"TAIL\")")[0], "compare_capture.py", "exec"))
reg_index = {int(r): i for i, r in enumerate(REGS)}


def reg(regs, r):
    return int(regs[reg_index[r]])


COLOR_FMT = {0: "8_8_8_8", 1: "8_8_8_8_GAMMA", 2: "2_10_10_10", 3: "2_10_10_10_FLOAT", 4: "16_16",
             5: "16_16_16_16", 6: "16_16_FLOAT", 7: "16_16_16_16_FLOAT", 10: "2_10_10_10_AS_10_10_10_10",
             12: "2_10_10_10_FLOAT_AS_16_16_16_16", 14: "32_FLOAT", 15: "32_32_FLOAT"}
TEX_FMT = {2: "8", 4: "5_6_5", 6: "8_8_8_8", 7: "2_10_10_10", 10: "8_8", 18: "DXT1", 19: "DXT2_3",
           20: "DXT4_5", 22: "24_8", 23: "24_8_FLOAT", 25: "16_16", 26: "16_16_16_16", 31: "16_16_FLOAT",
           32: "16_16_16_16_FLOAT", 36: "32_FLOAT", 33: "32"}
SAMPLE = ["s0", "s1", "s2", "s3", "s01", "s23", "s0123", "?"]


def texture_bases(regs):
    """Texture fetch constants of a draw: base address -> (format, width, height)."""
    out = {}
    for slot in range(32):
        dw = [reg(regs, 0x4800 + 6 * slot + j) for j in range(6)]
        if dw[0] & 3 == 2:
            out[(dw[1] >> 12) << 12] = (dw[1] & 0x3F, (dw[2] & 0x1FFF) + 1, ((dw[2] >> 13) & 0x1FFF) + 1)
    return out


modes = collections.Counter(reg(c["regs"], 0x2208) & 7 for c in cdraws)
print("RB_MODECONTROL edram mode over all CP draws:", dict(modes))

copies = [(k, c) for k, c in enumerate(cdraws) if reg(c["regs"], 0x2208) & 7 == 6]
print(f"{len(copies)} copy (resolve) draws\n")
resolves = []
for k, c in copies:
    r = c["regs"]
    surf, cinfo, dinfo = reg(r, 0x2000), reg(r, 0x2001), reg(r, 0x2002)
    ctl = reg(r, 0x2318)
    dest, pitch, info = reg(r, 0x2319), reg(r, 0x231A), reg(r, 0x231B)
    src_sel = ctl & 7
    if src_sel == 4:
        source = f"depth  edram {dinfo & 0xFFF:4d}"
    else:
        ci = cinfo if src_sel == 0 else reg(r, 0x2003 + src_sel - 1)
        source = f"color{src_sel} edram {ci & 0xFFF:4d} {COLOR_FMT.get((ci >> 16) & 0xF, (ci >> 16) & 0xF)}"
    cmd = (ctl >> 20) & 3
    clears = ("C" if ctl & 0x100 else "-") + ("D" if ctl & 0x200 else "-")
    fmt = (info >> 7) & 0x3F
    resolves.append({"k": k, "dest": dest, "fmt": fmt, "pitch": pitch & 0x3FFF, "height": (pitch >> 16) & 0x3FFF})
    print(f"cp#{k:5d} pitch {surf & 0x3FFF:4d} msaa {(surf >> 16) & 3} {source:32s} {SAMPLE[(ctl >> 4) & 7]:5s} "
          f"cmd {cmd} clear {clears} -> {dest:08X} {pitch & 0x3FFF:4d}x{(pitch >> 16) & 0x3FFF:<4d} "
          f"{TEX_FMT.get(fmt, COLOR_FMT.get(fmt, fmt))} endian {info & 7} "
          f"clearcol {reg(r, 0x231E):08X} cleardepth {reg(r, 0x231D):08X}")

print("\n=== who samples the resolve destinations")
uses = collections.defaultdict(list)
for k, c in enumerate(cdraws):
    for base in texture_bases(c["regs"]):
        uses[base].append(k)
dests = collections.OrderedDict()
for rv in resolves:
    dests.setdefault(rv["dest"] & ~0xFFF, []).append(rv["k"])
for d, ks in dests.items():
    later = [u for u in uses.get(d, []) if u > ks[0]]
    print(f"  {d:08X}: resolved at cp {ks[:6]}{'...' if len(ks) > 6 else ''} ({len(ks)}x); "
          f"sampled by {len(later)} later CP draws, first {later[:3]}")

# Textures sampled in the frame that are not static: base addresses written by
# a resolve in this frame vs. not.
all_tex = set(uses)
print(f"\n{len(all_tex)} texture bases sampled; {len(all_tex & set(dests))} of them are resolve destinations")
