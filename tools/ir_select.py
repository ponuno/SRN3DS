"""Select the AOT tier from a recorded profile.

Input: a profile CSV and the discovered functions.json (addr -> list of
[start,end] basic blocks). The CSV is backward compatible with `addr,hits`
and optionally accepts a third measured-time column:

    addr,hits,time_ns
    addr,hits,ticks

When a complete time column is present, time ranking is the default. Use
`--rank-by calls` to retain call-count ranking or `--rank-by weighted_insns`
for the explicitly-labelled proxy `hits * instruction_count`.

Output: a deterministic AOT address list, an optional IR range table for the
complement, and a manifest with call/time coverage and AOT/IR splits. Ties are
broken by address, so repeated runs over the same inputs are byte-identical.

Usage:
  python tools/ir_select.py PROFILE.csv FUNCTIONS.json --out DIR
  python tools/ir_select.py PROFILE.csv FUNCTIONS.json --out DIR --rank-by time
  python tools/ir_select.py PROFILE.csv FUNCTIONS.json --out DIR --rank-by calls
  python tools/ir_select.py PROFILE.csv FUNCTIONS.json --out DIR --rank-by weighted_insns
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Measured in this workspace: docs/CODEGEN.md (25.35 B/x86 insn, 9k functions)
# and docs/IR.md (8.26 B/x86 insn on the growth12 unique pass).
DEFAULT_AOT_BPI = 25.35
DEFAULT_IR_BPI = 8.26
DEFAULT_INSNS_PER_BYTE = 794315 / 2881478
CLOSURE_INSNS = 2_900_299


def read_profile(path):
    """Return (rows, metadata); rows are dicts with addr/hits/measured_time."""
    lines = [l.strip() for l in Path(path).read_text(encoding="utf-8-sig").splitlines()]
    lines = [l for l in lines if l and not l.startswith("#")]
    if not lines:
        return [], {"time_unit": None, "time_unit_source": "missing",
                    "time_data": "missing", "missing_time_addrs": []}

    time_unit = None
    unit_source = "missing"
    if lines[0].lower().startswith("addr,"):
        header = [p.strip().lower() for p in lines[0].split(",")]
        if len(header) not in (2, 3) or header[:2] != ["addr", "hits"]:
            raise SystemExit(f"bad profile header: {lines[0]}")
        if len(header) == 3:
            if header[2] not in ("time_ns", "ticks"):
                raise SystemExit(f"unsupported time column: {header[2]}")
            time_unit = header[2]
            unit_source = "header"
        lines = lines[1:]

    rows = []
    seen = set()
    inferred_three = False
    for line in lines:
        parts = [p.strip() for p in line.split(",")]
        if len(parts) not in (2, 3):
            raise SystemExit(f"bad profile row: {line}")
        addr = int(parts[0], 16)
        hits = int(parts[1])
        if addr in seen:
            raise SystemExit(f"duplicate profile address: {addr:08x}")
        seen.add(addr)
        measured = None
        if len(parts) == 3:
            inferred_three = True
            if parts[2] != "":
                measured = int(parts[2])
        rows.append({"addr": addr, "hits": hits, "measured_time": measured})

    if time_unit is None and inferred_three:
        # Headerless three-column profiles are accepted, but the assumption is
        # recorded explicitly; ticks cannot be inferred from an unlabelled row.
        time_unit = "time_ns"
        unit_source = "inferred_headerless"

    missing_time = [r["addr"] for r in rows if r["measured_time"] is None]
    if time_unit is None:
        time_data = "missing"
    elif not missing_time:
        time_data = "complete"
    elif len(missing_time) == len(rows):
        time_data = "missing"
    else:
        time_data = "partial"
    return rows, {"time_unit": time_unit, "time_unit_source": unit_source,
                  "time_data": time_data, "missing_time_addrs": missing_time}


def function_bytes(functions, addr):
    key = f"0x{addr:x}"
    ranges = functions.get(key)
    if not ranges:
        return None, 0
    return ranges, sum(int(hi) - int(lo) for lo, hi in ranges)


def union_bytes(ranges):
    spans = sorted((int(a), int(b)) for a, b in ranges)
    total = 0
    cur_lo = cur_hi = None
    for a, b in spans:
        if cur_lo is None:
            cur_lo, cur_hi = a, b
        elif a <= cur_hi:
            cur_hi = max(cur_hi, b)
        else:
            total += cur_hi - cur_lo
            cur_lo, cur_hi = a, b
    if cur_lo is not None:
        total += cur_hi - cur_lo
    return total


def read_addr_list(path):
    out = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(int(line, 16))
    return out


def make_insn_counter(exe):
    """Return (counter, basis). None counter means use the byte-ratio proxy."""
    if not exe or not Path(exe).exists():
        return None, "bytes_x_insn_per_byte_proxy"
    try:
        sys.path.insert(0, str(ROOT / "staged"))
        import peimg
        import capstone
    except Exception:
        return None, "bytes_x_insn_per_byte_proxy"
    pe, base, img = peimg.load(str(exe), None)
    _ = pe
    img = bytes(img)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def count(ranges):
        n = 0
        for lo, hi in ranges:
            lo, hi = int(lo), int(hi)
            n += sum(1 for _ in md.disasm(img[lo - base:hi - base], lo))
        return n

    return count, "capstone"


def ranking_value(row, mode):
    if mode == "time":
        return row["measured_time"]
    if mode == "weighted_insns":
        return row["weighted_insns"]
    return row["hits"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", type=Path)
    ap.add_argument("functions", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--rank-by", choices=("time", "calls", "weighted_insns"),
                    help="default: time when a complete time column exists, else calls")
    ap.add_argument("--limit", type=int, help="maximum AOT functions")
    ap.add_argument("--max-x86-bytes", type=int, help="AOT budget in x86 bytes")
    ap.add_argument("--aot-bytes", type=int, help="AOT budget in projected ARM bytes")
    ap.add_argument("--aot-bpi", type=float, default=DEFAULT_AOT_BPI)
    ap.add_argument("--ir-bpi", type=float, default=DEFAULT_IR_BPI)
    ap.add_argument("--insn-per-byte", type=float, default=DEFAULT_INSNS_PER_BYTE,
                    help="x86 instructions per x86 byte (growth12 unique-pass measurement)")
    ap.add_argument("--exe", type=Path, default=Path(r"E:\skyr\TESV.exe"),
                    help="PE image used to count profile/AOT instructions accurately")
    ap.add_argument("--force-aot", type=Path,
                    help="extra addresses to keep AOT (e.g. functions the IR tier cannot decode)")
    ap.add_argument("--ir-scope", choices=("all", "profile"), default="all",
                    help="emit IR ranges for every discovered function or only profile functions")
    ap.add_argument("--emit-ir-table", type=Path,
                    help="write a C IrFnEntry table for the non-AOT functions")
    ap.add_argument("--validate", action="store_true",
                    help="validate the emitted table/manifest partition and fail on ambiguity")
    args = ap.parse_args()

    rows, pmeta = read_profile(args.profile)
    if not rows:
        raise SystemExit("profile is empty")
    rank_by = args.rank_by or ("time" if pmeta["time_data"] == "complete" else "calls")
    if rank_by == "time" and pmeta["time_data"] != "complete":
        missing = ", ".join(f"{a:08x}" for a in pmeta["missing_time_addrs"][:8])
        raise SystemExit("--rank-by time requires a complete time_ns/ticks column; "
                         f"time data is {pmeta['time_data']}"
                         + (f" (missing at {missing})" if missing else ""))

    doc = json.loads(args.functions.read_text(encoding="utf-8"))
    functions = doc["functions"]
    count_insns, insn_basis = make_insn_counter(args.exe)

    stats = {}
    missing_functions = []
    for row in rows:
        addr = row["addr"]
        ranges, nbytes = function_bytes(functions, addr)
        if ranges is None:
            ninsns = None
            basis = "missing_function"
            missing_functions.append(addr)
        else:
            ninsns = count_insns(ranges) if count_insns else None
            basis = insn_basis
            if ninsns is None:
                ninsns = int(round(nbytes * args.insn_per_byte))
        t = row["measured_time"]
        rec = {
            "addr": addr,
            "hits": row["hits"],
            "x86_bytes": nbytes,
            "insns": ninsns,
            "insn_count_basis": basis,
            "weighted_insns": row["hits"] * ninsns if ninsns is not None else None,
            "measured_time": t,
            "time_unit": pmeta["time_unit"],
            "time_ns": t if pmeta["time_unit"] == "time_ns" else None,
            "ticks": t if pmeta["time_unit"] == "ticks" else None,
            "time_basis": "measured" if t is not None else "missing",
            "estimated_time": None,
            "estimate_basis": ("weighted_insns proxy; no time calibration"
                               if rank_by == "weighted_insns" else None),
        }
        rec["ranking_value"] = ranking_value(rec, rank_by)
        stats[addr] = rec

    if rank_by == "weighted_insns" and any(r["weighted_insns"] is None for r in stats.values()):
        bad = [a for a, r in stats.items() if r["weighted_insns"] is None]
        raise SystemExit("--rank-by weighted_insns requires an instruction count for every "
                         f"profile function; missing at {', '.join(f'{a:08x}' for a in bad[:8])}")

    ranked = sorted(rows, key=lambda r: (-stats[r["addr"]]["ranking_value"], r["addr"]))
    rank_position = {r["addr"]: i + 1 for i, r in enumerate(ranked)}
    forced = set(read_addr_list(args.force_aot)) if args.force_aot else set()
    hits_by_addr = {r["addr"]: r["hits"] for r in rows}

    selected = []
    seen = set()
    x86_total = 0
    aot_insns = 0
    projected = 0.0

    def add(addr, hits, why):
        nonlocal x86_total, aot_insns, projected
        if addr in seen:
            return True
        if addr in stats:
            rec = dict(stats[addr])
        else:
            ranges, nbytes = function_bytes(functions, addr)
            ninsns = count_insns(ranges) if ranges and count_insns else None
            basis = insn_basis if ranges else "missing_function"
            if ninsns is None:
                ninsns = int(round(nbytes * args.insn_per_byte))
            rec = {"addr": addr, "hits": hits, "x86_bytes": nbytes, "insns": ninsns,
                   "insn_count_basis": basis, "weighted_insns": hits * ninsns,
                   "measured_time": None, "time_unit": pmeta["time_unit"],
                   "time_ns": None, "ticks": None, "time_basis": "missing",
                   "estimated_time": None, "estimate_basis": None,
                   "ranking_value": None}
        if args.max_x86_bytes is not None and x86_total + rec["x86_bytes"] > args.max_x86_bytes:
            return False
        if args.aot_bytes is not None and projected + rec["insns"] * args.aot_bpi > args.aot_bytes:
            return False
        seen.add(addr)
        rec["reason"] = why
        rec["rank_position"] = rank_position.get(addr)
        selected.append(rec)
        x86_total += rec["x86_bytes"]
        aot_insns += rec["insns"]
        projected += rec["insns"] * args.aot_bpi
        return True

    for a in sorted(forced):
        add(a, hits_by_addr.get(a, 0), "forced")
    for row in ranked:
        if args.limit is not None and len(selected) >= args.limit:
            break
        add(row["addr"], row["hits"], "profile")

    profile_set = set(stats)
    covered = sum(r["hits"] for r in selected)
    share = covered / sum(r["hits"] for r in rows) if rows else 0.0
    aot_addrs = sorted(r["addr"] for r in selected)

    # Complement for the interpreter table.
    ir_addrs = []
    ir_ranges = []
    for key, ranges in functions.items():
        addr = int(key, 16)
        if addr in seen:
            continue
        if args.ir_scope == "profile" and addr not in profile_set:
            continue
        end = max(int(hi) for _, hi in ranges)
        ir_addrs.append((addr, end))
        ir_ranges.extend(ranges)
    ir_addrs.sort()
    ir_x86_union = union_bytes(ir_ranges)
    ir_x86_sum = sum(sum(int(hi) - int(lo) for lo, hi in functions[f"0x{a:x}"])
                     for a, _ in ir_addrs)
    aot_union = union_bytes([r for key, ranges in functions.items()
                             if int(key, 16) in seen for r in ranges])
    ir_insns = int(round(ir_x86_union * args.insn_per_byte))
    projected_ir = ir_insns * args.ir_bpi
    projected_total = projected + projected_ir
    closure_ir = CLOSURE_INSNS * args.ir_bpi / 2**20
    closure_20 = (CLOSURE_INSNS * 0.2 * args.aot_bpi +
                  CLOSURE_INSNS * 0.8 * args.ir_bpi) / 2**20
    forced_sel = [r for r in selected if r["reason"] == "forced"]
    forced_bytes = sum(r["x86_bytes"] for r in forced_sel)
    forced_insns = sum(r["insns"] for r in forced_sel)

    # Time split is reported only when every profile row has measured time.
    # Forced functions absent from the profile have unknown time and are not
    # silently assigned zero measured time.
    if pmeta["time_data"] == "complete":
        total_time = sum(r["measured_time"] for r in rows)
        aot_time = sum(stats[r["addr"]]["measured_time"] for r in selected
                       if r["addr"] in profile_set)
        ir_time = total_time - aot_time
        covered_time_share = aot_time / total_time if total_time else None
        time_split = {"basis": f"measured_{pmeta['time_unit']}",
                      "scope": "recorded profile functions only",
                      "aot": aot_time, "ir": ir_time,
                      "aot_share": aot_time / total_time if total_time else None,
                      "ir_share": ir_time / total_time if total_time else None}
    else:
        total_time = aot_time = ir_time = covered_time_share = None
        time_split = {"basis": "unavailable", "scope": "recorded profile functions only",
                      "aot": None, "ir": None, "aot_share": None, "ir_share": None}

    if all(r["weighted_insns"] is not None for r in stats.values()):
        total_weighted = sum(r["weighted_insns"] for r in stats.values())
        aot_weighted = sum(stats[r["addr"]]["weighted_insns"] for r in selected
                           if r["addr"] in profile_set)
        ir_weighted = total_weighted - aot_weighted
        weighted_split = {"basis": "proxy_hits_x_insns", "aot": aot_weighted,
                          "ir": ir_weighted,
                          "aot_share": aot_weighted / total_weighted if total_weighted else None,
                          "ir_share": ir_weighted / total_weighted if total_weighted else None}
    else:
        weighted_split = {"basis": "unavailable", "aot": None, "ir": None,
                          "aot_share": None, "ir_share": None}

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "aot.txt").write_text(
        "".join(f"0x{a:08x}\n" for a in aot_addrs), encoding="utf-8")
    (args.out / "ir.txt").write_text(
        "".join(f"0x{a:08x} 0x{e:08x}\n" for a, e in ir_addrs), encoding="utf-8")

    profile_rows = []
    for row in ranked:
        rec = dict(stats[row["addr"]])
        rec["rank_position"] = rank_position[row["addr"]]
        profile_rows.append(rec)

    manifest = {
        "profile": str(args.profile),
        "functions": str(args.functions),
        "ranking_mode": rank_by,
        "ranking_basis": {"time": "measured_time", "calls": "calls",
                          "weighted_insns": "proxy_hits_x_insns"}[rank_by],
        "weighted_insns_is_proxy": True,
        "hardware_time_claim": False,
        "profile_time_unit": pmeta["time_unit"],
        "profile_time_unit_source": pmeta["time_unit_source"],
        "profile_time_data": pmeta["time_data"],
        "missing_time_addrs": sorted(pmeta["missing_time_addrs"]),
        "profile_functions": len(rows),
        "total_calls": sum(r["hits"] for r in rows),
        "total_measured_time": total_time,
        "aot_functions": len(selected),
        "aot_hits": covered,
        "covered_call_share": round(share, 6),
        "covered_time_share": round(covered_time_share, 6) if covered_time_share is not None else None,
        "aot_ir_time_split": time_split,
        "aot_ir_weighted_insns_split": weighted_split,
        "aot_x86_bytes": x86_total,
        "aot_union_bytes": aot_union,
        "aot_insns": aot_insns,
        "forced_aot_functions": len(forced_sel),
        "forced_aot_x86_bytes": forced_bytes,
        "forced_aot_insns": forced_insns,
        "forced_aot_projected_bytes": int(round(forced_insns * args.aot_bpi)),
        "projected_aot_bytes": int(round(projected)),
        "ir_functions": len(ir_addrs),
        "ir_x86_bytes": ir_x86_union,
        "ir_x86_bytes_summed": ir_x86_sum,
        "ir_insns_est": ir_insns,
        "projected_ir_bytes": int(round(projected_ir)),
        "projected_total_bytes": int(round(projected_total)),
        "insn_per_byte": args.insn_per_byte,
        "insn_count_basis": insn_basis,
        "aot_bpi": args.aot_bpi,
        "ir_bpi": args.ir_bpi,
        "closure_projected_100ir_mib": round(closure_ir, 2),
        "closure_projected_20aot_mib": round(closure_20, 2),
        "limits": {"limit": args.limit, "max_x86_bytes": args.max_x86_bytes,
                   "aot_bytes": args.aot_bytes},
        "ir_scope": args.ir_scope,
        "missing_from_functions_json": sorted(missing_functions),
        "profile_rows": profile_rows,
        "selected": selected,
    }
    (args.out / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if args.emit_ir_table:
        args.emit_ir_table.parent.mkdir(parents=True, exist_ok=True)
        lines = [
            "/* Generated by tools/ir_select.py. Sorted, unique exact entries. */",
            '#include "ir_core.h"',
            "const IrFnEntry g_ir_table[] = {",
        ]
        lines += [f"    {{0x{a:08x}u, 0x{e:08x}u}}," for a, e in ir_addrs]
        lines += ["};", f"const unsigned g_ir_count = {len(ir_addrs)};", ""]
        args.emit_ir_table.write_text("\n".join(lines), encoding="utf-8")

    rank_label = {"time": f"time (measured {pmeta['time_unit']})",
                  "calls": "calls",
                  "weighted_insns": "weighted_insns (PROXY: hits x instructions)"}[rank_by]
    print(f"ranking: {rank_label}")
    print(f"profile: {len(rows)} functions, {manifest['total_calls']} calls")
    print(f"AOT: {len(selected)} functions, {covered} calls "
          f"({100.0 * share:.2f}% of calls), {x86_total} x86 bytes, {aot_insns} instructions")
    if time_split["basis"] != "unavailable":
        print(f"measured time ({pmeta['time_unit']}): AOT {aot_time} / IR {ir_time}; "
              f"covered time share {100.0 * covered_time_share:.2f}%")
    else:
        print(f"measured time: unavailable ({pmeta['time_data']}); "
              "covered time share is null, not inferred")
    if weighted_split["basis"] != "unavailable":
        print(f"weighted-instruction proxy: AOT {aot_weighted} / IR {ir_weighted}; "
              f"AOT share {100.0 * weighted_split['aot_share']:.2f}% (not measured time)")
    print(f"projected AOT: {projected / 2**20:.2f} MiB at {args.aot_bpi:.2f} B/insn")
    print(f"forced AOT (unsupported/undecodable): {len(forced_sel)} functions, "
          f"{forced_bytes} x86 bytes, {forced_insns} instructions, "
          f"{forced_insns * args.aot_bpi / 2**20:.2f} MiB projected")
    print(f"IR: {len(ir_addrs)} functions, {ir_x86_union} x86 bytes, "
          f"~{ir_insns} instructions ({projected_ir / 2**20:.2f} MiB at "
          f"{args.ir_bpi:.2f} B/insn)")
    print(f"slice projected total: {projected_total / 2**20:.2f} MiB")
    print(f"closure projection: 100% IR {closure_ir:.1f} MiB, "
          f"20% AOT {closure_20:.1f} MiB, 50% AOT "
          f"{(CLOSURE_INSNS * 0.5 * args.aot_bpi + CLOSURE_INSNS * 0.5 * args.ir_bpi) / 2**20:.1f} MiB")
    if missing_functions:
        print(f"warning: {len(missing_functions)} profile addresses are not in functions.json")
    if pmeta["time_data"] != "complete":
        print("warning: time data is missing/partial; no measured time share is claimed")
    print(f"wrote {args.out / 'aot.txt'}, {args.out / 'manifest.json'}"
          + (f", {args.emit_ir_table}" if args.emit_ir_table else ""))
    if args.validate:
        sys.path.insert(0, str(ROOT / "tools"))
        import ir_validate
        errs = ir_validate.validate(aot_addrs, ir_addrs, manifest, functions)
        if errs:
            for e in errs:
                print("PARTITION FAIL:", e)
            raise SystemExit(1)
        print("partition: OK")


if __name__ == "__main__":
    main()
