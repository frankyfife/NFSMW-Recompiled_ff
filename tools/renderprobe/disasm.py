"""Prints the PPC listing of guest functions from the generated code (the
instruction comments), one instruction per line, with its address.

    python disasm.py sub_8259C150 [sub_... ...]
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, "..", "..", "app", "generated", "default")
wanted = set(sys.argv[1:])
re_def = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
re_label = re.compile(r"^loc_([0-9A-F]{8}):")
for path in sorted(glob.glob(os.path.join(GEN, "nfsmw_recomp.*.cpp"))):
    cur = None
    addr = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re_def.match(line)
        if m:
            cur = m.group(1) if m.group(1) in wanted else None
            if cur:
                addr = int(cur[4:], 16)
                print(f"\n== {cur}")
            continue
        if not cur:
            continue
        lm = re_label.match(line)
        if lm:
            addr = int(lm.group(1), 16)
            continue
        s = line.strip()
        if s.startswith("// ") and not s.startswith("// PPC") :
            print(f"  {addr:08X}  {s[3:]}")
            addr += 4
