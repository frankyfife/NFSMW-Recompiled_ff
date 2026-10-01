"""Prints the first game draws next to the CP draws they matched exactly, to
see which call argument becomes which draw packet field.

    python draw_args.py <dir> <frame> [first] [count]
"""
import os
import sys

first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
count = int(sys.argv[4]) if len(sys.argv) > 4 else 14
cp_first = int(sys.argv[5]) if len(sys.argv) > 5 else first
sys.argv = sys.argv[:3]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "compare_capture.py")).read().split("# Registers the draw packet")[0])
for i in range(first, first + count):
    g = gdraws[i]
    print(g["name"], " ".join(f"{a:08X}" for a in g["args"][1:]))
for k in range(cp_first, cp_first + count + 2):
    c = cdraws[k]
    print(f"  cp {k}: prim {c['prim']} count {c['count']} indexed {c['indexed']} dma {c['dma']:08X} killed {c.get('killed')}")
