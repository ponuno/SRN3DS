"""Find direct call/jmp xrefs to a VA inside the IR container code image.

Usage: python tools/world_xref.py 0x6982c0 [0x6a0ae0 ...]
"""
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

IMAGE = Path(r"E:\census_ir2\ir_container\_work\ir_container_input.bin")


def main(argv):
    targets = {int(a, 16) for a in argv[1:]}
    if not targets:
        sys.exit(__doc__)
    d = IMAGE.read_bytes()
    magic, lo, size, entry = struct.unpack("<IIII", d[:16])
    code = d[16:16 + size]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    found = {t: [] for t in targets}
    for insn in md.disasm(code, lo):
        if insn.mnemonic in ("call", "jmp") and insn.op_str.startswith("0x"):
            try:
                tgt = int(insn.op_str, 16)
            except ValueError:
                continue
            if tgt in found:
                found[tgt].append(insn.address)
    for t in sorted(found):
        print("%08x <- %s" % (t, ", ".join("%08x" % a for a in found[t]) or "(none)"))


if __name__ == "__main__":
    main(sys.argv)
