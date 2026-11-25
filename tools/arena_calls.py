#!/usr/bin/env python3
"""Report call sites and the immediately-preceding argument pushes for a target.

  python tools/arena_calls.py 0xa4dcf0
  python tools/arena_calls.py 0xa4dcf0 0xa4db10
"""
import argparse
import os

import capstone
import pefile

EXE = os.environ.get("TESV_EXE", r"E:\skyr\TESV.exe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("targets", nargs="+")
    ap.add_argument("--window", type=int, default=32, help="instructions of context")
    args = ap.parse_args()
    targets = {int(t, 16) for t in args.targets}

    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    md.skipdata = True

    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):
            continue
        lo = base + s.VirtualAddress
        hi = lo + s.Misc_VirtualSize
        code = img[lo - base:hi - base]
        insns = list(md.disasm(code, lo))
        for i, ins in enumerate(insns):
            if ins.mnemonic != "call" or not ins.op_str.startswith("0x"):
                continue
            try:
                tgt = int(ins.op_str, 16)
            except ValueError:
                continue
            if tgt not in targets:
                continue
            print("=== call %x at %x" % (tgt, ins.address))
            for prev in insns[max(0, i - args.window):i + 1]:
                print("   %08x  %-8s %s" % (prev.address, prev.mnemonic, prev.op_str))


if __name__ == "__main__":
    main()
