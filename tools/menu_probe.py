#!/usr/bin/env python3
"""Compact report for one host-slice menu run.

Parses the stderr log produced with the diagnostic env set and answers the
questions that decide "why is there no menu geometry" in one screen:

  * how many Present / Draw* calls happened and of which type;
  * the 0xf874c0 2D wrapper frames and their alpha gate byte;
  * every .swf/.gfx/.bsa open and read (path -> bytes/result), plus whether
    Interface\\StartMenu.swf or any archive range was read;
  * the last engine menu milestone (0x876f60/0x876b90/0xa60c80/0xa5ed20);
  * thread sleep states and the first fault / ExitProcess.

Usage:
    python tools/menu_probe.py run.log [--json out.json]

The parser is deliberately line-based and tolerant: the slice log mixes
hundreds of unrelated diagnostic lines.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, OrderedDict

DRAW_METHODS = (
    "DrawPrimitive",
    "DrawIndexedPrimitive",
    "DrawPrimitiveUP",
    "DrawIndexedPrimitiveUP",
)
MILESTONES = OrderedDict(
    [
        ("0x00876f60", "MainMenu::Show"),
        ("0x00876b90", "MainMenu::ctor"),
        ("0x00a60c10", "Menu::movieName"),
        ("0x00a60c80", "Menu::CreateMovie"),
        ("0x00a60200", "GFxLoader::CreateMovie"),
        ("0x00a5ed20", "BSScaleformFileOpener"),
    ]
)
# BSA member offsets are only meaningful for small/known archive entries; the
# probe reports the ranges it saw instead of guessing names.
FILE_OPEN_RE = re.compile(
    r"\[filetrace\] open '(?P<path>[^']*)' host='(?P<host>[^']*)' "
    r"handle=(?P<handle>[0-9a-f]+) size=(?P<size>-?\d+) caller=(?P<caller>[0-9a-f]+)"
)
FILE_FAIL_RE = re.compile(
    r"\[filetrace\] open FAIL '(?P<path>[^']*)' host='(?P<host>[^']*)' "
    r"errno=(?P<errno>-?\d+) caller=(?P<caller>[0-9a-f]+)"
)
FILE_READ_RE = re.compile(
    r"\[filetrace\] read h=(?P<handle>[0-9a-f]+) off=(?P<off>-?\d+) "
    r"n=(?P<n>\d+) got=(?P<got>\d+) caller=(?P<caller>[0-9a-f]+) path=(?P<path>.*)"
)
DRAWPROBE_RE = re.compile(
    r"\[drawprobe\] (?P<method>\w+) caller=(?P<caller>[0-9a-f]+) "
    r"wr=(?P<wr>[0-9a-f]+) this=(?P<this>[0-9a-f]+) m28=(?P<m28>\d+) "
    r"arg0=(?P<arg0>[0-9a-f]+) gate=(?P<gate>[0-9a-f]+)"
)
MENUTRACE_RE = re.compile(
    r"\[menutrace\] (?P<what>[^ ]+)(?: url='(?P<url>[^']*)')? caller=(?P<caller>[0-9a-f]+)"
)
TRACE_CALL_RE = re.compile(
    r"\[d3d9-trace\] (?P<iface>\w+)::(?P<method>\w+) slot=\d+ this=0x[0-9a-f]+"
    r"(?: -> 0x[0-9a-f]+)?"
)
SLEEP_RE = re.compile(r"\[sync\] (?:Sleep|SleepEx) (?P<ms>\d+) ms from (?P<from>[0-9a-f]+) thread=(?P<thread>\d+)")
FAULT_RE = re.compile(r"first fault: (?P<fault>.*)")
EXIT_RE = re.compile(r"\[guest\] ExitProcess\((?P<code>\d+)\) from (?P<from>[0-9a-f]+)")


def parse(lines):
    out = {
        "presents": 0,
        "draws": Counter(),
        "draw_calls": [],
        "wrapper": {"calls": 0, "gates": Counter(), "arg0": Counter(), "frames": []},
        "opens": [],
        "opens_failed": [],
        "reads": [],
        "bsa_ranges": {},
        "milestones": [],
        "opener_urls": [],
        "thread_sleeps": [],
        "first_fault": None,
        "exit": None,
        "d3d_calls": Counter(),
        "events_before_first_present": 0,
    }
    seen_present = False
    for line in lines:
        if not seen_present:
            out["events_before_first_present"] += 1
        m = FILE_OPEN_RE.search(line)
        if m:
            d = m.groupdict()
            d["size"] = int(d["size"])
            out["opens"].append(d)
            continue
        m = FILE_FAIL_RE.search(line)
        if m:
            d = m.groupdict()
            d["errno"] = int(d["errno"])
            out["opens_failed"].append(d)
            continue
        m = FILE_READ_RE.search(line)
        if m:
            d = m.groupdict()
            d["off"] = int(d["off"])
            d["n"] = int(d["n"])
            d["got"] = int(d["got"])
            out["reads"].append(d)
            lo, hi = d["off"], d["off"] + d["got"]
            prev = out["bsa_ranges"].get(d["path"])
            if prev is None:
                out["bsa_ranges"][d["path"]] = [lo, hi]
            else:
                prev[0] = min(prev[0], lo)
                prev[1] = max(prev[1], hi)
            continue
        m = DRAWPROBE_RE.search(line)
        if m:
            d = m.groupdict()
            out["wrapper"]["calls"] += 1
            out["wrapper"]["gates"][d["gate"]] += 1
            out["wrapper"]["arg0"][d["arg0"]] += 1
            if len(out["wrapper"]["frames"]) < 8:
                out["wrapper"]["frames"].append(d)
            continue
        m = MENUTRACE_RE.search(line)
        if m:
            d = m.groupdict()
            out["milestones"].append({"what": d["what"], "caller": d["caller"]})
            if d["url"]:
                out["opener_urls"].append(d["url"])
            continue
        m = SLEEP_RE.search(line)
        if m:
            out["thread_sleeps"].append(m.groupdict())
            continue
        m = FAULT_RE.search(line)
        if m:
            out["first_fault"] = m.group("fault").strip()
            continue
        m = EXIT_RE.search(line)
        if m:
            out["exit"] = {"code": int(m.group("code")), "from": m.group("from")}
            continue
        m = TRACE_CALL_RE.search(line)
        if m:
            method = m.group("method")
            out["d3d_calls"][method] += 1
            if method == "Present":
                out["presents"] += 1
                seen_present = True
            elif method in DRAW_METHODS:
                out["draws"][method] += 1
                if len(out["draw_calls"]) < 16:
                    out["draw_calls"].append(m.groupdict())
            continue
        if "::Present" in line:
            # trace detail lines also carry the method; the call line above is
            # authoritative, but keep a fallback for traces without slot text.
            if "slot=" not in line:
                pass
    return out


def report(result, log_name=""):
    lines = []
    add = lines.append
    add(f"# menu probe {log_name}".rstrip())
    add("")
    add(f"presents           : {result['presents']}")
    if result["draws"]:
        add("draws              : " + ", ".join(f"{k}={v}" for k, v in sorted(result["draws"].items())))
    else:
        add("draws              : none")
    w = result["wrapper"]
    add(f"wrapper 0xf874c0   : calls={w['calls']} gates={dict(w['gates'])} arg0={dict(w['arg0'])}")
    add(f"milestones         : {', '.join(m['what'] for m in result['milestones']) or 'none'}")
    if result["opener_urls"]:
        add(f"GFx opener urls    : {', '.join(result['opener_urls'])}")
    else:
        add("GFx opener urls    : none (BSScaleformFileOpener was not reached)")
    add("")
    add("file opens (title-relevant):")
    if not result["opens"] and not result["opens_failed"]:
        add("  none")
    for d in result["opens"]:
        add(f"  ok   {d['path']}  size={d['size']} handle={d['handle']} caller={d['caller']}")
    for d in result["opens_failed"]:
        add(f"  FAIL {d['path']} errno={d['errno']} caller={d['caller']}")
    add("")
    add("archive/SWF byte ranges:")
    if not result["bsa_ranges"]:
        add("  none")
    for path, (lo, hi) in sorted(result["bsa_ranges"].items()):
        add(f"  {path}: [{lo}, {hi}) = {hi - lo} bytes")
    add("")
    if result["first_fault"]:
        add(f"first fault        : {result['first_fault']}")
    elif result["exit"]:
        add(f"exit               : ExitProcess({result['exit']['code']}) from {result['exit']['from']}")
    else:
        add("first fault        : none (run still alive or truncated log)")
    if result["thread_sleeps"]:
        last = result["thread_sleeps"][-1]
        add(f"last thread sleep  : {last['ms']} ms from {last['from']} thread={last['thread']}")
    else:
        add("last thread sleep  : none")
    add(f"events before 1st Present: {result['events_before_first_present']}")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="host-slice stderr log (TESV_FILE_TRACE/TESV_MENU_TRACE/TESV_DRAW_PROBE)")
    ap.add_argument("--json", help="write the parsed structure as JSON")
    args = ap.parse_args(argv)
    with open(args.log, "r", encoding="utf-8", errors="replace") as f:
        result = parse(f)
    print(report(result, args.log))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
