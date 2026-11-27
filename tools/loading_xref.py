#!/usr/bin/env python3
"""One-pass xref index for the retail image (used by docs/LOADING_MAP.md).

Disassembles every discovered function block once and records:
  * direct call/jmp edges (target -> [site, ...])
  * immediate operands equal to a chosen address (push/cmp/mov/lea/...)
  * memory-operand references to a chosen address (`[0xADDR]` form)

Usage:
    python tools/loading_xref.py --build            # writes %TEMP%/lm/xref.json
    python tools/loading_xref.py 0x6f3e20 0x1b39124 ...
    python tools/loading_xref.py --write 0x1b39124  # stores to that global
"""
import json
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loading_map as L

CACHE = os.path.join(os.environ.get("TEMP", "/tmp"), "lm", "xref.json")
MD = L._md


def build():
    calls = {}
    imms = {}
    mems = {}
    writes = {}
    for start, ranges in L.FUNCS_MAP.items():
        for a, b in ranges:
            code = L.get(a, b - a)
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
                        if i.mnemonic.startswith("mov") and ops[0].type == capstone.x86.X86_OP_MEM:
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
    # show the instruction text and its owning function
    own = L.own(at)
    code = L.get(at, 16)
    txt = ""
    for i in MD.disasm(code, at):
        txt = i.mnemonic + " " + i.op_str
        break
    return "%08x  %-46s [fn %s]" % (at, txt, own and hex(own))


def main(argv):
    if "--build" in argv:
        build()
        return
    db = load()
    kind = "calls"
    for a in argv[1:]:
        if a in ("--imms", "--mems", "--writes"):
            kind = a[2:]
            continue
        v = hex(int(a, 16))
        print("== %s %s" % (kind, v))
        for at in db[kind].get(v, []):
            print("   " + line(at))


if __name__ == "__main__":
    main(sys.argv)
