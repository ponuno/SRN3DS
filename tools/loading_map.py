#!/usr/bin/env python3
"""Static helper for docs/LOADING_MAP.md: disassemble and xref the retail image.

The analysis image is the raw PE *image* (headers included) at
`E:\\census_ir2\\ir_container\\_work\\ir_container_input.bin`; guest VAs map as
`off = 16 + VA - 0x401000`.  `functions.json` supplies the block ranges of each
discovered function so we disassemble exactly the function body.

Usage:
    python tools/loading_map.py dis  0x696810 [0x691030 ...]   # whole functions
    python tools/loading_map.py near 0x86f067                  # window around a VA
    python tools/loading_map.py xref 0x6f3e20 0x6f3ea0         # call/jmp/imm refs
    python tools/loading_map.py abs  0x6f3e20                  # function that owns VA
"""
import json
import os
import sys

import capstone

IMAGE = os.environ.get(
    "LOADING_IMAGE", r"E:\census_ir2\ir_container\_work\ir_container_input.bin")
FUNCS = os.environ.get(
    "LOADING_FUNCS", r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json")
BASE = 0x401000
OFF16 = 16

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
_md.detail = True

with open(IMAGE, "rb") as _f:
    IMG = _f.read()

with open(FUNCS, encoding="utf-8") as _f:
    _j = json.load(_f)
FUNCS_MAP = {int(k, 16): v for k, v in _j["functions"].items()}
IAT = {int(k, 16): v for k, v in _j.get("iat", {}).items()}


def get(va, n=1):
    off = OFF16 + va - BASE
    return IMG[off:off + n]


def own(va):
    for start, ranges in FUNCS_MAP.items():
        for a, b in ranges:
            if a <= va < b:
                return start
    return None


def dis(va, count=0, mark=None):
    own_start = own(va)
    ranges = FUNCS_MAP.get(own_start, [[va, va + 0x100]])
    lines = []
    for a, b in ranges:
        if count and a >= va + count:
            break
        code = get(a, b - a)
        for i in _md.disasm(code, a):
            note = ""
            for op in i.operands:
                if op.type == capstone.x86.X86_OP_MEM and not op.mem.base \
                        and not op.mem.index and op.mem.disp in IAT:
                    note = " ; " + IAT[op.mem.disp]
                if op.type == capstone.x86.X86_OP_IMM:
                    tgt = own(op.imm) if 0x401000 <= op.imm < 0x1200000 else None
                    if tgt:
                        note += " ; fn=%x" % tgt
            m = "   <== " if mark and i.address == mark else ""
            lines.append("%08x  %-8s %s%s%s" % (i.address, i.mnemonic, i.op_str, note, m))
    return own_start, lines


def xrefs(target):
    out = []
    for start, ranges in FUNCS_MAP.items():
        for a, b in ranges:
            code = get(a, b - a)
            for i in _md.disasm(code, a):
                hit = None
                if i.mnemonic in ("call", "jmp") and i.operands and \
                        i.operands[0].type == capstone.x86.X86_OP_IMM and \
                        i.operands[0].imm == target:
                    hit = i.mnemonic
                elif i.mnemonic == "push" and i.operands and \
                        i.operands[0].type == capstone.x86.X86_OP_IMM and \
                        i.operands[0].imm == target:
                    hit = "push-imm"
                if hit:
                    out.append((start, i.address, hit, i.mnemonic + " " + i.op_str))
    return out


def main(argv):
    mode = argv[1]
    if mode == "dis":
        for a in argv[2:]:
            va = int(a, 16)
            start, lines = dis(va, mark=va)
            print("=== func %x (target %x) ===" % (start, va))
            print("\n".join(lines))
    elif mode == "near":
        va = int(argv[2], 16)
        code = get(va - 0x60, 0x80)
        for i in _md.disasm(code, va - 0x60):
            print("%08x  %-8s %s" % (i.address, i.mnemonic, i.op_str))
    elif mode == "xref":
        for a in argv[2:]:
            va = int(a, 16)
            print("=== xrefs to %x (owner %s) ===" % (va, own(va) and hex(own(va))))
            for start, at, kind, text in xrefs(va):
                print("  %08x  %-8s %s   [in %x]" % (at, kind, text, start))
    elif mode == "abs":
        for a in argv[2:]:
            va = int(a, 16)
            print("%x -> fn %s ranges %s" % (va, own(va) and hex(own(va)),
                                             FUNCS_MAP.get(own(va))))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
