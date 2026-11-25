#!/usr/bin/env python3
"""Find instructions referencing a constant/immediate address in TESV.exe.

  python tools/arena_xref.py 0x1b418b0        # any operand encoding the address
  python tools/arena_xref.py 0x5500000 --imm  # only push/mov immediate
"""
import argparse
import os
import struct

import capstone
import pefile

EXE = os.environ.get("TESV_EXE", r"E:\skyr\TESV.exe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("value")
    ap.add_argument("--imm", action="store_true")
    args = ap.parse_args()
    val = int(args.value, 16) & 0xFFFFFFFF

    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()

    text_ranges = []
    for s in pe.sections:
        if s.Characteristics & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            text_ranges.append((base + s.VirtualAddress,
                                base + s.VirtualAddress + s.Misc_VirtualSize))

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    needle = struct.pack("<I", val)
    hits = []
    for lo, hi in text_ranges:
        blob = img[lo - base:hi - base]
        pos = blob.find(needle)
        while pos != -1:
            hits.append(lo + pos)
            pos = blob.find(needle, pos + 1)
    # Re-disassemble a window before each hit to identify the real instruction.
    seen = set()
    for hit in sorted(hits):
        start = max(hit - 16, 0)
        for i in md.disasm(img[start - base:hit - base + 8], start):
            if i.address <= hit < i.address + i.size:
                if args.imm:
                    ok = any(op.type == capstone.x86.X86_OP_IMM and (op.imm & 0xFFFFFFFF) == val
                             for op in i.operands)
                    if not ok:
                        break
                key = i.address
                if key not in seen:
                    seen.add(key)
                    print(f"{i.address:08x}  {i.bytes.hex():<24} {i.mnemonic:<8} {i.op_str}")
                break


if __name__ == "__main__":
    main()
