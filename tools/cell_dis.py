"""Disassemble the retail guest image for cell / streaming tracing.

Offset rule: `off = 16 + VA - lo`, where `lo` is the image base from the
`<IIII` header written by tools/ir_pack.py (see docs/CELL_TRACE.md).

Usage:
  python tools/cell_dis.py 0x4397e0                    # whole owning function
  python tools/cell_dis.py 0x4397e0 0x439e80           # VA range
  python tools/cell_dis.py 0x4397e0 --off 0x6c         # only [reg+0x6c] accesses
  python tools/cell_dis.py 0x4397e0 --calls            # only call/jmp lines
  python tools/cell_dis.py 0x4397e0 --off 0x6c --calls # both filters
"""
import json
import os
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
import capstone

IMAGE = os.environ.get(
    "CELL_IMAGE", r"E:\census_ir2\ir_container\_work\ir_container_input.bin")
FUNCS = os.environ.get(
    "CELL_FUNCS", r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json")
BASE = 0x401000
OFF16 = 16

_md = Cs(CS_ARCH_X86, CS_MODE_32)
_md.detail = True

with open(IMAGE, "rb") as _f:
    IMG = _f.read()
MAGIC, LO, SIZE, _ENTRY = struct.unpack("<IIII", IMG[:16])

_FUNCS_MAP = None


def funcs():
    global _FUNCS_MAP
    if _FUNCS_MAP is None:
        try:
            with open(FUNCS, encoding="utf-8") as f:
                j = json.load(f)
            _FUNCS_MAP = {int(k, 16): v for k, v in j["functions"].items()}
        except (OSError, ValueError, KeyError):
            _FUNCS_MAP = {}
    return _FUNCS_MAP


def own(va):
    for start, ranges in funcs().items():
        for a, b in ranges:
            if a <= va < b:
                return start
    return None


def get(va, n=1):
    off = OFF16 + va - BASE
    return IMG[off:off + n]


def dis(va, end=None, only_off=None, only_calls=False):
    f = own(va)
    ranges = funcs().get(f, [[va, end or va + 0x100]])
    if end is None:
        end = ranges[-1][1]
    out = []
    for a, b in ranges:
        b = min(b, end)
        if b <= a:
            continue
        start = max(a, va)
        for insn in _md.disasm(get(start, b - start), start):
            if only_calls and insn.mnemonic not in ("call", "jmp"):
                continue
            if only_off is not None:
                hit = False
                for op in insn.operands:
                    if op.type == capstone.x86.X86_OP_MEM and \
                            (op.mem.disp & 0xffffffff) == (only_off & 0xffffffff):
                        hit = True
                if not hit:
                    continue
            out.append(insn)
    return f, out


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if MAGIC != 0x49524D31:
        sys.exit("not an IRM1 container input (magic=%08x)" % MAGIC)
    va = int(argv[1], 16)
    end = None
    only_off = None
    only_calls = False
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--off":
            only_off = int(argv[i + 1], 16)
            i += 2
            continue
        if a == "--calls":
            only_calls = True
        elif not a.startswith("--"):
            end = int(a, 16)
        i += 1
    f, out = dis(va, end, only_off, only_calls)
    print("=== func %s (image %08x..%08x) ===" % (
        f and "%08x" % f, LO, LO + SIZE))
    for insn in out:
        print("%08x  %-20s %-6s %s" % (insn.address, insn.bytes.hex(),
                                       insn.mnemonic, insn.op_str))


if __name__ == "__main__":
    main(sys.argv)
