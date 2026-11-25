#!/usr/bin/env python3
"""Annotate a saved TESV host run against a generated slice.

Input is a run directory produced by tools/run_host_tests.ps1 / grow16_manual.ps1
(probe_run.log and, when profiling was enabled, tesv_profile.csv).  The tool
turns the single first-fault line into table context:

  * which function contains the fault address and whether the slice has it,
  * which .rdata/.data pointer slot referenced it,
  * the ordered table walk before/after that slot,
  * whether every non-NULL slot before it really was in the slice (the
    observed prefix), and
  * the next expected initializer targets.

It is read-only and diagnostic.  It never launches the game and never edits a
slice, a growth script or the runtime.

Example
-------
  python tools/annotate_host_run.py ^
    E:\\Skyrim_N3DS_build\\analysis\\growth21\\iter04 ^
    --slice E:\\Skyrim_N3DS_build\\analysis\\growth21\\iter04\\slice_functions.json ^
    --markdown E:\\Skyrim_N3DS_build\\init_trace\\growth21_iter04.md
"""

import argparse
import collections
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from init_trace import (  # noqa: E402
    Image, classify_target, load_slice, load_starts, table_rows,
)

DEFAULT_TABLE = "0x106b464:0x1076a98"   # C++ dynamic-initializer table (this build)


