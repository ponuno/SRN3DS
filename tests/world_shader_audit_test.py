"""Gate: every draw-enabled *world* plan has a working runtime path.

``tools/shaders/world_runtime_audit.py`` scopes ``runtime_audit`` to the five
world packs (``terrain``, ``lighting``, ``skinning``, ``sky_water``,
``blit_post``) and adds the checks those packs need:

* the pack manifest must round-trip to committed plans (kind + draw status);
* a ``draw_enabled: 1`` row must have ``runtime: 1`` when that field is claimed
  and an exact-hash PICA VS program or a draw-enabled generated TEV row;
* TEV passes/stages/constant binding and PICA VS lengths/registers/varyings
  (<= 7)/uniform bank (<= c31) must be valid;
* a draw-disabled ``emitted-verified`` PICA VS must state why it is not in the
  runtime VS catalog (no silent green counters).

The test also exercises the negative case: removing the TEV row that backs a
draw-enabled world plan must make the audit report it as unmatched.
"""

from __future__ import annotations

import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "shaders"))

import runtime_audit  # noqa: E402
import world_runtime_audit  # noqa: E402

EXPECTED_DRAW = {
    "terrain": 0,
    "lighting": 6,
    "skinning": 3,
    "sky_water": 0,
    "blit_post": 1,
}


def main() -> int:
    report = world_runtime_audit.audit()
    checks = 0
    failures = []

    def check(condition, text):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(text)

    for row in report["unmatched"]:
        failures.append("world draw-enabled plan without runtime path: "
                        "%s/%s (%s): %s"
                        % (row["pack"], row["id"], row["sha256"][:16],
                           row["reason"]))
    for text in report["invalid"]:
        failures.append("invalid world runtime artifact: %s" % text)
    for text in report["silent_off"]:
        failures.append("draw-disabled plan without a reason: %s" % text)
    for text in report["manifest_mismatches"]:
        failures.append("manifest/plan mismatch: %s" % text)
    for text in report["coverage_mismatches"]:
        failures.append("coverage/plan mismatch: %s" % text)

    check(report["totals"]["draw_enabled"] ==
          report["totals"]["runtime_paths"],
          "world draw-enabled %d != runtime paths %d"
          % (report["totals"]["draw_enabled"],
             report["totals"]["runtime_paths"]))
    seen = {pack["pack"]: pack for pack in report["packs"]}
    check(set(seen) == set(EXPECTED_DRAW),
          "world pack set %s != %s"
          % (sorted(seen), sorted(EXPECTED_DRAW)))
    for pack, expected in EXPECTED_DRAW.items():
        row = seen.get(pack)
        check(row is not None and row["draw_enabled"] == expected,
              "%s draw-enabled %r, expected %d"
              % (pack, row and row["draw_enabled"], expected))
        check(row is not None and row["runtime_paths"] == expected,
              "%s runtime paths %r, expected %d"
              % (pack, row and row["runtime_paths"], expected))
        check(row is None or row["catalog_only"] == 0,
              "%s has %r catalog-only draw plans"
              % (pack, row and row["catalog_only"]))

    top = report["top_world_shaders"]
    check(len(top) == 10, "expected top-10 world shaders, got %d" % len(top))
    check(all(row["status"] for row in top),
          "a top-10 world shader has no status")
    check(all(top[i]["chunks"] >= top[i + 1]["chunks"]
              for i in range(len(top) - 1)),
          "top-10 world shaders are not ordered by chunk count")

    # Negative: the gate must actually fail when a draw plan loses its row.
    saved = runtime_audit.parse_plan_table
    try:
        def broken_table():
            table = saved()
            table.pop(
                "6022044ef19e4b1c241482207acca5a6be484081e629b6d72cdefdd9568a45de",
                None)
            return table
        runtime_audit.parse_plan_table = broken_table
        broken = world_runtime_audit.audit()
    finally:
        runtime_audit.parse_plan_table = saved
    check(not broken["ok"] and
          any("menu.composite.ps" in row["id"]
              for row in broken["unmatched"]),
          "negative check: removing menu.composite.ps TEV row was not caught")

    print("world_shader_audit_test: %d checks, %d failures "
          "(%d world draw-enabled, %d runtime paths, %d packs)"
          % (checks, len(failures),
             report["totals"]["draw_enabled"],
             report["totals"]["runtime_paths"], len(report["packs"])))
    for text in failures[:40]:
        print("FAIL: %s" % text)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
