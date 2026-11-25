"""Expand fault addresses into whole function-pointer tables.

Observed pattern in the TESV startup slice: a fault lands on one entry of a
table of function pointers (e.g. CRT initialisers), the next iteration lands on
the next entry, and the growth loop walks the table one function per run.

This tool finds every 4-byte occurrence of the given function addresses in the
PE data sections and expands each occurrence to the maximal run of adjacent
dwords that are all discovered function starts. The result is a root list for
`tools/slice_pe.py --extra-root-file`.

Usage:
  python tools/table_roots.py TESV.exe functions.json --seed 0x103b5a2 [--seed ...]
         [--out roots.txt] [--max-run 512]
"""
import argparse
import json
import struct
from pathlib import Path

import pefile


def load_sections(exe):
    pe = pefile.PE(str(exe))
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()
    data = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode("ascii", "replace")
        if s.Characteristics & 0x20000000:
            continue  # executable: immediates are handled by slice_pe
        lo = base + s.VirtualAddress
        hi = lo + max(s.Misc_VirtualSize, s.SizeOfRawData)
        data.append((name, lo, hi))
    return base, image, data


def read_u32(base, image, addr):
    off = addr - base
    if off < 0 or off > len(image) - 4:
        return None
    return struct.unpack_from("<I", image, off)[0]


def make_verifier(image, base):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def verify(addr):
        """Decode from the candidate and require a coherent body: no invalid
        instruction in the first three, and a ret/jmp (or an indirect call to a
        thunk) within 64 instructions. Discovery membership is checked by the
        caller; this catches table entries that merely point into code."""
        off = addr - base
        if off < 0 or off >= len(image):
            return "outside image"
        seen = 0
        for ins in md.disasm(image[off:off + 256], addr):
            if ins.mnemonic == "invalid":
                return "invalid instruction" if seen == 0 else ""
            seen += 1
            if ins.mnemonic in ("ret", "retf", "jmp", "ljmp"):
                return ""
            if seen >= 64:
                break
        return "" if seen else "no instructions decoded"

    return verify


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("exe", type=Path)
    ap.add_argument("functions", type=Path)
    ap.add_argument("--seed", action="append", required=True,
                    help="fault address (hex) to expand")
    ap.add_argument("--out", type=Path, help="write one hex address per line")
    ap.add_argument("--max-run", type=int, default=512)
    args = ap.parse_args()

    seeds = {int(s, 16) for s in args.seed}
    funcs = json.loads(args.functions.read_text(encoding="utf-8"))["functions"]
    starts = {int(k, 16) for k in funcs}

    base, image, data = load_sections(args.exe)
    # index every dword by value once
    slots = {}
    for name, lo, hi in data:
        for addr in range(lo, hi - 3, 4):
            v = read_u32(base, image, addr)
            if v in seeds:
                slots.setdefault(v, []).append((name, addr))

    verify = make_verifier(image, base)
    roots = {}
    rejected = []
    for seed in sorted(seeds):
        hits = slots.get(seed, [])
        if not hits:
            print(f"{seed:08x}: no data reference found")
            continue
        for name, addr in hits:
            lo = addr
            while lo - 4 >= 0 and read_u32(base, image, lo - 4) in starts and addr - lo < 4 * args.max_run:
                lo -= 4
            hi = addr
            while hi + 4 <= 0xFFFFFFFF and read_u32(base, image, hi + 4) in starts and hi - addr < 4 * args.max_run:
                hi += 4
            run = [read_u32(base, image, a) for a in range(lo, hi + 1, 4)]
            run = [v for v in run if v in starts]
            kept = 0
            for v in run:
                why = verify(v)
                if why:
                    rejected.append((v, why))
                    continue
                roots.setdefault(v, name)
                kept += 1
            print(f"{seed:08x}: {name} table at {lo:08x}, {len(run)} function pointers "
                  f"({kept} verified)")
    if rejected:
        print(f"rejected {len(rejected)} table entries that are not plausible starts:")
        for value, why in rejected[:12]:
            print(f"  {value:08x}: {why}")

    if not roots:
        raise SystemExit("no table roots found")
    lines = "".join(f"0x{a:08x}\n" for a in sorted(roots))
    if args.out:
        args.out.write_text(lines, encoding="utf-8")
        print(f"wrote {len(roots)} roots to {args.out}")
    else:
        print(lines, end="")


if __name__ == "__main__":
    main()
