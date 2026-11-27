#!/usr/bin/env python3
"""Pack-local checks for the StartMenu/GFx menu plans.

Run with ``python source/shader_packs/ui/test_menu_plans.py`` (no retail bytes
needed) or ``--tesv PATH`` to also re-extract the blobs and regenerate plans.
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
    with open(T.CATALOG_PATH, encoding="utf-8") as handle:
        catalog = json.load(handle)

    entries = catalog.get("entries", [])
    refusals = catalog.get("refusals", [])
    ps = [e for e in entries if e["stage"] == "ps_3_0"]
    vs = [e for e in entries if e["stage"] == "vs_3_0"]
    runtime = [e for e in entries if e.get("runtime") == 1]

    check(catalog.get("schema") == "d9-shader-plan-v1", "catalog schema")
    check(len(ps) == 14, "fourteen PS full blobs translated")
    check(len(vs) == 5, "five VS full blobs translated")
    check(len(entries) + len(refusals) == 43, "43 full blobs partition")
    check(len(refusals) == 24, "24 named PS refusals")
    check(2 <= len(runtime) <= 4, "2-4 runtime entries")
    check(all(e["draw_enabled"] == 1 and e["quality"] == "exact"
              and e["material_contract"]["complete"] for e in runtime),
          "runtime entries are exact with a complete contract")
    check(all(e["draw_enabled"] == 0 for e in entries if e not in runtime),
          "non-runtime entries never claim draw")
    check(all(e.get("quality") == "approx" and e.get("approximation")
              for e in entries if e["stage"] == "ps_3_0"
              and e.get("quality") == "approx"),
          "approximations are named")
    check(sum(1 for e in ps if e.get("quality") == "approx") == 4,
          "four declared approximations")
    check(len({e["sha256"] for e in entries}) == len(entries),
          "full-blob hashes are unique")
    check(all(len(e["sha256"]) == 64 for e in entries),
          "full-blob hashes are 64 hex")
    check(all(r.get("class") and r.get("reason") for r in refusals),
          "every refusal is named")

    cov = catalog.get("coverage", {})
    check(cov.get("translated") == len(entries), "coverage translated")
    check(cov.get("refused") == len(refusals), "coverage refused")
    check(cov.get("draw_enabled") == len(runtime), "coverage draw")
    check(cov.get("full_blobs") == {"ps": 38, "vs": 5}, "full blob counts")
    check(doc["measurement"]["prefix_hash_defect"] is True,
          "prefix-hash defect recorded")

    problems = T.check(plans_path=T.PLANS_PATH, ir_path=T.IR_PATH,
                       catalog_path=T.CATALOG_PATH, tesv=tesv)
    check(not problems, "symbolic verifier: %r" % problems)

    print(json.dumps({
        "checks": 18,
        "failures": FAILS,
        "ps_translated": len(ps),
        "vs_translated": len(vs),
        "runtime": [e["id"] for e in runtime],
        "refused": len(refusals),
        "approximations": [e["id"] for e in ps if e.get("quality") == "approx"],
        "symbolic": not problems,
    }, indent=2))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    raise SystemExit(main())
