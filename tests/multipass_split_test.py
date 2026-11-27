"""Gate for the multi-pass texture-splitting feasibility record.

``tools/multipass_split.py`` measures, for the refused multi-texture lighting
PS, how many samplers they read and what a two-texture scheduler plan costs.
The current ``d9_draw_plan_passes`` replays the same draw with the same two
device textures, so a pass cannot rebind s2..s6; a split would not reproduce
the shader.  This test keeps that verdict honest and prevents a plan from being
promoted on the strength of a ~0.9-error fallback.
"""

from __future__ import annotations

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
REPORT = os.path.join(REPO, "source", "shader_packs",
                      "multipass_split_report.json")


def main() -> int:
    checks = 0
    failures = []

    def check(condition, text):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(text)

    doc = json.load(open(REPORT, encoding="utf-8"))
    entries = doc.get("entries", [])
    by_id = {row["id"]: row for row in entries}
    check(doc.get("device_textures") == 2, "device texture budget")
    check(len(entries) == 8, "expected 8 probed hashes, got %d"
          % len(entries))
    pilot = by_id.get("multipass.53fe44bb")
    check(pilot is not None, "pilot 53fe44bb missing")
    if pilot:
        check(pilot["samplers"] == ["s0", "s1", "s2", "s3", "s4", "s5",
                                    "s6"], "pilot sampler set")
        check(pilot["passes_needed"] == 4, "pilot split needs 4 passes")
        check(pilot["two_texture_plan"]["samples"] == 256,
              "pilot numeric samples")
        check(pilot["two_texture_plan"]["max_abs_error"] >
              doc["meaningful_error"], "pilot error is above the review bar")
        check(pilot["executable"] is False, "pilot cannot be executed")
        check(any("d->texture" in reason for reason in pilot["reasons"]),
              "pilot reason names the shared-texture contract")
    for row in entries:
        check(row["executable"] is False,
              "%s claims an executable split" % row["id"])
        check(row["sampler_count"] > doc["device_textures"],
              "%s does not exceed the device texture budget" % row["id"])
        check(bool(row["reasons"]), "%s has no reason" % row["id"])

    # No probed hash may be draw-enabled while the split is not executable.
    plans = json.load(open(os.path.join(REPO, "source", "shader_packs",
                                        "lighting", "plans.json"),
                           encoding="utf-8"))
    drawn = {e["sha256"] for e in plans["entries"]
             if e.get("draw_enabled") == 1}
    for row in entries:
        check(row["sha256"] not in drawn,
              "%s is draw-enabled without an executable split" % row["id"])

    print("multipass_split_test: %d checks, %d failures (%d hashes, pilot "
          "4 passes, max err %.3f)"
          % (checks, len(failures), len(entries),
             (pilot or {}).get("two_texture_plan", {}).get("max_abs_error", -1)))
    for text in failures[:40]:
        print("FAIL: %s" % text)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
