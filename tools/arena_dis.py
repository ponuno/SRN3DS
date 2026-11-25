#!/usr/bin/env python3
"""Disassemble an address range of the real TESV.exe (measurement helper).

Used by docs/ARENA.md. Reads the PE from disk, maps it like the loader, and
prints capstone disassembly with IAT-slot annotations from functions.json.

  python tools/arena_dis.py 0x687e20 0x200
  python tools/arena_dis.py 0xa48d60 --until-ret 0xa48c80
"""
import argparse
import json
import os

import capstone
import pefile

EXE = os.environ.get("TESV_EXE", r"E:\skyr\TESV.exe")
FUNCS = os.environ.get("TESV_FUNCS", r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json")


def load():
    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    return base, pe.get_memory_mapped_image()


def disasm(start, length, md, img, base, iat):
    end = start + length
    code = img[start - base:end - base]
    for i in md.disasm(code, start):
        note = ""
        for op in i.operands:
            if op.type == capstone.x86.X86_OP_MEM and not op.mem.base and not op.mem.index:
                slot = op.mem.disp & 0xFFFFFFFF
                if slot in iat:
                    note = "   ; " + iat[slot]
        print(f"{i.address:08x}  {i.bytes.hex():<20} {i.mnemonic:<8} {i.op_str}{note}")
        if i.mnemonic in ("ret", "retn") and i.address + i.size >= end:
            break


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("start", help="hex address")
    ap.add_argument("length", nargs="?", default="0x200")
    ap.add_argument("--count", type=int, default=0, help="stop after N instructions")
    ap.add_argument("--until-ret", action="store_true", help="stop at the first ret")
    args = ap.parse_args()

    base, img = load()
    iat = {}
    if os.path.exists(FUNCS):
        data = json.load(open(FUNCS, encoding="utf-8"))
        iat = {int(k, 16): v for k, v in data.get("iat", {}).items()}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    start = int(args.start, 16)
    length = int(args.length, 16)
    code = img[start - base:start - base + length]
    n = 0
    seen_ret = False
    for i in md.disasm(code, start):
        note = ""
        for op in i.operands:
            if op.type == capstone.x86.X86_OP_MEM and not op.mem.base and not op.mem.index:
                slot = op.mem.disp & 0xFFFFFFFF
                if slot in iat:
                    note = "   ; " + iat[slot]
        print(f"{i.address:08x}  {i.bytes.hex():<20} {i.mnemonic:<8} {i.op_str}{note}")
        n += 1
        if args.count and n >= args.count:
            break
        if args.until_ret and i.mnemonic in ("ret", "retn"):
            seen_ret = True
            break
    if args.until_ret and not seen_ret:
        print("; no ret within range")


if __name__ == "__main__":
    main()
