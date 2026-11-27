#!/usr/bin/env python3
"""Inventory real MMX instruction boundaries in the TESV discovery graph.

Unlike a byte scan, this replays the basic-block ranges in functions.json with
Capstone.  That excludes pointer tables, strings and instruction immediates
which happen to contain 0f 6f/0f 77/etc.  Shared blocks are deduplicated by
instruction address and bytes, while the owning-function sets are retained.

Usage:
  python tools/mmx_audit.py --json build_host/mmx_audit.json
  python tools/mmx_audit.py --exe E:/skyr/TESV.exe \
      --functions E:/Skyrim_N3DS_build/analysis/tesv/functions.json
"""
import argparse
import collections
import csv
import json
from pathlib import Path

import capstone
from capstone import x86

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = Path(r"E:\skyr\TESV.exe")
DEFAULT_FUNCS = Path(r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json")
DEFAULT_HAVOK = (0xD0C5E0, 0xFC0C5F)


def load_text(exe):
    import pefile
    pe = pefile.PE(str(exe), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    sec = max((s for s in pe.sections if s.Characteristics & 0x20000000),
              key=lambda s: s.VirtualAddress)
    lo = base + sec.VirtualAddress
    raw = sec.get_data()
    return lo, raw


def prefixes(bs):
    i = 0
    pfx = []
    while i < len(bs) and bs[i] in (0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E,
                                    0x36, 0x3E, 0x26, 0x64, 0x65):
        pfx.append(bs[i])
        i += 1
    op2 = bs[i + 1] if i + 1 < len(bs) and bs[i] == 0x0F else None
    return pfx, op2


def is_mmx(insn):
    _pfx, op2 = prefixes(insn.bytes)
    if op2 == 0x77:  # emms has no MM register operand in Capstone
        return True
    return any(op.type == capstone.x86.X86_OP_REG and
               x86.X86_REG_MM0 <= op.reg <= x86.X86_REG_MM7
               for op in insn.operands)


def form_key(insn):
    _pfx, op2 = prefixes(insn.bytes)
    sig = []
    for op in insn.operands:
        if op.type == capstone.x86.X86_OP_REG:
            if x86.X86_REG_MM0 <= op.reg <= x86.X86_REG_MM7:
                sig.append("mm")
            elif x86.X86_REG_XMM0 <= op.reg <= x86.X86_REG_XMM7:
                sig.append("xmm")
            elif op.size == 4:
                sig.append("r32")
            else:
                sig.append(f"r{op.size * 8}")
        elif op.type == capstone.x86.X86_OP_MEM:
            sig.append(f"m{op.size * 8}")
        elif op.type == capstone.x86.X86_OP_IMM:
            sig.append("imm")
        else:
            sig.append("?")
    return f"{insn.mnemonic}|0f{op2:02x}|{','.join(sig)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--functions", type=Path, default=DEFAULT_FUNCS)
    ap.add_argument("--json", type=Path, default=ROOT / "build_host" / "mmx_audit.json")
    ap.add_argument("--invalid-csv", type=Path,
                    help="optional ir_pack invalid.csv to cross-check MMX owners")
    ap.add_argument("--havok-lo", type=lambda s: int(s, 0), default=DEFAULT_HAVOK[0])
    ap.add_argument("--havok-hi", type=lambda s: int(s, 0), default=DEFAULT_HAVOK[1])
    args = ap.parse_args()

    text_lo, text = load_text(args.exe)
    text_hi = text_lo + len(text)
    data = json.loads(args.functions.read_text(encoding="utf-8"))
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    # address -> [mnemonic, bytes, owning entry set, in_havok, form key]
    sites = {}
    ranges_seen = set()
    decoded = 0
    for key, ranges in data["functions"].items():
        entry = int(key, 16)
        for span in ranges:
            lo, hi = int(span[0]), int(span[1])
            lo, hi = max(lo, text_lo), min(hi, text_hi)
            if hi <= lo or (lo, hi) in ranges_seen:
                continue
            ranges_seen.add((lo, hi))
            for insn in md.disasm(text[lo - text_lo:hi - text_lo], lo):
                decoded += 1
                if not is_mmx(insn):
                    continue
                rec = sites.setdefault(insn.address,
                                       [insn.mnemonic, insn.bytes, set(),
                                        args.havok_lo <= insn.address <= args.havok_hi,
                                        form_key(insn)])
                rec[2].add(entry)

    def summarize(want_havok):
        counts = collections.Counter()
        funcs = collections.defaultdict(set)
        examples = collections.defaultdict(list)
        for addr, (_mn, _bs, owners, in_havok, key) in sites.items():
            if in_havok != want_havok:
                continue
            counts[key] += 1
            funcs[key].update(owners)
            if len(examples[key]) < 12:
                examples[key].append(f"0x{addr:08x}")
        return [{"form": k, "instructions": n,
                 "functions": len(funcs[k]),
                 "function_addrs": [f"0x{a:08x}" for a in sorted(funcs[k])],
                 "examples": examples[k]}
                for k, n in counts.most_common()]

    havok = summarize(True)
    outside = summarize(False)
    totals = collections.Counter()
    for row in havok + outside:
        totals[row["form"]] += row["instructions"]
    invalid_mmx = []
    if args.invalid_csv:
        invalid = {}
        with args.invalid_csv.open(newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                invalid[int(row["addr"], 16)] = row.get("error", "")
        owners = set().union(*(rec[2] for rec in sites.values())) if sites else set()
        invalid_mmx = [{"addr": f"0x{a:08x}", "error": invalid[a]}
                       for a in sorted(owners & invalid.keys())]
    report = {
        "exe": str(args.exe), "functions": str(args.functions),
        "text": [f"0x{text_lo:x}", f"0x{text_hi:x}"],
        "havok_region": [f"0x{args.havok_lo:x}", f"0x{args.havok_hi:x}"],
        "discovery_ranges": len(ranges_seen), "decoded_instructions": decoded,
        "mmx_instructions": len(sites),
        "havok": {"instructions": sum(r["instructions"] for r in havok),
                  "forms": havok},
        "outside_havok": {"instructions": sum(r["instructions"] for r in outside),
                          "forms": outside},
        "all_forms": [{"form": k, "instructions": n}
                      for k, n in totals.most_common()],
        "invalid_csv": str(args.invalid_csv) if args.invalid_csv else None,
        "invalid_mmx_functions": invalid_mmx,
    }
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"decoded {decoded} instructions from {len(ranges_seen)} discovery ranges")
    print(f"MMX total {len(sites)}; Havok {report['havok']['instructions']}; "
          f"outside {report['outside_havok']['instructions']}")
    print("Havok forms:")
    for row in havok:
        print(f"  {row['form']:<20} {row['instructions']:5} insns "
              f"{row['functions']:4} functions  {', '.join(row['examples'][:4])}")
    print("Outside-Havok forms:")
    for row in outside:
        print(f"  {row['form']:<20} {row['instructions']:5} insns "
              f"{row['functions']:4} functions  {', '.join(row['examples'][:4])}")
    print("wrote", args.json)
    if args.invalid_csv:
        print(f"invalid MMX-owning functions: {len(invalid_mmx)}")
        for row in invalid_mmx:
            print(f"  {row['addr']}: {row['error']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
