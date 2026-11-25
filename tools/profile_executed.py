"""Turn a TESV execution profile into root lists and statistics.

Input is the CSV written by the diagnostic runner (rt_prof_write_csv):
`addr,hits`, one lifted function per row.

Outputs, in --out:
  executed.txt      every executed function, one hex address per line
  hot-<n>.txt       the N busiest functions (for an AOT-only build)
and prints call-share and size statistics using functions.json ranges.

Usage:
  python tools/profile_executed.py PROFILE.csv FUNCTIONS.json --out DIR [--hot 512]
"""
import argparse
import json
from pathlib import Path


def read_profile(path):
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.lower().startswith("addr,"):
            continue
        addr, hits = line.split(",")
        rows.append((int(addr, 16), int(hits)))
    rows.sort(key=lambda r: (-r[1], r[0]))
    return rows


def function_bytes(functions, addr):
    key = f"0x{addr:x}"
    ranges = functions.get(key)
    if not ranges:
        return 0
    return sum(hi - lo + 1 for lo, hi in ranges)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("profile", type=Path)
    ap.add_argument("functions", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--hot", type=int, default=512)
    args = ap.parse_args()

    rows = read_profile(args.profile)
    functions = json.loads(args.functions.read_text(encoding="utf-8"))["functions"]
    if not rows:
        raise SystemExit("profile is empty: no lifted function was executed")

    total = sum(h for _, h in rows)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "executed.txt").write_text(
        "".join(f"0x{a:08x}\n" for a, _ in rows), encoding="utf-8")
    hot = rows[: args.hot]
    (args.out / f"hot-{len(hot)}.txt").write_text(
        "".join(f"0x{a:08x}\n" for a, _ in hot), encoding="utf-8")

    known = sum(function_bytes(functions, a) for a, _ in rows)
    total_bytes = sum(function_bytes(functions, a) for a in
                      (int(k, 16) for k in functions))
    print(f"executed functions: {len(rows)} of {len(functions)} discovered "
          f"({100.0 * len(rows) / len(functions):.1f}%)")
    print(f"total calls: {total}")
    print(f"executed x86 bytes (known ranges): {known} "
          f"({100.0 * known / total_bytes:.2f}% of discovered code)")
    print(f"hot-{len(hot)} covers "
          f"{100.0 * sum(h for _, h in hot) / total:.1f}% of calls and "
          f"{sum(function_bytes(functions, a) for a, _ in hot)} x86 bytes")
    print("top 15 by calls:")
    for a, h in rows[:15]:
        print(f"  {a:08x}  {h:8d}  {function_bytes(functions, a):6d} B")
    print(f"wrote {args.out / 'executed.txt'} and {args.out / f'hot-{len(hot)}.txt'}")


if __name__ == "__main__":
    main()
