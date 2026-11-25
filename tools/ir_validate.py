"""Validate an emitted AOT/IR partition.

Checks the aot.txt / ir_table.c / manifest.json triple produced by
tools/ir_select.py:

  - AOT addresses are sorted and unique;
  - IR exact entries are sorted and unique (extents may overlap; that is the
    documented shared-cold-block case, and only exact entries are callable);
  - no address is in both tiers;
  - every entry exists in functions.json;
  - end > addr for every IR entry;
  - the manifest's counts and union-byte projections agree with the files.

Shared basic blocks are counted once: the union of the interval lists is what
the manifest reports as ir_x86_bytes / aot_union_bytes.

Usage:
  python tools/ir_validate.py DIR FUNCTIONS.json
"""
import json
import re
import sys
from pathlib import Path

IR_ENTRY = re.compile(r"\{\s*0x([0-9a-fA-F]+)u?\s*,\s*0x([0-9a-fA-F]+)u?\s*\}")


def read_aot(path):
    out = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(int(line, 16))
    return out


def read_ir_table(path):
    return [(int(a, 16), int(b, 16)) for a, b in IR_ENTRY.findall(
        Path(path).read_text(encoding="utf-8"))]


def union_bytes(ranges):
    spans = sorted((int(a), int(b)) for a, b in ranges)
    total = 0
    lo = hi = None
    for a, b in spans:
        if lo is None:
            lo, hi = a, b
        elif a <= hi:
            hi = max(hi, b)
        else:
            total += hi - lo
            lo, hi = a, b
    if lo is not None:
        total += hi - lo
    return total


def validate(aot, ir, manifest, functions, strict_union=True):
    errors = []
    if aot != sorted(set(aot)):
        errors.append("aot.txt is not sorted/unique")
    addrs = [a for a, _ in ir]
    if addrs != sorted(set(addrs)):
        errors.append("ir_table.c entries are not sorted/unique")
    both = set(aot) & set(addrs)
    if both:
        errors.append(f"{len(both)} addresses are in both tiers: {sorted(both)[:4]}")
    for a, e in ir:
        if e <= a:
            errors.append(f"IR entry {a:08x}: end {e:08x} <= addr")
        if f"0x{a:x}" not in functions:
            errors.append(f"IR entry {a:08x} is not in functions.json")
    for a in aot:
        if f"0x{a:x}" not in functions:
            errors.append(f"AOT entry {a:08x} is not in functions.json")
    if manifest:
        if manifest.get("aot_functions") != len(aot):
            errors.append("manifest aot_functions != aot.txt length")
        if manifest.get("ir_functions") != len(ir):
            errors.append("manifest ir_functions != ir_table.c length")
        ranges = []
        for a, _ in ir:
            if f"0x{a:x}" in functions:
                ranges.extend(functions[f"0x{a:x}"])
        u = union_bytes(ranges)
        if strict_union and manifest.get("ir_x86_bytes") is not None and manifest["ir_x86_bytes"] != u:
            errors.append(f"manifest ir_x86_bytes {manifest['ir_x86_bytes']} != union {u}")
    return errors


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    d = Path(sys.argv[1])
    functions = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))["functions"]
    aot = read_aot(d / "aot.txt")
    ir = read_ir_table(d / "ir_table.c")
    manifest = json.loads((d / "manifest.json").read_text(encoding="utf-8"))
    errs = validate(aot, ir, manifest, functions)
    print(f"aot {len(aot)}, ir {len(ir)}, union {manifest.get('ir_x86_bytes')} B")
    if errs:
        for e in errs:
            print("FAIL:", e)
        raise SystemExit(1)
    print("partition: OK")


if __name__ == "__main__":
    main()
