"""Disassemble the real guest image around the buffered-window translate.

The on-disk TESV.exe differs from the image the runtime actually executes, so
this reads the IR container input (`<IIII` magic/lo/size/entry header written
by tools/ir_pack.py) and disassembles at offset `16 + VA - lo`.

Usage:
  python tools/a43f_dis.py                     # 0xa43f80..0xa44050
  python tools/a43f_dis.py 0xac6c10 0xac6d60   # caller
"""
import struct
import sys
from pathlib import Path

IMAGE = Path(r"E:\census_ir2\ir_container\_work\ir_container_input.bin")


def main(argv):
    start = int(argv[1], 16) if len(argv) > 1 else 0xA43F80
    end = int(argv[2], 16) if len(argv) > 2 else 0xA44050
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    except ImportError:
        sys.exit("capstone missing (use the repo .venv)")
    data = IMAGE.read_bytes()
    magic, lo, size, entry = struct.unpack("<IIII", data[:16])
    if magic != 0x49524D31:
        sys.exit("not an IRM1 container input")
    code = data[16:16 + size]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for insn in md.disasm(code[start - lo:end - lo], start):
        print("%08x: %-24s %-6s %s" % (insn.address, insn.bytes.hex(),
                                       insn.mnemonic, insn.op_str))


if __name__ == "__main__":
    main(sys.argv)
