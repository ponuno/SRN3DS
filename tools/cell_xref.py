"""One-pass xref index for cell / streaming work (retail image, docs/CELL_TRACE.md).

Disassembles every function block listed in functions.json once and records:
  * direct call/jmp edges                (--calls VA)
  * immediates equal to a VA             (--imms 0x12e3560)
  * memory operands `[0xADDR]`           (--mems 0x1b2e864)
  * stores to `[0xADDR]`                 (--writes 0x12e3560)

Usage:
  python tools/cell_xref.py --build                 # %TEMP%/cell/xref.json
  python tools/cell_xref.py 0x4397e0 0x433980       # calls (default kind)
  python tools/cell_xref.py --mems 0x12e3560 0x1ba7738
  python tools/cell_xref.py --writes 0x1b2e864
"""
import json
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cell_dis as C

CACHE = os.path.join(os.environ.get("TEMP", "/tmp"), "cell", "xref.json")
MD = C._md


def build():
    calls, imms, mems, writes = {}, {}, {}, {}
    for _start, ranges in C.funcs().items():
        for a, b in ranges:
            code = C.get(a, b - a)
            for i in MD.disasm(code, a):
                ops = i.operands
                if i.mnemonic in ("call", "jmp") and ops and \
                        ops[0].type == capstone.x86.X86_OP_IMM:
                    calls.setdefault(ops[0].imm, []).append(i.address)
                for op in ops:
                    if op.type == capstone.x86.X86_OP_IMM and 0x1000000 <= op.imm < 0x1c00000:
                        imms.setdefault(op.imm, []).append(i.address)
                    elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and \
                            op.mem.index == 0 and 0x1000000 <= (op.mem.disp & 0xffffffff) < 0x1c00000:
                        d = op.mem.disp & 0xffffffff
                        mems.setdefault(d, []).append(i.address)
                        if i.mnemonic.startswith("mov") and \
                                ops[0].type == capstone.x86.X86_OP_MEM:
                            writes.setdefault(d, []).append(i.address)
    out = {"calls": {hex(k): v for k, v in calls.items()},
           "imms": {hex(k): v for k, v in imms.items()},
           "mems": {hex(k): v for k, v in mems.items()},
           "writes": {hex(k): v for k, v in writes.items()}}
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE, "w", encoding="utf-8") as f:
        json.dump(out, f)
    print("wrote", CACHE, len(calls), len(imms), len(mems), len(writes))


def load():
    with open(CACHE, encoding="utf-8") as f:
        return json.load(f)


def line(at):
    f = C.own(at)
    txt = ""
    for i in MD.disasm(C.get(at, 16), at):
        txt = i.mnemonic + " " + i.op_str
        break
    return "%08x  %-46s [fn %s]" % (at, txt, f and "%08x" % f)


def main(argv):
    if "--build" in argv:
        build()
        return
    db = load()
    kind = "calls"
    for a in argv[1:]:
        if a in ("--calls", "--imms", "--mems", "--writes"):
            kind = a[2:]
            continue
        v = hex(int(a, 16))
        print("== %s %s" % (kind, v))
        hits = db[kind].get(v, [])
        for at in hits[:80]:
            print("   " + line(at))
        if len(hits) > 80:
            print("   ... %d more" % (len(hits) - 80))


if __name__ == "__main__":
    main(sys.argv)
