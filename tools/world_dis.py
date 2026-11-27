"""Disassemble the real guest image (IR container input) for world-entry work.

Offset rule: `16 + VA - lo`, where lo is the image base recorded in the
`<IIII` header written by tools/ir_pack.py.

Usage:
  python tools/world_dis.py 0x6a0ae0 0x6a0c40
  python tools/world_dis.py 0x6982c0 0x698400 --raw   # no detail (strings kept)
"""
import struct
import sys
from pathlib import Path

IMAGE = Path(r"E:\census_ir2\ir_container\_work\ir_container_input.bin")


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    start = int(argv[1], 16)
    end = int(argv[2], 16) if len(argv) > 2 and not argv[2].startswith("--") else start + 0x100
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    except ImportError:
        sys.exit("capstone missing (use the repo .venv)")
    data = IMAGE.read_bytes()
    magic, lo, size, entry = struct.unpack("<IIII", data[:16])
    if magic != 0x49524D31:
        sys.exit("not an IRM1 container input (magic=%08x)" % magic)
    code = data[16:16 + size]
    if start < lo or end > lo + size:
        sys.exit("range outside image: lo=%08x hi=%08x" % (lo, lo + size))
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for insn in md.disasm(code[start - lo:end - lo], start):
        print("%08x: %-24s %-6s %s" % (insn.address, insn.bytes.hex(),
                                       insn.mnemonic, insn.op_str))


if __name__ == "__main__":
    main(sys.argv)
