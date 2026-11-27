"""Gate: every draw-enabled plan has a concrete runtime path.

``tools/shaders/runtime_audit.py`` joins ``plans.json``/``docs/shader_plans.json``
with the two runtime artifacts the 3DS build actually ships:

* ``source/d3d9_pica_vs_*_table.inc`` -- exact-hash PICA VS programs,
* ``source/d3d9_shader_plan_table.inc`` -- generated TEV/pica_vs descriptors.

A plan with ``draw_enabled == 1`` but no validated runtime row is a hole: the
runtime would answer ``no compiled plan`` (TEV) or leave the legacy path (VS)
at create time.  This test fails on that, on a structurally invalid PICA VS
program, and on a stale generated C table.
"""

from __future__ import annotations

import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "shaders"))

import runtime_audit  # noqa: E402


def main() -> int:
    report = runtime_audit.audit()
    checks = 0
    failures = []

    def check(condition, text):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(text)

    for row in report["unmatched"]:
        failures.append("draw-enabled plan without runtime path: %s (%s): %s"
                        % (row["id"], row["sha256"][:16], row["reason"]))
    for text in report["invalid"]:
        failures.append("invalid runtime artifact: %s" % text)
    for text in report["conflicts"]:
        failures.append("conflicting plan row: %s" % text)
    for text in report["stale_table"]:
        failures.append("stale generated plan table: %s" % text)

    check(report["draw_enabled"] == report["runtime_matched"],
          "draw-enabled %d != runtime matched %d"
          % (report["draw_enabled"], report["runtime_matched"]))
    check(report["vs_programs"] == 38,
          "PICA VS catalog has %d programs, expected 38"
          % report["vs_programs"])
    check(report["vs_valid"] == report["vs_programs"],
          "only %d of %d PICA VS programs are valid"
          % (report["vs_valid"], report["vs_programs"]))
    check(report["draw_enabled_census"] > 0,
          "no draw-enabled census hash found")

    coverage = json.load(open(os.path.join(REPO, "docs", "shader_coverage.json"),
                              encoding="utf-8"))
    total = coverage["total"]
    check(total["draw-enabled"] == report["draw_enabled_census"],
          "coverage.json says %d census draws, audit saw %d"
          % (total["draw-enabled"], report["draw_enabled_census"]))
    check(total["translated"] + total["draw-enabled"] +
          total["refused"] == coverage["census_hashes"],
          "coverage buckets do not partition the census")

    print("pica_runtime_audit_test: %d checks, %d failures "
          "(%d draw-enabled, %d runtime paths, %d VS programs)"
          % (checks, len(failures), report["draw_enabled"],
             report["runtime_matched"], report["vs_programs"]))
    for text in failures[:40]:
        print("FAIL: %s" % text)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
