"""Build an indexed, SD-backed predecoded IR container for TESV.

The decoder used here is the real C implementation in source/ir_decode.c. This
tool only:

  1. reads TESV.exe and the discovery functions.json,
  2. writes the IRM1 input consumed by tests/ir_container_pack.c,
  3. compiles and runs that packer,
  4. adds size / distribution / profile cache analysis to the report.

Every discovered function gets an index entry. Functions that the decoder can
fully decode get a predecoded function record. Functions that cannot decode are
kept as explicit invalid entries with a status; they are never silently skipped.

Usage:
  python tools/ir_pack.py
  python tools/ir_pack.py --exe E:/skyr/TESV.exe \
      --functions E:/Skyrim_N3DS_build/analysis/tesv/functions.json \
      --out E:/Skyrim_N3DS_build/ir_container
  python tools/ir_pack.py --limit 2000 --out build_ir_container/smoke
  python tools/ir_pack.py --aot aot.txt --forced forced.txt \
      --profile host/alloc_probe/tesv_profile.csv
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "staged"))

CLANG = Path(r"C:\Program Files\LLVM\bin\clang.exe")


# ---------------------------------------------------------------- inputs


def read_profile(path):
    rows = []
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.lower().startswith("addr,"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 2:
            continue
        try:
            rows.append((int(parts[0], 16), int(parts[1])))
        except ValueError:
            continue
    return rows


def read_hex_list(path):
    out = []
    if not path:
        return out
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        out.append(int(line.split()[0], 16))
    return out


def collect_functions(full, limit=None):
    """Return [(entry, end, [(lo,hi)])] sorted by entry, including empties."""
    out = []
    for key, ranges in full["functions"].items():
        entry = int(key, 16)
        blocks = []
        for lo, hi in ranges:
            lo, hi = int(lo), int(hi)
            if hi > lo:
                blocks.append((lo, hi))
        blocks.sort()
        end = max((hi for _, hi in blocks), default=entry + 1)
        if end <= entry:
            end = entry + 1
        out.append((entry, end, blocks))
    out.sort()
    if limit:
        out = out[:limit]
    return out


def build_input(exe, funcs, path, expected_base=None, name="ir_container_input.bin"):
    import peimg

    pe, base, img = peimg.load(str(exe), None)
    _ = pe
    img = bytes(img)
    if expected_base is not None and int(expected_base) != base:
        raise SystemExit(f"functions.json image_base {int(expected_base):x} != PE image base {base:x}")
    spans = [(lo, hi) for _, _, blocks in funcs for lo, hi in blocks]
    if not spans:
        raise SystemExit("no function ranges")
    lo = min(a for a, _ in spans)
    hi = max(b for _, b in spans)
    if lo < base or hi > base + len(img):
        raise SystemExit(f"functions.json range {lo:08x}..{hi:08x} is outside the image")
    blob = img[lo - base:hi - base]
    payload = struct.pack("<IIII", 0x49524D31, lo, len(blob), len(funcs))
    payload += blob
    for entry, end, blocks in funcs:
        payload += struct.pack("<III", entry, end, len(blocks))
        payload += b"".join(struct.pack("<II", a, b) for a, b in blocks)
    Path(path).write_bytes(payload)
    return len(payload), lo, hi


# --------------------------------------------------------------- toolchain


def compile_packer(work):
    if not CLANG.exists():
        found = shutil.which("clang")
        if not found:
            raise SystemExit("clang not found")
        clang = found
    else:
        clang = str(CLANG)
    exe = Path(work) / "ir_container_pack.exe"
    srcs = [str(ROOT / "tests" / "ir_container_pack.c"),
            str(ROOT / "source" / "ir_decode.c")]
    cmd = [clang, "-std=gnu11", "-O2", "-g",
           f"-I{ROOT / 'staged'}", f"-I{ROOT / 'source'}",
           "-fwrapv", "-fno-strict-aliasing",
           "-Wno-unused-function", "-Wno-unused-variable",
           "-Wno-unused-but-set-variable"] + srcs + ["-o", str(exe)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("clang failed while building tests/ir_container_pack.c")
    return exe


# ---------------------------------------------------------------- analysis


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_index_csv(path):
    entries = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines()[1:]:
        p = line.split(",")
        if len(p) != 7:
            continue
        addr = int(p[0], 16)
        entries[addr] = {
            "addr": addr,
            "end": int(p[1], 16),
            "record_size": int(p[2]),
            "blocks": int(p[3]),
            "insns": int(p[4]),
            "status": int(p[5]),
            "flags": int(p[6]),
        }
    return entries


def percentile(sorted_values, q):
    if not sorted_values:
        return 0
    k = int((len(sorted_values) - 1) * q)
    return sorted_values[k]


def simulate_lru(entries, profile, capacity, seed=0x9E3779B9, samples=1_000_000,
                 max_records=None):
    """Frequency-weighted synthetic LRU model.

    The recorded profile is aggregate (addr,hits), not a temporal trace. For a
    reproducible cache model we sample accesses from the profile weighted by
    hits using a fixed xorshift seed. The static working set is reported
    separately; it does not depend on this model.
    """
    rows = [(a, h) for a, h in profile if a in entries and entries[a]["flags"] & 1]
    if not rows:
        return {"capacity_bytes": capacity, "hit_rate": 0.0, "accesses": 0,
                "loads": 0, "load_bytes": 0, "peak_records": 0,
                "model": "frequency-weighted synthetic trace", "seed": seed}
    total_hits = sum(h for _, h in rows)
    if total_hits <= 0:
        return {"capacity_bytes": capacity, "hit_rate": 0.0, "accesses": 0,
                "loads": 0, "load_bytes": 0, "peak_records": 0,
                "model": "frequency-weighted synthetic trace", "seed": seed}

    # Deterministic weighted choice using a cumulative table and binary search.
    cumulative = []
    acc = 0
    for _, h in rows:
        acc += h
        cumulative.append(acc)
    import bisect

    state = seed & 0xFFFFFFFF
    cache = {}
    order = []
    used = 0
    hits = 0
    loads = 0
    load_bytes = 0
    peak = 0
    n = min(samples, max(1000, total_hits))
    for _ in range(n):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        pick = state % total_hits
        idx = bisect.bisect_right(cumulative, pick)
        addr = rows[idx][0]
        size = entries[addr]["record_size"]
        if addr in cache:
            hits += 1
            order.remove(addr)
            order.append(addr)
            continue
        # evict LRU until both limits allow the new record
        while order and (used + size > capacity or
                         (max_records is not None and len(order) >= max_records)):
            victim = order.pop(0)
            used -= cache.pop(victim)
        if size > capacity or (max_records is not None and max_records < 1):
            continue
        cache[addr] = size
        order.append(addr)
        used += size
        loads += 1
        load_bytes += size
        peak = max(peak, len(order))
    return {
        "capacity_bytes": capacity,
        "max_records": max_records,
        "hit_rate": hits / n if n else 0.0,
        "accesses": n,
        "loads": loads,
        "load_bytes": load_bytes,
        "peak_records": peak,
        "model": "frequency-weighted synthetic trace",
        "seed": seed,
    }


def analyze_profile(entries, profile_path, capacities, record_limits):
    profile = read_profile(profile_path)
    present = [(a, h) for a, h in profile if a in entries]
    missing = [a for a, _ in profile if a not in entries]
    invalid = [a for a, _ in present if not (entries[a]["flags"] & 1)]
    valid = [(a, h) for a, h in present if entries[a]["flags"] & 1]
    working_set = sum(entries[a]["record_size"] for a, _ in valid)
    total_hits = sum(h for _, h in valid)
    weighted = sum(h for _, h in valid)
    # Static hot set: greedy by hits until the capacity is reached.
    hot = []
    for capacity in capacities:
        used = 0
        covered = 0
        for a, h in sorted(valid, key=lambda kv: (-kv[1], kv[0])):
            size = entries[a]["record_size"]
            if used + size <= capacity:
                used += size
                covered += h
        hot.append({"capacity_bytes": capacity,
                    "hot_set_bytes": used,
                    "hit_rate": covered / weighted if weighted else 0.0,
                    "model": "static hot set (upper bound, no temporal effects)"})
    coverage = []
    acc_hits = 0
    acc_bytes = 0
    acc_fns = 0
    targets = [0.50, 0.90, 0.99, 1.00]
    ti = 0
    for a, h in sorted(valid, key=lambda kv: (-kv[1], kv[0])):
        acc_fns += 1
        acc_hits += h
        acc_bytes += entries[a]["record_size"]
        while ti < len(targets) and acc_hits >= targets[ti] * weighted:
            coverage.append({"coverage": targets[ti],
                             "functions": acc_fns,
                             "bytes": acc_bytes,
                             "mib": acc_bytes / 2**20})
            ti += 1
    return {
        "profile": str(profile_path),
        "functions": len(profile),
        "present": len(present),
        "missing": len(missing),
        "invalid": len(invalid),
        "valid": len(valid),
        "hits": total_hits,
        "working_set_bytes": working_set,
        "working_set_mib": working_set / 2**20,
        "missing_addrs": [f"0x{a:x}" for a in missing[:32]],
        "invalid_addrs": [f"0x{a:x}" for a in invalid[:32]],
        "lru": [simulate_lru(entries, profile, c, max_records=r)
                for c in capacities for r in record_limits],
        "hot_set": hot,
        "coverage": coverage,
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, default=Path(r"E:\skyr\TESV.exe"))
    ap.add_argument("--functions", type=Path,
                    default=Path(r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json"))
    ap.add_argument("--out", type=Path,
                    default=Path(r"E:\Skyrim_N3DS_build\ir_container"))
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--aot", type=Path, help="hex addresses that stay AOT")
    ap.add_argument("--forced", type=Path, help="hex addresses forced AOT")
    ap.add_argument("--profile", type=Path, action="append", default=[],
                    help="addr,hits CSV for cache working-set analysis (repeatable)")
    ap.add_argument("--cache", type=int, default=4 << 20,
                    help="cache budget used in the profile LRU model (default 4 MiB)")
    ap.add_argument("--cache-sizes", type=str,
                    default="1,2,4,8,16",
                    help="comma-separated MiB capacities to report")
    ap.add_argument("--cache-records", type=str, default="64,256",
                    help="comma-separated record limits to report; empty = unlimited")
    ap.add_argument("--limit", type=int, help="only the first N functions by address")
    ap.add_argument("--name", default="tesv", help="output basename")
    ap.add_argument("--no-index-csv", action="store_true")
    ap.add_argument("--keep-work", action="store_true")
    args = ap.parse_args()

    if not args.exe.exists():
        raise SystemExit(f"missing exe: {args.exe}")
    if not args.functions.exists():
        raise SystemExit(f"missing functions.json: {args.functions}")
    args.out.mkdir(parents=True, exist_ok=True)
    work = args.work or (args.out / "_work")
    work.mkdir(parents=True, exist_ok=True)

    full = json.loads(args.functions.read_text(encoding="utf-8"))
    funcs = collect_functions(full, args.limit)
    if not funcs:
        raise SystemExit("no functions selected")
    print(f"functions: {len(funcs)} (from {args.functions})")
    inp = work / "ir_container_input.bin"
    inp_bytes, lo, hi = build_input(args.exe, funcs, inp,
                                    expected_base=full.get("image_base"))
    print(f"input: {inp_bytes:,} bytes, image {lo:08x}..{hi:08x}")

    packer = compile_packer(work)
    container = args.out / f"{args.name}.irc"
    report_path = args.out / f"{args.name}_container.json"
    csv_path = work / f"{args.name}_index.csv"
    invalid_path = work / f"{args.name}_invalid.csv"
    cmd = [str(packer), str(inp), str(container),
           "--report", str(report_path)]
    if not args.no_index_csv:
        cmd += ["--index-csv", str(csv_path)]
    cmd += ["--invalid-csv", str(invalid_path)]
    if args.aot:
        cmd += ["--aot", str(args.aot)]
    if args.forced:
        cmd += ["--forced", str(args.forced)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("ir_container_pack failed")
    print(p.stdout.strip())

    report = json.loads(report_path.read_text(encoding="utf-8"))
    report["container_sha256"] = sha256(container)
    report["input_bytes"] = inp_bytes
    report["image_lo"] = f"0x{lo:x}"
    report["image_hi"] = f"0x{hi:x}"
    report["aot_list"] = str(args.aot) if args.aot else None
    report["forced_list"] = str(args.forced) if args.forced else None
    known = {a for a, _, _ in funcs}
    if args.aot:
        a = read_hex_list(args.aot)
        report["aot_missing"] = len(set(a) - known)
    if args.forced:
        f = read_hex_list(args.forced)
        report["forced_missing"] = len(set(f) - known)
    report["sd_path"] = f"sdmc:/Skyrim/ir/{args.name}.irc"
    if invalid_path.exists():
        public_invalid = args.out / f"{args.name}_invalid.csv"
        shutil.copyfile(invalid_path, public_invalid)
        report["invalid_csv"] = str(public_invalid)
        # Hex-only list ready for --forced / AOT selection.
        addrs = []
        for line in public_invalid.read_text(encoding="utf-8").splitlines()[1:]:
            p = line.split(",", 1)
            if p and p[0]:
                addrs.append(p[0])
        forced_txt = args.out / f"{args.name}_invalid_addrs.txt"
        forced_txt.write_text("".join(a + "\n" for a in addrs), encoding="ascii")
        report["invalid_addrs_txt"] = str(forced_txt)
    else:
        report["invalid_csv"] = None
        report["invalid_addrs_txt"] = None
    report["status_names"] = {
        "0": "ok", "1": "unsupported opcode", "2": "block has no terminator",
        "3": "empty block", "4": "bad range", "5": "block too large",
        "6": "not decoded", "7": "unknown",
    }
    report["cache_default_bytes"] = args.cache

    entries = {}
    if not args.no_index_csv and csv_path.exists():
        entries = read_index_csv(csv_path)
    if entries and args.profile:
        capacities = [int(x) * 2**20 for x in args.cache_sizes.split(",") if x.strip()]
        record_limits = [None if not x.strip() else int(x)
                         for x in args.cache_records.split(",")]
        if not record_limits:
            record_limits = [None]
        report["profiles"] = [analyze_profile(entries, p, capacities, record_limits)
                              for p in args.profile]
    report_path.write_text(json.dumps(report, indent=1), encoding="utf-8")

    size = container.stat().st_size
    print(f"container: {size:,} bytes ({size / 2**20:.2f} MiB) "
          f"-> {container}")
    print(f"  index: {report['index_bytes']:,} bytes")
    print(f"  data:  {report['data_bytes']:,} bytes, "
          f"{report['decoded']:,}/{report['functions']:,} decoded, "
          f"{report['invalid']:,} invalid")
    print(f"  blocks {report['blocks']:,}, insns {report['insns']:,}, "
          f"record avg {report['record_size']['avg']:.1f} B, "
          f"p50 {report['record_size']['p50']}, p99 {report['record_size']['p99']}")
    for name, n in report["status"].items():
        if n:
            print(f"  status {name}: {n}")
    for prof in report.get("profiles", []):
        print(f"profile {prof['profile']}: {prof['functions']} rows, "
              f"{prof['valid']} valid, working set "
              f"{prof['working_set_mib']:.2f} MiB")
        for row in prof["lru"]:
            cap = row["capacity_bytes"] / 2**20
            recs = row.get("max_records")
            print(f"  LRU {cap:5.1f} MiB / {str(recs) if recs else 'all':>4} recs: "
                  f"hit {100 * row['hit_rate']:5.1f}%  loads {row['loads']:,}  "
                  f"peak {row['peak_records']} records")
        for row in prof["coverage"]:
            print(f"  hits {100 * row['coverage']:5.1f}%: "
                  f"{row['functions']:,} functions, {row['mib']:.2f} MiB")
    print(f"report: {report_path}")

    if not args.keep_work and not args.work:
        try:
            for f in work.iterdir():
                if f.name == "ir_container_input.bin":
                    f.unlink()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
