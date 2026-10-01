"""How does a D3D shader object point to its microcode? Prints, for a few
vertex/pixel shader objects, the object address, the microcode address the
GPU thread loaded (IM_LOAD) and the object's dwords that look like addresses.

    python shader_objects.py <build dir> <frame>
"""
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv = sys.argv[:3]
src = open(os.path.join(HERE, "compare_capture.py"), encoding="utf-8").read()
exec(compile(src.split("\nif os.environ.get(\"TAIL\")")[0], "compare_capture.py", "exec"))
objects = read_game.objects
seen = {0: {}, 1: {}}
for gi, k, nd in matches:
    g, c = gdraws[gi], cdraws[k]
    seen[0].setdefault(g["objects"][0], c["vs"])
    seen[1].setdefault(g["objects"][1], c["ps"])
for kind in (0, 1):
    print("== vertex" if kind == 0 else "== pixel")
    for obj, (addr, dwords, h) in list(seen[kind].items())[:4]:
        data = objects[obj]["bytes"]
        words = struct.unpack(">64I", data[:256])
        print(f"object {obj:08X}  microcode at {addr:08X}, {dwords} dwords (object - microcode = {obj - addr:+X})")
        for i, w in enumerate(words):
            if w and (abs(w - addr) < 0x100000 or abs((w & 0x1FFFFFFF) - addr) < 0x100000
                      or abs(w - obj) < 0x10000 or w == dwords or w == dwords * 4):
                print(f"   +{4 * i:3d}: {w:08X}")
