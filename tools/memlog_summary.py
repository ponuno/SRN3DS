#!/usr/bin/env python3
"""Parse mem_stats snapshots from logs into a table, deltas and a summary.

Accepts device/host logs containing lines of the form documented in
``docs/MEMORY_BUDGET_178.md`` (``source/mem_stats.c`` writes them)::

    mem tag=<boot|arenas|start|init|fault|exit|frame|cell|alloc_fail> key=value ...

Lines can be embedded in other log output (prefixes are ignored), and several
logs can be summarised in one run. Byte values in the text table are MiB with
two decimals; ``--json`` keeps the exact byte/count values.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

MEM_LINE = re.compile(r"\bmem\s+tag=([A-Za-z_][\w-]*)")

# (counter, text-table header). Order is the table column order.
TABLE_FIELDS = (
    ("guest_used", "guest_used"),
    ("guest_peak", "guest_peak"),
    ("irc_bytes", "irc"),
    ("irdec_bytes", "irdec"),
    ("bsa_resident", "bsa_res"),
    ("tex_guest", "tex_g"),
    ("tex_gpu", "tex_gpu"),
    ("alloc_failures", "alloc_fail"),
    ("va_backed_peak", "va_back_pk"),
)

# Fields reported as init -> fault/exit deltas. A few adjacent gauges are
# included because they explain a change in the table columns above.
DELTA_FIELDS = (
    "guest_used", "guest_peak", "guest_regions", "irc_bytes", "irdec_bytes",
    "bsa_resident", "tex_guest", "tex_gpu", "tex_guest_peak", "tex_gpu_peak",
    "linear_live", "alloc_calls", "alloc_failures", "va_committed_peak",
    "va_backed_peak",
)

MIB = 1024 * 1024


def parse_value(text: str) -> int | str:
    """Counters are decimal integers; extra keys may be plain strings."""
    try:
        return int(text, 10)
    except ValueError:
        return text


def parse_log(path: Path) -> list[dict]:
    rows: list[dict] = []
    try:
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            for lineno, line in enumerate(fh, 1):
                match = MEM_LINE.search(line)
                if not match:
                    continue
                row: dict = {"source": str(path), "line": lineno,
                             "tag": match.group(1)}
                for token in line[match.end():].split():
                    if "=" not in token:
                        continue
                    key, value = token.split("=", 1)
                    row[key] = parse_value(value)
                rows.append(row)
    except OSError as exc:
        print(f"warn: cannot read {path}: {exc}", file=sys.stderr)
    return rows


def number(row: dict | None, key: str) -> int | None:
    if not row:
        return None
    value = row.get(key)
    return value if isinstance(value, int) else None


def first_tag(rows: list[dict], *tags: str) -> dict | None:
    for tag in tags:
        for row in rows:
            if row["tag"] == tag:
                return row
    return None


def max_field(rows: list[dict], key: str) -> tuple[int | None, dict | None]:
    best_row = None
    best = None
    for row in rows:
        value = number(row, key)
        if value is not None and (best is None or value > best):
            best, best_row = value, row
    return best, best_row


def compute_deltas(rows: list[dict]) -> tuple[dict | None, dict]:
    """Baseline is the first `init` snapshot, falling back to `start`."""
    baseline = first_tag(rows, "init", "start")
    result: dict = {"baseline_tag": baseline["tag"] if baseline else None,
                    "baseline_line": baseline["line"] if baseline else None,
                    "by_tag": {}}
    for tag in ("fault", "exit"):
        target = first_tag(rows, tag)
        if not target:
            continue
        fields = {}
        for key in DELTA_FIELDS:
            before, after = number(baseline, key), number(target, key)
            fields[key] = {
                "init": before,
                "value": after,
                "delta": after - before
                if before is not None and after is not None else None,
            }
        result["by_tag"][tag] = {
            "line": target["line"],
            "fields": fields,
        }
    return baseline, result


def compute_summary(rows: list[dict]) -> dict:
    guest, guest_row = max_field(rows, "guest_peak")
    tex, tex_row = max_field(rows, "tex_gpu_peak")
    failures, _ = max_field(rows, "alloc_failures")
    return {
        "snapshots": len(rows),
        "peak_guest": guest,
        "peak_guest_at": source_ref(guest_row),
        "peak_tex_gpu": tex,
        "peak_tex_gpu_at": source_ref(tex_row),
        "alloc_failures": failures,
        "alloc_fail_events": sum(1 for r in rows if r["tag"] == "alloc_fail"),
    }


def source_ref(row: dict | None) -> str:
    if not row:
        return "-"
    return f"{Path(row['source']).name}:{row['line']} tag={row['tag']}"


def fmt_bytes(value: int | None, signed: bool = False) -> str:
    if value is None:
        return "-"
    return f"{value / MIB:+.2f}" if signed else f"{value / MIB:.2f}"


def fmt_count(value: int | None, signed: bool = False) -> str:
    if value is None:
        return "-"
    return f"{value:+d}" if signed else str(value)


def print_table(headers: list[str], rows: list[list[str]]) -> None:
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))
    print("  ".join(h.ljust(widths[i]) for i, h in enumerate(headers)))
    print("  ".join("-" * w for w in widths))
    for row in rows:
        print("  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)))


def print_text(logs: list[Path], grouped: list[tuple[Path, list[dict]]],
               all_rows: list[dict], delta_sets: list[dict]) -> None:
    print(f"mem snapshots: {len(all_rows)} from "
          f"{', '.join(Path(path).name for path in logs)}")
    print("values: MiB (2 d.p.); alloc_fail/alloc_calls are counts\n")

    headers = ["tag"]
    if len(logs) > 1:
        headers.append("log")
    headers.extend(label for _, label in TABLE_FIELDS)
    table: list[list[str]] = []
    for row in all_rows:
        cells = [row["tag"]]
        if len(logs) > 1:
            cells.append(Path(row["source"]).name)
        for key, _ in TABLE_FIELDS:
            cells.append(fmt_count(row[key]) if key == "alloc_failures"
                         else fmt_bytes(row.get(key)))
        table.append(cells)
    print_table(headers, table)

    print("\nDeltas (init -> fault/exit):")
    for path, deltas in zip((p for p, _ in grouped), delta_sets):
        if len(logs) > 1:
            print(f"  {Path(path).name} (baseline tag={deltas['baseline_tag']}, "
                  f"line={deltas['baseline_line']}):")
        rows: list[list[str]] = []
        header = ["tag", "field", "init", "value", "delta"]
        for tag, entry in deltas["by_tag"].items():
            for key, values in entry["fields"].items():
                cells = [tag, key]
                formatter = fmt_count if key in ("alloc_failures", "alloc_calls",
                                                  "guest_regions") else fmt_bytes
                cells.extend([formatter(values["init"]),
                              formatter(values["value"]),
                              formatter(values["delta"], signed=True)])
                rows.append(cells)
        if rows:
            print_table(header, rows)
        else:
            print("  (no fault/exit snapshot)")
        print()

    summary = compute_summary(all_rows)
    print("Summary:")
    print(f"  peak guest      : {fmt_bytes(summary['peak_guest'])} MiB "
          f"@ {summary['peak_guest_at']}")
    print(f"  peak tex_gpu    : {fmt_bytes(summary['peak_tex_gpu'])} MiB "
          f"@ {summary['peak_tex_gpu_at']}")
    print(f"  alloc_failures  : {summary['alloc_failures']} "
          f"(alloc_fail events: {summary['alloc_fail_events']})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-Log", "--log", dest="logs", action="append",
                    metavar="PATH", help="log file to parse (repeatable)")
    ap.add_argument("extra_logs", nargs="*", metavar="PATH",
                    help="additional log files")
    ap.add_argument("--json", action="store_true",
                    help="emit parsed snapshots, deltas and summary as JSON")
    args = ap.parse_args()

    logs = [Path(p) for p in (args.logs or []) + args.extra_logs]
    if not logs:
        ap.error("at least one -Log PATH is required")

    grouped: list[tuple[Path, list[dict]]] = []
    for path in logs:
        rows = parse_log(path)
        if not rows and not path.exists():
            print(f"warn: no such log: {path}", file=sys.stderr)
        grouped.append((path, rows))
    all_rows = [row for _, rows in grouped for row in rows]
    delta_sets = [compute_deltas(rows)[1] for _, rows in grouped]

    if args.json:
        payload = {
            "logs": [str(p) for p in logs],
            "snapshots": all_rows,
            "deltas": [
                {"log": str(path), **deltas}
                for (path, _), deltas in zip(grouped, delta_sets)
            ],
            "summary": compute_summary(all_rows),
        }
        print(json.dumps(payload, indent=2))
    elif all_rows:
        print_text(logs, grouped, all_rows, delta_sets)
    else:
        print("no mem tag= snapshots found in the given logs", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
