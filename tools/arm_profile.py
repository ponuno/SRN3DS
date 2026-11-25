"""Validate and consume the isolated ARM11/Azahar timing-profile fixture.

The guest harness writes ``addr,hits,ticks`` CSV plus metadata.  ``check``
refuses to rank a profile unless AOT/IR equivalence passed, the run is labelled
as emulator timing, every fixture address/hit count matches, warm IR cache misses
are zero, and cold IR cache misses are present.  It then feeds the canonical CSV
to ``tools/ir_select.py --rank-by time`` and reports the selector's covered time
share and AOT/IR partition.

``collect`` archives the known guest outputs and a bounded tail of Azahar's log.
It never archives an unbounded emulator log or silently truncates CSV/JSON.

Usage:
  python tools/arm_profile.py collect --sd-root "$env:APPDATA\\Azahar\\sdmc" --out RUN_DIR
  python tools/arm_profile.py check --profile RUN_DIR/armtime_profile.csv \
      --metadata RUN_DIR/armtime_meta.json --out RUN_DIR/selection --limit 5
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FIXTURE = ROOT / "tests" / "isolated" / "armtime" / "fixture.json"
DEFAULT_SELECTOR = ROOT / "tools" / "ir_select.py"
RESULT_FILES = (
    "Skyrim_N3DS.log",
    "armtime_profile.csv",
    "armtime_aot.csv",
    "armtime_ir_warm.csv",
    "armtime_ir_cold.csv",
    "armtime_meta.json",
)


class ProfileError(Exception):
    pass


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProfileError(f"cannot read JSON {path}: {exc}") from exc


def as_int(value, what):
    try:
        return int(str(value), 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as exc:
        raise ProfileError(f"invalid integer for {what}: {value!r}") from exc


def bounded_tail(path, max_bytes):
    path = Path(path)
    size = path.stat().st_size
    with path.open("rb") as f:
        if size > max_bytes:
            f.seek(size - max_bytes)
            data = f.read(max_bytes)
            marker = b"\n[truncated: retained last %d bytes]\n" % max_bytes
            return marker + data, True
        return f.read(), False


def collect_command(args):
    sd = Path(args.sd_root)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    files = []
    for name in RESULT_FILES:
        src = sd / name
        if not src.is_file():
            continue
        original_size = src.stat().st_size
        if original_size > args.max_result_bytes:
            raise ProfileError(f"{src} is {original_size} bytes; limit is "
                               f"{args.max_result_bytes}. Refusing to truncate CSV/JSON.")
        data = src.read_bytes()
        dst = out / name
        dst.write_bytes(data)
        files.append({
            "name": name,
            "source": str(src),
            "original_bytes": original_size,
            "archived_bytes": len(data),
            "truncated": False,
            "sha256": sha256_bytes(data),
        })

    azahar_log = Path(args.azahar_log)
    if azahar_log.is_file():
        original_size = azahar_log.stat().st_size
        data, truncated = bounded_tail(azahar_log, args.max_log_bytes)
        dst = out / "azahar_log_tail.txt"
        dst.write_bytes(data)
        files.append({
            "name": dst.name,
            "source": str(azahar_log),
            "original_bytes": original_size,
            "archived_bytes": len(data),
            "truncated": truncated,
            "sha256": sha256_bytes(data),
        })

    manifest = {
        "format_version": 1,
        "timing_scope": "azahar_emulator",
        "hardware_time_claim": False,
        "max_log_bytes": args.max_log_bytes,
        "max_result_bytes": args.max_result_bytes,
        "files": files,
    }
    (out / "archive_manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"archived {len(files)} files into {out}")
    for item in files:
        suffix = " (bounded tail)" if item["truncated"] else ""
        print(f"  {item['name']}: {item['archived_bytes']} bytes{suffix}")
    if not (out / "armtime_profile.csv").is_file():
        raise ProfileError(f"no armtime_profile.csv found under {sd}")
    return 0


def read_profile_strict(path):
    path = Path(path)
    try:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
    except OSError as exc:
        raise ProfileError(f"cannot read profile {path}: {exc}") from exc
    lines = [line.strip() for line in lines if line.strip() and not line.startswith("#")]
    if not lines or lines[0].lower() != "addr,hits,ticks":
        raise ProfileError("profile header must be exactly addr,hits,ticks")
    rows = {}
    for number, line in enumerate(lines[1:], 2):
        parts = [part.strip() for part in line.split(",")]
        if len(parts) != 3:
            raise ProfileError(f"profile line {number}: expected three columns")
        try:
            addr = int(parts[0], 16)
            hits = int(parts[1], 10)
            ticks = int(parts[2], 10)
        except ValueError as exc:
            raise ProfileError(f"profile line {number}: {exc}") from exc
        if addr in rows:
            raise ProfileError(f"duplicate profile address {addr:08x}")
        if hits <= 0 or ticks <= 0:
            raise ProfileError(f"profile line {number}: hits and ticks must be positive")
        rows[addr] = {"hits": hits, "ticks": ticks}
    if not rows:
        raise ProfileError("profile has no data rows")
    return rows


def fixture_functions(fixture):
    if fixture.get("format_version") != 1:
        raise ProfileError("unsupported fixture format_version")
    if fixture.get("benchmark") != "armtime-fixture-v1":
        raise ProfileError("unexpected fixture benchmark name")
    if fixture.get("canonical_tier") != "aot_warm":
        raise ProfileError("canonical fixture tier must be aot_warm")
    if fixture.get("clock_source") != "svcGetSystemTick":
        raise ProfileError("fixture clock must be svcGetSystemTick")
    out = {}
    for item in fixture.get("functions", []):
        addr = as_int(item.get("addr"), "fixture addr")
        end = as_int(item.get("end"), "fixture end")
        hits = as_int(item.get("hits"), "fixture hits")
        if addr in out:
            raise ProfileError(f"duplicate fixture address {addr:08x}")
        if end <= addr or hits <= 0:
            raise ProfileError(f"invalid fixture range/hits at {addr:08x}")
        out[addr] = {
            "addr": addr,
            "end": end,
            "hits": hits,
            "name": str(item.get("name", "")),
            "workload": str(item.get("workload", "")),
        }
    if not out:
        raise ProfileError("fixture has no functions")
    return out


def validate_run(profile_path, metadata_path, fixture_path):
    fixture = read_json(fixture_path)
    meta = read_json(metadata_path)
    expected = fixture_functions(fixture)
    rows = read_profile_strict(profile_path)

    required = {
        "format_version": 1,
        "benchmark": fixture["benchmark"],
        "timing_scope": "azahar_emulator",
        "hardware_time_claim": False,
        "clock_source": "svcGetSystemTick",
        "time_unit": "ticks",
        "canonical_tier": "aot_warm",
        "canonical_profile": "armtime_profile.csv",
        "verification": "pass",
        "postflight_verification": "pass",
        "verification_inputs": 2,
        "tick_hz": as_int(fixture.get("tick_hz"), "fixture tick_hz"),
    }
    for key, want in required.items():
        if meta.get(key) != want:
            raise ProfileError(f"metadata {key} must be {want!r}, got {meta.get(key)!r}")
    if "not New 3DS hardware" not in str(meta.get("notes", "")):
        raise ProfileError("metadata notes must explicitly disclaim New 3DS hardware timing")

    if set(rows) != set(expected):
        missing = sorted(set(expected) - set(rows))
        extra = sorted(set(rows) - set(expected))
        raise ProfileError(f"profile/fixture address mismatch; missing={missing}, extra={extra}")

    meta_functions = {}
    for item in meta.get("functions", []):
        addr = as_int(item.get("addr"), "metadata addr")
        if addr in meta_functions:
            raise ProfileError(f"duplicate metadata address {addr:08x}")
        meta_functions[addr] = item
    if set(meta_functions) != set(expected):
        raise ProfileError("metadata function set does not match the fixture")

    for addr, want in expected.items():
        row = rows[addr]
        item = meta_functions[addr]
        checks = {
            "end": as_int(item.get("end"), "metadata end"),
            "hits": as_int(item.get("hits"), "metadata hits"),
            "aot_ticks": as_int(item.get("aot_ticks"), "metadata aot_ticks"),
            "ir_warm_ticks": as_int(item.get("ir_warm_ticks"), "metadata ir_warm_ticks"),
            "ir_cold_ticks": as_int(item.get("ir_cold_ticks"), "metadata ir_cold_ticks"),
            "decode_est_ticks": as_int(item.get("decode_est_ticks"), "decode_est_ticks"),
            "warm_cache_hits": as_int(item.get("warm_cache_hits"), "warm_cache_hits"),
            "warm_cache_misses": as_int(item.get("warm_cache_misses"), "warm_cache_misses"),
            "cold_cache_misses": as_int(item.get("cold_cache_misses"), "cold_cache_misses"),
            "cold_cache_blocks": as_int(item.get("cold_cache_blocks"), "cold_cache_blocks"),
            "cold_reps": as_int(item.get("cold_reps"), "cold_reps"),
        }
        if checks["end"] != want["end"] or checks["hits"] != want["hits"]:
            raise ProfileError(f"fixture range/hits mismatch at {addr:08x}")
        if row["hits"] != want["hits"] or row["ticks"] != checks["aot_ticks"]:
            raise ProfileError(f"CSV does not match verified AOT warm metadata at {addr:08x}")
        if item.get("name") != want["name"] or item.get("workload") != want["workload"]:
            raise ProfileError(f"fixture name/workload mismatch at {addr:08x}")
        if item.get("verified") is not True:
            raise ProfileError(f"metadata verification is not true at {addr:08x}")
        if any(checks[key] <= 0 for key in ("aot_ticks", "ir_warm_ticks", "ir_cold_ticks",
                                            "cold_cache_blocks")):
            raise ProfileError(f"non-positive timing/cache evidence at {addr:08x}")
        if checks["decode_est_ticks"] < 0:
            raise ProfileError(f"negative decode estimate at {addr:08x}")
        if checks["warm_cache_hits"] <= 0 or checks["warm_cache_misses"] != 0:
            raise ProfileError(f"warm IR cache was not warm at {addr:08x}")
        if checks["cold_cache_misses"] <= 0:
            raise ProfileError(f"cold IR cache misses were not observed at {addr:08x}")
        if checks["cold_reps"] != as_int(fixture.get("cold_reps"), "fixture cold_reps"):
            raise ProfileError(f"cold repetition count mismatch at {addr:08x}")

    totals = meta.get("totals", {})
    expected_totals = {
        "hits": sum(row["hits"] for row in rows.values()),
        "aot_ticks": sum(row["ticks"] for row in rows.values()),
        "ir_warm_ticks": sum(as_int(item.get("ir_warm_ticks"), "ir_warm_ticks")
                             for item in meta_functions.values()),
        "ir_cold_ticks": sum(as_int(item.get("ir_cold_ticks"), "ir_cold_ticks")
                             for item in meta_functions.values()),
    }
    for key, want in expected_totals.items():
        if as_int(totals.get(key), f"totals.{key}") != want:
            raise ProfileError(f"metadata totals.{key} mismatch")
    return fixture, meta, rows, meta_functions


def check_command(args):
    fixture, meta, rows, meta_functions = validate_run(
        args.profile, args.metadata, args.fixture)
    count = len(rows)
    if not 1 <= args.limit < count:
        raise ProfileError(f"--limit must be between 1 and {count - 1} for a nontrivial split")
    args.out.mkdir(parents=True, exist_ok=True)
    functions_doc = {
        "source": "synthetic armtime fixture extents, not game discovery ranges",
        "functions": {f"0x{addr:x}": [[addr, as_int(item["end"], "metadata end")]]
                      for addr, item in sorted(meta_functions.items())},
    }
    functions_path = args.out / "functions.json"
    functions_path.write_text(json.dumps(functions_doc, indent=2, sort_keys=True) + "\n",
                              encoding="utf-8")
    missing_exe = args.out / "fixture-has-no-pe.exe"
    cmd = [
        sys.executable, str(args.selector), str(args.profile), str(functions_path),
        "--out", str(args.out), "--rank-by", "time", "--limit", str(args.limit),
        "--ir-scope", "profile", "--exe", str(missing_exe),
        "--insn-per-byte", "1", "--validate",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    (args.out / "ir_select.log").write_text(proc.stdout + proc.stderr, encoding="utf-8")
    if proc.returncode:
        raise ProfileError(f"ir_select.py failed ({proc.returncode}); see {args.out / 'ir_select.log'}")

    manifest = read_json(args.out / "manifest.json")
    total_time = sum(row["ticks"] for row in rows.values())
    expected_rank = sorted(rows, key=lambda addr: (-rows[addr]["ticks"], addr))[:args.limit]
    selected = [item["addr"] for item in manifest.get("selected", [])]
    if selected != expected_rank:
        raise ProfileError(f"selector order mismatch: got {selected}, expected {expected_rank}")
    if manifest.get("ranking_mode") != "time" or manifest.get("profile_time_unit") != "ticks":
        raise ProfileError("selector did not use measured ticks for time ranking")
    if manifest.get("profile_time_data") != "complete":
        raise ProfileError("selector time data is not complete")
    if manifest.get("hardware_time_claim") is not False:
        raise ProfileError("selector manifest makes a hardware timing claim")
    if manifest.get("total_measured_time") != total_time:
        raise ProfileError("selector total time does not match the CSV")
    aot_time = sum(rows[addr]["ticks"] for addr in selected)
    ir_time = total_time - aot_time
    covered_share = aot_time / total_time
    split = manifest.get("aot_ir_time_split", {})
    if split.get("basis") != "measured_ticks" or split.get("aot") != aot_time or split.get("ir") != ir_time:
        raise ProfileError("selector AOT/IR time split does not match the CSV")
    if manifest.get("covered_time_share") != round(covered_share, 6):
        raise ProfileError("selector covered time share does not match the CSV")
    if not 0 < aot_time < total_time:
        raise ProfileError("expected a nontrivial AOT/IR time split")

    aot_total = sum(as_int(item["aot_ticks"], "aot_ticks") for item in meta_functions.values())
    warm_total = sum(as_int(item["ir_warm_ticks"], "ir_warm_ticks") for item in meta_functions.values())
    cold_total = sum(as_int(item["ir_cold_ticks"], "ir_cold_ticks") for item in meta_functions.values())
    report = {
        "format_version": 1,
        "fixture_only": True,
        "full_game_profile": False,
        "timing_scope": "Azahar emulator; not New 3DS hardware",
        "hardware_time_claim": False,
        "clock_source": "svcGetSystemTick",
        "profile_time_unit": "ticks",
        "canonical_tier": "aot_warm",
        "verification": "pass",
        "profile_sha256": sha256_file(args.profile),
        "metadata_sha256": sha256_file(args.metadata),
        "profile_functions": count,
        "profile_hits": sum(row["hits"] for row in rows.values()),
        "selector_limit": args.limit,
        "selector_partition": {
            "ranking_mode": "time",
            "aot_functions": len(selected),
            "ir_functions": count - len(selected),
            "aot_ticks": aot_time,
            "ir_ticks": ir_time,
            "covered_time_share": covered_share,
            "aot_time_share": aot_time / total_time,
            "ir_time_share": ir_time / total_time,
            "aot_addresses": [f"0x{addr:08x}" for addr in selected],
        },
        "measured_execution_totals_not_selector_split": {
            "aot_warm_ticks": aot_total,
            "ir_warm_ticks": warm_total,
            "ir_cold_ticks": cold_total,
            "ir_warm_over_aot": warm_total / aot_total,
            "note": "Tier execution comparison; distinct from the selector AOT/IR partition above",
        },
    }
    (args.out / "check.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print("armtime check: PASS (verified Azahar emulator fixture)")
    print(f"  profile: {count} functions, {report['profile_hits']} hits, {total_time} warm AOT ticks")
    print(f"  selector: top {args.limit} by measured time")
    print(f"  covered time share: {100.0 * covered_share:.2f}%")
    print(f"  selector AOT/IR split: AOT {aot_time} ticks ({100.0 * covered_share:.2f}%), "
          f"IR {ir_time} ticks ({100.0 * (1.0 - covered_share):.2f}%)")
    print(f"  measured tiers: AOT {aot_total} ticks, IR warm {warm_total} ticks "
          f"({warm_total / aot_total:.2f}x); this is not the selector split")
    print("  scope: emulator timing only; no New 3DS hardware claim")
    print(f"  wrote {args.out / 'check.json'}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    collect = sub.add_parser("collect", help="archive bounded guest/Azahar outputs")
    collect.add_argument("--sd-root", type=Path, required=True)
    collect.add_argument("--out", type=Path, required=True)
    collect.add_argument("--azahar-log", type=Path,
                         default=Path.home() / "AppData" / "Roaming" / "Azahar" / "log" / "azahar_log.txt")
    collect.add_argument("--max-log-bytes", type=int, default=131072)
    collect.add_argument("--max-result-bytes", type=int, default=262144)
    collect.set_defaults(func=collect_command)

    check = sub.add_parser("check", help="validate, rank with ir_select.py, and report the split")
    check.add_argument("--profile", type=Path, required=True)
    check.add_argument("--metadata", type=Path, required=True)
    check.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    check.add_argument("--selector", type=Path, default=DEFAULT_SELECTOR)
    check.add_argument("--out", type=Path, required=True)
    check.add_argument("--limit", type=int, default=5,
                       help="AOT function limit for the selector check (default: 5)")
    check.set_defaults(func=check_command)

    args = ap.parse_args(argv)
    if args.command == "collect":
        if args.max_log_bytes <= 0 or args.max_result_bytes <= 0:
            ap.error("archive byte limits must be positive")
    try:
        return args.func(args)
    except (ProfileError, OSError) as exc:
        print(f"arm_profile: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
