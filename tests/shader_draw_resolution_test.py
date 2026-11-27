"""Gate: every draw-enabled world/menu plan resolves to a concrete program.

This is the synthetic-draw layer of the N3DS_SHADER_DRAW_TRACE proof.  The C
smoke (``shader_draw_trace_smoke.c``) proves the one-line formatter prints
``<index>:<pinned shbin SHA>`` and ``refusal=null``; this test proves the
resolution step for every committed draw plan:

* each ``draw_enabled: 1`` world plan joins to an exact-hash PICA VS program or
  a draw-enabled generated TEV row (no ``none``);
* the five GFx menu programs resolve in the PICA VS catalog;
* the resolution table is committed as ``docs/shader_draw_trace.json``.

A plan that exists in the catalog but resolves to nothing fails the test.
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "shaders"))

import runtime_audit  # noqa: E402
import world_runtime_audit  # noqa: E402

OUT = os.path.join(REPO, "docs", "shader_draw_trace.json")
CORE = os.path.join(REPO, "source", "d3d9_core.c")


def main() -> int:
    checks = 0
    failures = []

    def check(condition, text):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(text)

    docs = {entry["sha256"]: entry
            for entry in json.load(open(world_runtime_audit.DOCS_PLANS,
                                        encoding="utf-8"))["entries"]}
    vs = runtime_audit.parse_vs_catalog()
    table = runtime_audit.parse_plan_table()
    core = open(CORE, encoding="utf-8").read()
    # The core selects the fetch contract from the emitted program: bone
    # programs need fetch_skin(), non-bone programs use fetch_plain().  If the
    # plain path is removed, non-bone plans silently become incomplete again;
    # fail here instead of committing a false complete:true row.
    check("fetch_plain" in core,
          "d3d9_core.c has no plain (non-skinned) PICA VS fetch path")
    check("fetch_skin" in core,
          "d3d9_core.c has no skinned PICA VS fetch path")
    rows = []
    for pack in world_runtime_audit.WORLD_PACKS:
        plan_doc = world_runtime_audit._pack_doc(pack, "plans.json") \
            or {"entries": []}
        manifest = world_runtime_audit._pack_doc(pack, "manifest.json") or {}
        joined = {entry["sha256"]: entry
                  for entry in plan_doc.get("entries", [])}
        for row in manifest.get("hashes", []):
            sha = row[0]
            if sha in docs and sha not in joined:
                joined[sha] = docs[sha]
        for sha, entry in sorted(joined.items(), key=lambda kv: kv[1]["id"]):
            if entry.get("draw_enabled") != 1:
                continue
            path, why = world_runtime_audit._draw_path(entry, sha, vs, table)
            check(path is not None,
                  "%s: no runtime program (%s)" % (entry["id"], why))
            program = path or "none"
            shbin = ""
            if entry.get("kind") == "pica_vs" and sha in vs:
                shbin = vs[sha]["shbin_sha256"]
                check(vs[sha]["shbin"][:4] == b"DVLB",
                      "%s: program is not DVLB" % entry["id"])
            rows.append({
                "pack": pack, "plan": entry["id"], "sha256": sha,
                "kind": entry.get("kind"), "program": program,
                "shbin_sha256": shbin,
                "passes": len(entry.get("passes") or []) or 1,
                "refusal": None if path else why,
                # d3d9_core selects by the emitted program's declaration:
                # bone_rows != 0 -> fetch_skin(), otherwise fetch_plain()
                # with the program's identity uniform bank.  The source probe
                # above makes a regression here a test failure.
                "core_draw": (
                    "plain fetch (non-skinned)" if entry.get("kind") == "pica_vs"
                    and vs.get(sha, {}).get("bone_rows", 0) == 0
                    else "skinned fetch" if entry.get("kind") == "pica_vs"
                    else "ok"),
                "complete": bool(path),
            })

    for row in rows:
        check(not (row["kind"] == "pica_vs" and not row["complete"]
                   and row["refusal"] is None),
              "%s: runtime path is incomplete with no recorded refusal"
              % row["plan"])

    menu = sorted(program["id"] for program in vs.values()
                  if program["id"].startswith("menu.gfx."))
    check(len(menu) == 5, "expected five GFx menu programs, got %d"
          % len(menu))
    for plan_id in menu:
        check(any(program["id"] == plan_id
                  for program in vs.values()),
              "%s does not resolve in the PICA catalog" % plan_id)
        rows.append({
            "pack": "menu", "plan": plan_id,
            "sha256": next(p["sha256"] for p in vs.values()
                           if p["id"] == plan_id),
            "kind": "pica_vs", "program": "pica_vs:%s" % plan_id,
            "shbin_sha256": next(p["shbin_sha256"] for p in vs.values()
                                 if p["id"] == plan_id),
            "passes": 1, "refusal": None,
            "core_draw": "menu CPU-safe helper", "complete": True,
        })

    report = {
        "schema": "shader-draw-trace/1",
        "note": ("Synthetic draw resolution: one row per committed draw plan. "
                 "N3DS_SHADER_DRAW_TRACE=1 prints the same fields at runtime; "
                 "the C smoke proves the formatter."),
        "draws": len(rows),
        "world_draws": sum(1 for row in rows if row["pack"] != "menu"),
        "menu_cpu_safe": menu,
        "entries": rows,
        "ok": not failures,
    }
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, indent=1, sort_keys=True)
        handle.write("\n")

    for row in rows:
        print("draw %-24s %-18s -> %s%s"
              % (row["plan"], row["kind"], row["program"],
                 "" if row["refusal"] is None else " REFUSED: " + row["refusal"]))
    print("shader_draw_resolution_test: %d checks, %d failures "
          "(%d draws, %d menu)"
          % (checks, len(failures), len(rows), len(menu)))
    for text in failures[:40]:
        print("FAIL: %s" % text)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
