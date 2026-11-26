#!/usr/bin/env python3
"""Summarise TESV_D3D9_SHADER_USAGE JSON lines.

The env is implemented in source/d3d9_com.c and emits one line per
CreateVertexShader/CreatePixelShader:

    [shader-usage] {"stage":"ps","sha256":"...","bytes":200,
                    "caller":"0x00f883df","result":"0x00000000",
                    "plan":"null","plan_hit":0}

This tool deduplicates by (stage, sha256), matches docs/shader_plans.json and
separates GFx's built-in menu shaders (caller around 0xf8838c/0xf883df) from the
engine's Shaders001/002 preload (caller 0xccbbc5).

Usage:
    python tools/menu_shader_usage.py LOG [--json OUT]
"""

import argparse
import json
import re
import sys
from pathlib import Path

LINE = re.compile(r"\[shader-usage\]\s+(\{.*)$")


def load_lines(path):
    """Parse JSON lines, re-joining hard wraps introduced by PowerShell."""
    rows = []
    pending = None
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = LINE.search(line)
        if m:
            pending = m.group(1)
        elif pending is not None and line and not line.startswith("["):
            pending += line.strip()
        else:
            pending = None
            continue
        try:
            rows.append(json.loads(pending))
            pending = None
        except json.JSONDecodeError:
            continue
    return rows


def classify(caller):
    try:
        va = int(caller, 16)
    except (TypeError, ValueError):
        return "unknown"
    if 0xF88000 <= va < 0xF89000:
        return "gfx"
    if 0xCCBB00 <= va < 0xCCC000:
        return "shaderfx_preload"
    return "other"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log")
    ap.add_argument("--json", default="")
    ap.add_argument("--plans", default=str(Path(__file__).resolve().parent.parent /
                                          "docs" / "shader_plans.json"))
    args = ap.parse_args(argv)

    rows = load_lines(args.log)
    plans = {}
    if Path(args.plans).exists():
        for e in json.loads(Path(args.plans).read_text(encoding="utf-8"))["entries"]:
            plans[(e["stage"][:2], e["sha256"])] = e

    unique = {}
    for r in rows:
        key = (r.get("stage"), r.get("sha256"))
        u = unique.setdefault(key, {
            "stage": r.get("stage"), "sha256": r.get("sha256"),
            "bytes": r.get("bytes", 0), "calls": 0,
            "results": set(), "callers": set(), "classes": set(),
            "addrs": set(), "plan": r.get("plan"),
            "plan_hit": r.get("plan_hit", 0),
        })
        u["calls"] += 1
        u["results"].add(r.get("result"))
        u["callers"].add(r.get("caller"))
        u["addrs"].add(r.get("addr"))
        u["classes"].add(classify(r.get("caller")))
        if r.get("plan_hit"):
            u["plan"] = r.get("plan")
            u["plan_hit"] = 1

    out = []
    for u in unique.values():
        covered = None
        for e in plans.values():
            if e["sha256"] == u["sha256"] and e["stage"][:2] == u["stage"]:
                covered = e
                break
        out.append({
            "stage": u["stage"], "sha256": u["sha256"], "bytes": u["bytes"],
            "calls": u["calls"], "results": sorted(u["results"]),
            "callers": sorted(u["callers"]),
            "addrs": sorted(a for a in u["addrs"] if a),
            "class": "/".join(sorted(u["classes"])),
            "plan_hit": u["plan_hit"], "plan_id": u["plan"],
            "in_shader_plans": bool(covered),
            "plan_draw_enabled": covered.get("draw_enabled") if covered else None,
            "plan_family": covered.get("family") if covered else None,
        })
    out.sort(key=lambda r: (r["class"] != "gfx", r["stage"], -r["calls"], r["sha256"]))

    print("shader-usage lines: %d, unique: %d" % (len(rows), len(out)))
    for cls in ("gfx", "shaderfx_preload", "other", "unknown"):
        sel = [r for r in out if r["class"] == cls]
        if not sel:
            continue
        for stage in ("vs", "ps"):
            s = [r for r in sel if r["stage"] == stage]
            if not s:
                continue
            cov = sum(1 for r in s if r["in_shader_plans"])
            print("  %-17s %s: unique=%2d calls=%4d covered=%d"
                  % (cls, stage, len(s), sum(r["calls"] for r in s), cov))
    print("covered by shader_plans.json: %d / %d unique"
          % (sum(1 for r in out if r["in_shader_plans"]), len(out)))

    if args.json:
        Path(args.json).write_text(json.dumps({
            "source_log": str(args.log),
            "lines": len(rows),
            "unique": out,
        }, indent=2), encoding="utf-8")
        print("json: " + args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
