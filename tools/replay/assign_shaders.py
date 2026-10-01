"""Which shaders the GPU ran for each game draw of a captured frame.

    python assign_shaders.py <build dir> <frame>

The game side cannot always tell which microcode runs (the D3D library patches
vertex shaders for the vertex declaration and links them to the pixel shader,
partly in scratch memory). The GPU side knows it per draw (capture_cp_N.bin,
microcode in capture_ucode_N.bin). This matches the draws of both sides like
tools/renderprobe/compare_capture.py and writes capture_shaders_N.bin for
nfsmw_replay: per game draw u32 sequence, u64 vertex shader hash, u64 pixel
shader hash (0 = none). Inner draws (BeginVertices inside DrawVerticesUP) get
the shaders of their outer draw.
"""
import contextlib
import io
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "..", "renderprobe")
d, frame = sys.argv[1], int(sys.argv[2])
sys.argv = [sys.argv[0], d, str(frame)]
src = open(os.path.join(PROBE, "compare_capture.py"), encoding="utf-8").read()
src = src.split("\nif os.environ.get(\"TAIL\")")[0]
__file__ = os.path.join(PROBE, "compare_capture.py")
with contextlib.redirect_stdout(io.StringIO()):
    exec(compile(src, "compare_capture.py", "exec"))

_, all_draws = read_game(os.path.join(d, f"capture_game_{frame}.bin"))
by_seq = {g["seq"]: i for i, g in enumerate(all_draws)}
out = []
for gi, k, nd in matches:
    g, c = gdraws[gi], cdraws[k]
    entry = (c["vs"][2], c["ps"][2] if c["ps"][1] else 0)
    out.append((g["seq"], *entry))
    i = by_seq[g["seq"]]
    if i > 0 and all_draws[i - 1]["name"] == "sub_825932D8" and g["name"] == "sub_82593588":
        out.append((all_draws[i - 1]["seq"], *entry))
with open(os.path.join(d, f"capture_shaders_{frame}.bin"), "wb") as f:
    for seq, vs, ps in sorted(out):
        f.write(struct.pack("<IQQ", seq, vs, ps))
print(f"{len(out)} draws with GPU shaders ({len(matches)} matched of {len(gdraws)})")