def parse_run(run_dir):
    run = Path(run_dir)
    log = run / "probe_run.log" if run.is_dir() else run
    text = log.read_text(encoding="utf-8", errors="replace")
    out = {
        "log": str(log),
        "first_fault": None,
        "fault": None,
        "reason": None,
        "tls": None,
        "tls_callbacks_ran": None,
        "tls_non_null": None,
        "exit": None,
        "mem": [],
        "unresolved": [],
        "unresolved_total": None,
        "profile": None,
    }
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("first fault:"):
            out["first_fault"] = s
            m = re.search(r"Guest fault at ([0-9a-fA-F]+):\s*(.*)", s)
            if m:
                out["fault"] = int(m.group(1), 16)
                out["reason"] = m.group(2).strip()
        elif "TLS callbacks" in s:
            out["tls"] = s
        elif s.startswith("unresolved imports bound to fault trap:"):
            try:
                out["unresolved_total"] = int(s.rsplit(":", 1)[1].strip())
            except ValueError:
                pass
        elif "ExitProcess" in s and out["exit"] is None:
            out["exit"] = s
        elif s.startswith("[mem] va ") or s.startswith("[mem] pages:"):
            out["mem"].append(s)
        elif s.startswith("unresolved import "):
            out["unresolved"].append(s.split("unresolved import ", 1)[1])
    profile = run / "tesv_profile.csv" if run.is_dir() else None
    if profile and profile.exists():
        rows = []
        for i, line in enumerate(profile.read_text(encoding="utf-8").splitlines()):
            if i == 0 or not line:
                continue
            a, h = line.split(",")[:2]
            rows.append((int(a, 16), int(h)))
        rows.sort(key=lambda x: -x[1])
        out["profile"] = {"distinct": len(rows), "rows": rows}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--exe", default=r"E:\skyr\TESV.exe")
    ap.add_argument("--functions", default=r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json")
    ap.add_argument("--slice", default=None)
    ap.add_argument("--table", default=DEFAULT_TABLE, help="initializer table base:end")
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--json", default=None)
    ap.add_argument("--markdown", default=None)
    args = ap.parse_args()

    run = parse_run(args.run_dir)
    img = Image(args.exe)
    starts, _ = load_starts(args.functions)
    slset = load_slice(args.slice)

    # TLS callback reality: the loader stops at the first NULL entry.
    try:
        tls = img.pe.DIRECTORY_ENTRY_TLS.struct
        cbs = []
        for i in range(16):
            v = img.u32(tls.AddressOfCallBacks + i * 4)
            if v is None:
                break
            cbs.append(v)
            if i > 0 and v == 0 and img.u32(tls.AddressOfCallBacks + (i + 1) * 4) == 0:
                break
        ran = 0
        for v in cbs:
            if v == 0:
                break
            ran += 1
        run["tls_callbacks_ran"] = ran
        run["tls_non_null"] = sum(1 for v in cbs if v)
        run["tls_array"] = {"va": tls.AddressOfCallBacks, "values": cbs}
    except Exception:
        pass
    base, end = (int(x, 16) for x in args.table.split(":"))
    rows = table_rows(img, base, end, starts, slset)
    nz = [r for r in rows if not r["null"]]
    by_target = collections.defaultdict(list)
    for r in nz:
        by_target[r["target"]].append(r)

    lines = []
    add = lines.append

    add("# Host run annotation")
    add("")
    add("- log: `%s`" % run["log"])
    if args.slice:
        add("- slice: `%s` (%d functions)" % (args.slice, len(slset)))
    add("- initializer table: `0x%08x..0x%08x` (%d slots, %d non-NULL)"
        % (base, end, (end - base) // 4, len(nz)))
    add("")
    add("## Measured run facts")
    add("")
    if run["first_fault"]:
        add("- first fault: `%s`" % run["first_fault"])
    if run["fault"] is not None:
        add("- fault address: `0x%08x` (%s)" % (run["fault"], img.region(run["fault"])))
    if run["tls"]:
        add("- TLS runtime message: `%s`" % run["tls"])
    if run.get("tls_array"):
        add("- TLS callback array at `0x%08x`: %s" % (
            run["tls_array"]["va"],
            ", ".join("0x%08x" % v for v in run["tls_array"]["values"])))
        add("- loader callbacks executed (stops at first NULL): **%d**; "
            "non-NULL pointers in the array: %d"
            % (run.get("tls_callbacks_ran") or 0, run.get("tls_non_null") or 0))
    if run["exit"]:
        add("- exit: `%s`" % run["exit"])
    for m in run["mem"]:
        add("- `%s`" % m)
    if run["unresolved_total"] is not None:
        add("- unresolved imports bound to trap: %d" % run["unresolved_total"])
    elif run["unresolved"]:
        add("- unresolved imports bound to trap: %d" % len(run["unresolved"]))
    if run["unresolved"]:
        add("  - first: " + ", ".join("`%s`" % x for x in run["unresolved"][:8]))
    add("")

    payload = dict(run)
    payload["table"] = {"base": base, "end": end, "non_null": len(nz)}
    payload["slots"] = []

    fault = run["fault"]
    if fault is not None:
        owners = [r for r in rows if r["target"] == fault]
        add("## Fault classification")
        add("")
        add("- function containing the fault: `0x%08x`" % (
            img.containing_function(fault, starts) if starts else 0))
        owner = img.containing_function(fault, starts) if starts else None
        add("- owner in slice: **%s**" % ("yes" if owner in slset else "no"))
        add("- target is a discovered function start: **%s**" % (
            "yes" if (not starts or fault in starts) else "no"))
        add("- target class: `%s`" % classify_target(img, fault))
        if not owners:
            add("- no initializer-table slot points at this address")
        else:
            add("- initializer slot(s): " + ", ".join(
                "`0x%08x` index %d" % (r["slot"], r["index"]) for r in owners))
            for r in owners:
                payload["slots"].append({"slot": r["slot"], "index": r["index"]})
                before = [x for x in nz if x["slot"] < r["slot"]]
                after = [x for x in nz if x["slot"] > r["slot"]]
                missing_prefix = [x for x in before if not x["in_slice"]]
                add("")
                add("### Slot `0x%08x` (index %d)" % (r["slot"], r["index"]))
                add("")
                add("- non-NULL slots before it: %d; in slice: %d; **missing: %d**"
                    % (len(before), len(before) - len(missing_prefix), len(missing_prefix)))
                if missing_prefix:
                    add("- WARNING: the slice is inconsistent with this run; first "
                        "missing prefix slot `0x%08x` -> `0x%08x`"
                        % (missing_prefix[0]["slot"], missing_prefix[0]["target"]))
                add("- previous non-NULL entries:")
                for x in before[-5:]:
                    add("  - index %d slot `0x%08x` -> `0x%08x` %s"
                        % (x["index"], x["slot"], x["target"], x["class"]))
                add("- next non-NULL entries (expected order):")
                for x in after[:args.top]:
                    add("  - index %d slot `0x%08x` -> `0x%08x` %s (slice=%s)"
                        % (x["index"], x["slot"], x["target"], x["class"],
                           "yes" if x["in_slice"] else "no"))
                add("")
                add("Observed status: `0x%08x` was the **first fault**; every one of "
                    "the %d non-NULL slots before it had to return for the walk to "
                    "reach this slot." % (r["target"], len(before)))
        add("")

    if run["profile"]:
        add("## Executed profile (measured, call counts)")
        add("")
        add("- distinct functions: %d" % run["profile"]["distinct"])
        add("")
        add("| function | hits | in slice |")
        add("|---|---:|---|")
        for a, h in run["profile"]["rows"][:args.top]:
            add("| `0x%08x` | %d | %s |" % (a, h, "yes" if a in slset else "no"))
        add("")

    text = "\n".join(lines) + "\n"
    print(text)
    if args.markdown:
        Path(args.markdown).parent.mkdir(parents=True, exist_ok=True)
        Path(args.markdown).write_text(text, encoding="utf-8")
        print("wrote %s" % args.markdown)
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print("wrote %s" % args.json)


if __name__ == "__main__":
    main()
