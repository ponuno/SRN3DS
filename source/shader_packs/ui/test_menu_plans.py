#!/usr/bin/env python3
"""Pack-local checks for the StartMenu/GFx menu plans.

Run with ``python source/shader_packs/ui/test_menu_plans.py`` (no retail bytes
needed) or ``--tesv PATH`` to also re-extract the blobs and regenerate plans.
These assertions are intentionally independent of the global plan table: the
ten measured hashes are not in ``Skyrim - Shaders.bsa`` and therefore have no
row in ``docs/shader_plans.json`` yet.
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import menu_plan_tool as T  # noqa: E402

FAILS = []


def check(condition, text):
    if not condition:
        FAILS.append(text)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    tesv = ""
    if "--tesv" in argv:
        tesv = argv[argv.index("--tesv") + 1]

    with open(T.PLANS_PATH, encoding="utf-8") as handle:
        doc = json.load(handle)
    with open(T.IR_PATH, encoding="utf-8") as handle:
        ir = json.load(handle)

    entries = doc.get("entries", [])
    refusals = doc.get("refusals", [])
    check(doc.get("schema") == "ui-menu-plans/1", "schema id")
    check(len(entries) + len(refusals) == 10, "10 measured hashes partition")
    check(len(entries) == 5, "five translated representatives")
    check(len(refusals) == 5, "five named refusals")
    check(all(e["draw_enabled"] == 0 for e in entries),
          "no menu plan is draw-enabled without a measured material contract")
    check(sum(1 for e in entries if e["kind"] == "pica_vs") == 3,
          "three VS descriptors")
    check(sum(1 for e in entries if e["kind"] == "tev") == 2,
          "two TEV descriptors")
    check(all(len(e["sha256"]) == 64 for e in entries),
          "full-blob hashes are 64 hex")
    check(len({e["sha256"] for e in entries}) == len(entries),
          "full-blob hashes are unique")
    check(all(e.get("material_contract", {}).get("complete") is False
              for e in entries if e["kind"] == "tev"),
          "TEV material contracts stay open")
    check(all(r.get("class") and r.get("reason") for r in refusals),
          "every refusal is named")
    measurement = doc.get("measurement", {})
    check(measurement.get("prefix_hash_defect") is True,
          "prefix-hash defect is recorded")
    check(measurement.get("distinct_full_blobs") == {"ps": 38, "vs": 5},
          "full-blob identity counts")
    check(len(ir.get("rows", [])) == 10, "ten IR rows")

    problems = T.check(plans_path=T.PLANS_PATH, ir_path=T.IR_PATH, tesv=tesv)
    check(not problems, "symbolic verifier: %r" % problems)

    print(json.dumps({
        "checks": 16,
        "failures": FAILS,
        "translated": len(entries),
        "refused": len(refusals),
        "draw_enabled": 0,
        "symbolic": not problems,
    }, indent=2))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    raise SystemExit(main())
