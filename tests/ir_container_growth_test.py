"""Targeted growth-style verification against the real 90,900-function container.

Runs tests/ir_container_growth.c with the integrated runtime:
  - AOT table has only a dummy, no slice IR table;
  - TESV 0x401860 (`mov eax,0x67; ret`) exists only in the container;
  - repeated invoke proves container entry + record cache, not lifting;
  - required-missing and required-corrupt produce named bind faults.

Usage:
  .venv/Scripts/python.exe tests/ir_container_growth_test.py
  ... --container E:/Skyrim_N3DS_build/ir_container/tesv.irc --repeat 200000
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLANG = Path(r"C:\Program Files\LLVM\bin\clang.exe")
TARGET = 0x401860


def parse_index(path):
    data = Path(path).read_bytes()
    if len(data) < 64 or data[:8] != b"IRCNT001":
        raise SystemExit("bad container")
    nfun = struct.unpack_from("<I", data, 20)[0]
    for i in range(nfun):
        off = 64 + i * 40
        addr, end, flags, status, blocks, insns, ro, rs, rc, _ = \
            struct.unpack_from("<IIIHHIIIII", data, off)
        if addr == TARGET:
            return {"addr": addr, "end": end, "flags": flags, "status": status,
                    "blocks": blocks, "insns": insns, "record_offset": ro,
                    "record_size": rs, "record_crc": rc}
    return None


def run(cmd, env=None):
    t0 = time.perf_counter()
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                       env=env)
    return p, time.perf_counter() - t0


def build(clang, out):
    srcs = ["tests/ir_container_growth.c", "source/guest.c",
            "source/ir_container.c", "source/ir_decode.c", "source/ir_exec.c",
            "source/ir_core.c", "source/n3ds_lift_runtime.c", "staged/fpu.c"]
    cmd = [clang, "-std=gnu11", "-O1", "-g", "-Istaged", "-Isource", "-Itests",
           "-fwrapv", "-fno-strict-aliasing", "-Wno-deprecated-declarations",
           "-Wno-format", "-Wno-unused-function", "-Wno-unused-variable"]
    cmd += srcs + ["-o", str(out)]
    p = run(cmd)[0]
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("growth test build failed")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--container", type=Path,
                    default=Path(r"E:\Skyrim_N3DS_build\ir_container\tesv.irc"))
    ap.add_argument("--repeat", type=int, default=200000)
    ap.add_argument("--out", type=Path, default=ROOT / "build_ir_container")
    args = ap.parse_args()
    if not args.container.exists():
        raise SystemExit(f"missing container: {args.container}")
    clang = CLANG if CLANG.exists() else shutil.which("clang")
    if not clang:
        raise SystemExit("clang not found")

    entry = parse_index(args.container)
    if not entry:
        raise SystemExit(f"target {TARGET:08x} not in container index")
    if not (entry["flags"] & 1) or entry["status"] != 0:
        raise SystemExit(f"target is not decoded: {entry}")
    print(f"container {args.container} ({args.container.stat().st_size:,} bytes)")
    print(f"target {TARGET:08x}: record {entry['record_size']} bytes, "
          f"{entry['blocks']} block(s), {entry['insns']} insn(s)")

    args.out.mkdir(parents=True, exist_ok=True)
    exe = build(clang, args.out / "ir_container_growth.exe")

    env = dict(os.environ)
    env["N3DS_IRC_CACHE_RECORDS"] = "1"
    p, wall = run([exe, args.container, str(args.repeat)], env=env)
    print(p.stdout.rstrip())
    if p.returncode != 0 or "IR container growth PASS" not in p.stdout:
        print(p.stderr)
        return 1
    stats = {}
    for line in p.stdout.splitlines():
        if line.startswith("stats:"):
            for k, v in re.findall(r"([a-z_]+)\s+(\d+)", line):
                stats[k] = int(v)
    if stats.get("loads", 0) != 1:
        print("FAIL: expected exactly one record load")
        return 1
    if stats.get("redecodes", -1) != 0 or stats.get("block_misses", -1) != 0:
        print("FAIL: redecodes/block_misses not zero")
        return 1
    if args.repeat > 1 and stats.get("hits", 0) < args.repeat - 1:
        print("FAIL: record cache did not hit on repeated invokes")
        return 1
    print(f"wall: {wall * 1000.0:.3f} ms, "
          f"{wall * 1e6 / args.repeat:.3f} us/invoke")

    # Required-missing and required-corrupt must be named bind faults.
    req = dict(os.environ)
    req["N3DS_IRC_REQUIRED"] = "1"
    missing = args.out / "missing_growth.irc"
    p, _ = run([exe, missing, "1", "require-missing"], env=req)
    print(p.stdout.rstrip())
    if p.returncode != 0 or "IR container unavailable" not in p.stdout:
        print(p.stderr)
        return 1
    bad = args.out / "corrupt_growth.irc"
    blob = bytearray(args.container.read_bytes())
    blob[64 + 7] ^= 0x01
    bad.write_bytes(blob)
    p, _ = run([exe, bad, "1", "require-missing"], env=req)
    print(p.stdout.rstrip())
    if p.returncode != 0 or "index crc mismatch" not in p.stdout:
        print(p.stderr)
        return 1

    report = {
        "container": str(args.container),
        "container_bytes": args.container.stat().st_size,
        "target": f"0x{TARGET:08x}",
        "target_record": entry,
        "repeat": args.repeat,
        "wall_ms": wall * 1000.0,
        "us_per_invoke": wall * 1e6 / args.repeat,
        "stats": stats,
    }
    (args.out / "growth_report.json").write_text(json.dumps(report, indent=1),
                                                 encoding="utf-8")
    print("IR container growth test PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
