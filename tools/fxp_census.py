#!/usr/bin/env python3
"""Read-only FXP shader census and native-plan coverage (variant B, stage B0).

Reads the retail ``Skyrim - Shaders.bsa``, parses every ``.fxp`` package,
decodes each VS/PS chunk and classifies it against the PICA200 envelope using
``tools/shaders/compile.py``.

Writes derived metadata only (hashes + features, never bytecode):

* ``E:\\Skyrim_N3DS_build\\assets\\shaders\\fxp_census.json`` -- full per-hash
  table (local build artifact);
* ``docs/SHADER_CENSUS.json`` -- the small summary quoted in
  ``docs/SHADER_COMPILER.md``.

Usage::

    set PYTHONPATH=tools
    python tools\\fxp_census.py --data F:\\Skyrim\\Data
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter, defaultdict
from typing import Dict, List, Optional, Sequence

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import compile as C  # noqa: E402
from skyrim import bsa  # noqa: E402
from skyrim.shaders import parse_package, version_name  # noqa: E402

DEFAULT_DATA = [
    r"E:\SteamLibrary\steamapps\common\Skyrim\Data",
]
ARCHIVE = "Skyrim - Shaders.bsa"


def find_data(explicit: str = "") -> str:
    if explicit:
        return explicit
    for candidate in DEFAULT_DATA:
        if os.path.exists(os.path.join(candidate, ARCHIVE)):
            return candidate
    raise SystemExit("Shaders.bsa not found; pass --data")


def _refusal_bucket(why: str) -> str:
    if not why:
        return "ok"
    head = why.split(":", 1)[0]
    return head.strip()


def classify_chunk(chunk) -> dict:
    ir = C.build_ir(chunk.code)
    plan = None
    cls = None
    why = None
    if chunk.kind == "ps":
        # The certified menu-light signature is a pre-specialised plan: its
        # `def c8` is outside the LDR range, so check it before the generic
        # literal screen.
        plan = C.menu_light_plan(ir)
        if plan is not None:
            cls = "tev_menu_light"
        else:
            plan, cls, why = C.plan_ps(ir)
    else:
        plan, cls, why = C.plan_vs(ir)
    row = {
        "sha256": ir.sha256,
        "sha1": ir.sha1,
        "kind": chunk.kind,
        "version": ir.version_name,
        "version_token": "0x%08x" % ir.version,
        "bytes": chunk.size,
        "instructions": len(ir.body),
        "class": cls or _refusal_bucket(why),
        "refusal": why or "",
        "inputs": [{"usage": d.usage_name, "index": d.usage_index,
                    "reg": d.reg_index} for d in ir.program.declarations
                   if d.reg_type == 1],
        "samplers": [{"reg": d.reg_index, "type": d.texture_type}
                     for d in ir.program.declarations if d.reg_type == 10],
        "constants": len({c.register_index for c in ir.program.constants}),
        "texture_samples": sum(1 for i in ir.body
                               if i.opcode in (0x42, 0x5D, 0x5F)),
        "branches": sum(1 for i in ir.body
                        if i.opcode in (0x19, 0x1A, 0x1B, 0x1C, 0x1D,
                                        0x26, 0x27, 0x28, 0x29, 0x2A,
                                        0x2B, 0x2C, 0x2D, 0x60)),
        "opcodes": [i.name for i in ir.body
                    if i.opcode not in (0x1F, 0x51, 0x00)],
    }
    if plan is not None:
        row["plan"] = {
            "kind": plan["kind"],
            "stages": plan.get("stages"),
            "textures": len(plan.get("textures", [])),
            "constants": plan.get("constants", []),
            "family": plan.get("family", ""),
            "expandable": plan.get("expandable", []),
        }
    elif chunk.kind == "ps" and why and why.startswith("def_literal_out_of_range"):
        # Coverage ceiling, not a plan: what would fit if the LDR literal
        # range were rewritten?  The clamped descriptor is not emitted.
        p2, c2, w2 = C.plan_ps(ir, ignore_def_range=True)
        row["potential"] = c2 or _refusal_bucket(w2)
    return row


def census(data_dir: str, include_update: bool = False) -> dict:
    archives = [ARCHIVE] + (["Update.bsa"] if include_update else [])
    chunks: List[dict] = []
    packages = 0
    for archive_name in archives:
        path = os.path.join(data_dir, archive_name)
        if not os.path.exists(path):
            continue
        archive = bsa.open_archive(path)
        try:
            for entry in sorted(archive.list_files(), key=lambda e: e.path.lower()):
                if not entry.path.lower().endswith(".fxp"):
                    continue
                data = archive.read_file(entry.path)
                package = parse_package(entry.path, data)
                packages += 1
                for chunk in package.chunks:
                    row = classify_chunk(chunk)
                    row["package"] = package.path
                    row["key"] = "0x%08x" % chunk.meta
                    row["offset"] = chunk.offset
                    chunks.append(row)
        finally:
            archive.close()

    by_hash: Dict[str, dict] = {}
    for row in chunks:
        prev = by_hash.get(row["sha256"])
        if prev is None:
            by_hash[row["sha256"]] = dict(row, chunks=1)
        else:
            prev["chunks"] += 1

    ps = [r for r in by_hash.values() if r["kind"] == "ps"]
    vs = [r for r in by_hash.values() if r["kind"] == "vs"]
    ps_chunks = sum(r["chunks"] for r in ps)
    vs_chunks = sum(r["chunks"] for r in vs)

    def class_table(rows: Sequence[dict], chunks_total: int) -> dict:
        by_class = Counter(r["class"] for r in rows)
        by_class_chunks = Counter()
        for r in rows:
            by_class_chunks[r["class"]] += r["chunks"]
        return {
            "hashes": dict(by_class.most_common()),
            "chunks": dict(by_class_chunks.most_common()),
            "hash_fraction": {k: round(v / len(rows), 4) for k, v in by_class.items()}
            if rows else {},
            "chunk_fraction": {k: round(v / chunks_total, 4)
                               for k, v in by_class_chunks.items()} if chunks_total else {},
        }

    def reason_table(rows: Sequence[dict]) -> dict:
        reasons = Counter(_refusal_bucket(r["refusal"]) for r in rows if r["refusal"])
        return dict(reasons.most_common(20))

    direct = [r for r in ps if r["class"] == "tev_direct"]
    prebake = [r for r in ps if r["class"] == "tev_prebake"]
    menu_light = [r for r in ps if r["class"] == "tev_menu_light"]
    pica = [r for r in vs if r["class"] == "pica_vs"]

    versions = Counter(r["version"] for r in by_hash.values())
    opcode_hist: Counter = Counter()
    opcode_hash_hist: Counter = Counter()
    for r in by_hash.values():
        opcode_hist.update(r["opcodes"])
        opcode_hash_hist.update(set(r["opcodes"]))
    potential_chunks: Counter = Counter()
    for r in ps:
        if r.get("potential"):
            potential_chunks[r["potential"]] += r["chunks"]
    return {
        "generated_for": data_dir,
        "archives": archives,
        "packages": packages,
        "chunks": len(chunks),
        "chunk_kinds": {"ps": ps_chunks, "vs": vs_chunks},
        "distinct_hashes": len(by_hash),
        "distinct_versions": dict(versions),
        "ps": {
            "chunks": ps_chunks,
            "distinct_hashes": len(ps),
            "classes": class_table(ps, ps_chunks),
            "top_refusals": reason_table(ps),
            "stage_histogram": dict(sorted(Counter(
                r["plan"]["stages"] for r in direct).items())),
            "texture_histogram": dict(sorted(Counter(
                r["plan"]["textures"] for r in direct).items())),
            "constant_histogram": dict(sorted(Counter(
                len(r["plan"]["constants"]) for r in direct).items())),
        },
        "vs": {
            "chunks": vs_chunks,
            "distinct_hashes": len(vs),
            "classes": class_table(vs, vs_chunks),
            "top_refusals": reason_table(vs),
            "input_usage": dict(Counter(
                i["usage"] for r in pica for i in r["inputs"]).most_common()),
            "constant_histogram": dict(sorted(Counter(
                len(r["plan"]["constants"]) for r in pica).items())),
            "expandable": sum(1 for r in pica if r["plan"]["expandable"]),
        },
        "coverage": {
            "ps_tev_direct_hashes": len(direct),
            "ps_tev_direct_chunks": sum(r["chunks"] for r in direct),
            "ps_tev_prebake_hashes": len(prebake),
            "ps_tev_prebake_chunks": sum(r["chunks"] for r in prebake),
            "ps_tev_menu_light_hashes": len(menu_light),
            "ps_tev_menu_light_chunks": sum(r["chunks"] for r in menu_light),
            "ps_software_hashes": len(ps) - len(direct) - len(prebake) - len(menu_light),
            "ps_software_chunks": ps_chunks - sum(r["chunks"] for r in direct)
            - sum(r["chunks"] for r in prebake) - sum(r["chunks"] for r in menu_light),
            "vs_pica_hashes": len(pica),
            "vs_pica_chunks": sum(r["chunks"] for r in pica),
            "vs_software_hashes": len(vs) - len(pica),
            "vs_software_chunks": vs_chunks - sum(r["chunks"] for r in pica),
        },
        "opcode_histogram": dict(opcode_hist.most_common()),
        "opcode_hash_histogram": dict(opcode_hash_hist.most_common()),
        "ps_def_range_potential": {
            "hashes": dict(Counter(r["potential"] for r in ps
                                   if r.get("potential")).most_common()),
            "chunks": dict(potential_chunks.most_common()),
            "note": ("Upper bound only: these PS are refused today because a "
                     "def literal is outside the RGBA8 [0,1] TEV range.  The "
                     "potential class re-runs the planner with clamped "
                     "literals; no such plan is emitted or drawn."),
        },
        "notable": _notable(ps, vs),
        "_rows": list(by_hash.values()),
    }


def _notable(ps: Sequence[dict], vs: Sequence[dict]) -> dict:
    wanted = {
        "8725393bf3ed950789e50501893ab141efdfd0377309271f6ef7493239be5ff9":
            "menu.logo.ps (p1200005)",
        "6022044ef19e4b1c241482207acca5a6be484081e629b6d72cdefdd9568a45de":
            "menu.composite.ps (copy)",
    }
    out = {}
    for row in ps + vs:
        if row["sha256"] in wanted:
            out[wanted[row["sha256"]]] = {
                "sha256": row["sha256"], "sha1": row["sha1"],
                "package": row["package"], "key": row["key"],
                "bytes": row["bytes"], "instructions": row["instructions"],
                "class": row["class"], "refusal": row["refusal"],
                "plan": row.get("plan"),
            }
    return out


def write_json(path: str, value, compact: bool = False) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        if compact:
            json.dump(value, handle, separators=(",", ":"), sort_keys=True)
        else:
            json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default="")
    parser.add_argument("--out", default=r"E:\Skyrim_N3DS_build\assets\shaders\fxp_census.json")
    parser.add_argument("--summary-out", default=os.path.join(REPO, "docs", "SHADER_CENSUS.json"))
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    result = census(find_data(args.data), args.update)
    rows = result.pop("_rows")
    write_json(args.out, {"summary": result, "hashes": rows}, compact=True)
    write_json(args.summary_out, result)
    if not args.quiet:
        ps = result["ps"]
        vs = result["vs"]
        cov = result["coverage"]
        print("packages %d  chunks %d  distinct hashes %d" % (
            result["packages"], result["chunks"], result["distinct_hashes"]))
        print("PS: %d chunks / %d hashes" % (ps["chunks"], ps["distinct_hashes"]))
        for name, count in ps["classes"]["hashes"].items():
            print("  %-32s %5d hashes  %6d chunks" % (
                name, count, ps["classes"]["chunks"].get(name, 0)))
        print("VS: %d chunks / %d hashes" % (vs["chunks"], vs["distinct_hashes"]))
        for name, count in vs["classes"]["hashes"].items():
            print("  %-32s %5d hashes  %6d chunks" % (
                name, count, vs["classes"]["chunks"].get(name, 0)))
        print("coverage: PS tev_direct %d hashes / %d chunks; prebake %d / %d; "
              "menu_light %d / %d; software %d / %d" % (
                  cov["ps_tev_direct_hashes"], cov["ps_tev_direct_chunks"],
                  cov["ps_tev_prebake_hashes"], cov["ps_tev_prebake_chunks"],
                  cov["ps_tev_menu_light_hashes"], cov["ps_tev_menu_light_chunks"],
                  cov["ps_software_hashes"], cov["ps_software_chunks"]))
        print("          VS pica %d hashes / %d chunks; software %d / %d" % (
            cov["vs_pica_hashes"], cov["vs_pica_chunks"],
            cov["vs_software_hashes"], cov["vs_software_chunks"]))
        print("wrote %s" % args.out)
        print("wrote %s" % args.summary_out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
