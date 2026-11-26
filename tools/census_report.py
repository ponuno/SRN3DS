#!/usr/bin/env python3
"""Compact triage for a TESV census / D3D9 trace log.

Agents otherwise read 1-4 MB of trace output.  This prints only the layer that
matters: the first fault with a little context, per-method D3D9 call/result
counts, the most common diagnostics, depth binds, the last trace lines and the
unresolved-import count.

Usage: python tools/census_report.py <log> [--context N] [--top N]
"""

import argparse
import collections
import io
import os
import re
import sys

TRACE_RE = re.compile(r"\[d3d9-trace\] ([A-Za-z0-9_]+)::([A-Za-z0-9_]+).*?-> (0x[0-9a-fA-F]+)")
CALL_RE = re.compile(r"\[d3d9-trace\] ([A-Za-z0-9_]+)::([A-Za-z0-9_]+)")
DIAG_RE = re.compile(r"^\s*(?:\[d3d9\])?\s*(?:diagnostic:\s*)?(.*(?:does not match|is not|not implemented|cannot|refus|failed|mismatch|requires).*)$", re.I)
FAULT_RE = re.compile(r"(Guest fault at [0-9a-fA-F]+: .*|first fault: .*|run rc=.*)")
BIND_RE = re.compile(r"Depth=(0x[0-9a-fA-F]+) (\d+)x(\d+) RenderTarget=(0x[0-9a-fA-F]+) (\d+)x(\d+) Implicit=(\d+)")
MSG_RE = re.compile(r"\[messagebox\] (.+)")


def read_lines(path, limit_mb=64):
    size = os.path.getsize(path)
    if size > limit_mb * 1024 * 1024:
        # Tail keeps the fault and the final state; the head holds startup gates.
        with io.open(path, "r", encoding="utf-8", errors="replace") as f:
            head = [next(f, "") for _ in range(400)]
            f.seek(max(0, size - (limit_mb // 2) * 1024 * 1024))
            tail = f.read().splitlines()
        return head + tail, size
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines(), size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--context", type=int, default=10)
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args()

    lines, size = read_lines(args.log)
    print(f"# census report: {args.log}")
    print(f"size={size/1024:.0f} KiB lines={len(lines)}")

    fault_idx = None
    for i, line in enumerate(lines):
        if line.startswith("Guest fault at") or line.startswith("first fault:") or line.startswith("run rc="):
            fault_idx = i
            break
    if fault_idx is not None:
        print("\n## first fault")
        for i in range(max(0, fault_idx - args.context), min(len(lines), fault_idx + 4)):
            print(f"  {i+1}: {lines[i][:180]}")
    else:
        print("\n## first fault\n  none (log may be truncated)")

    calls = collections.Counter()
    results = collections.defaultdict(collections.Counter)
    pending = collections.Counter()
    binds = []
    messages = []
    diags = collections.Counter()
    unresolved = 0
    last_trace = collections.deque(maxlen=12)

    for line in lines:
        m = TRACE_RE.search(line)
        if m:
            obj, method, code = m.group(1), m.group(2), m.group(3)
            key = f"{obj}::{method}"
            calls[key] += 1
            results[key][code.lower()] += 1
            last_trace.append(line.strip())
            continue
        m = CALL_RE.search(line)
        if m:
            key = f"{m.group(1)}::{m.group(2)}"
            if "->" not in line:
                pending[key] += 1
            last_trace.append(line.strip())
            continue
        if "unresolved import " in line:
            unresolved += 1
        m = BIND_RE.search(line)
        if m:
            binds.append(line.strip())
        m = MSG_RE.search(line)
        if m:
            messages.append(m.group(1).strip())
        if "diagnostic:" in line or line.lstrip().startswith("[d3d9] ") and ("not " in line or "refus" in line):
            text = line.split("diagnostic:", 1)[-1].strip()
            text = re.sub(r"^\[d3d9\]\s*", "", text)
            text = re.sub(r"\s+", " ", text)
            if text and not text.startswith("MEASUREMENT ONLY"):
                diags[text[:160]] += 1

    print("\n## d3d9 methods")
    if not calls:
        print("  none")
    for key, n in calls.most_common(args.top):
        method = key.split("::", 1)[-1]
        if method in ("AddRef", "Release"):
            vals = ", ".join(f"{k}x{c}" for k, c in results[key].most_common(3))
            print(f"  {key:42s} calls={n:6d} refcounts: {vals}")
            continue
        ok = results[key].get("0x00000000", 0)
        bad = n - ok
        detail = ""
        if bad:
            worst = ", ".join(f"{c}x{k}" for k, c in results[key].most_common(4) if k != "0x00000000")
            detail = f"  failures={bad} ({worst})"
        unret = pending[key] - sum(results[key].values())
        if unret > 0:
            detail += f"  unreturned={unret}"
        print(f"  {key:42s} calls={n:6d} ok={ok:6d}{detail}")

    if messages:
        print("\n## messagebox")
        for text in collections.Counter(messages).most_common(5):
            print(f"  {text[1]}x {text[0][:200]}")

    if binds:
        print("\n## depth binds")
        print(f"  count={len(binds)}")
        for text in binds[:3]:
            print(f"  {text[:180]}")

    if diags:
        print("\n## diagnostics")
        for text, n in diags.most_common(args.top):
            print(f"  {n:5d}x {text}")

    if last_trace:
        print("\n## last trace lines")
        for text in last_trace:
            print(f"  {text[:180]}")

    print(f"\n## unresolved imports: {unresolved}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
