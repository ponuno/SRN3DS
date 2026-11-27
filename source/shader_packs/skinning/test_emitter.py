#!/usr/bin/env python3
"""Tests for the skinned PICA200 VS emitter and its numeric verification.

    python -m unittest source/shader_packs/skinning/test_emitter.py -v

Offline checks do not need the retail archive.  The numeric/picasso checks
regenerate the three representative plans from F:\\Skyrim\\Data (or the E:
fallback); they are skipped when neither archive is present.
"""

from __future__ import annotations

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for path in (HERE, REPO, os.path.join(REPO, "tools"),
             os.path.join(REPO, "tools", "shaders")):
    if path not in sys.path:
        sys.path.insert(0, path)

import pica_vs_emit as E  # noqa: E402


class EmitterCatalogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plans = E.load_plans()
        with open(E.REPORT, encoding="utf-8") as handle:
            cls.report = json.load(handle)

    def test_report_has_three_passing_representatives(self):
        self.assertEqual(self.report["status"], "pass")
        self.assertEqual(len(self.report["entries"]), 3)
        self.assertEqual(self.report["samples"], 256)
        self.assertEqual(self.report["tolerance"], 1e-4)
        self.assertEqual(sorted(e["id"] for e in self.report["entries"]),
                         sorted(E.REPRESENTATIVES))

    def test_each_representative_is_inside_the_pica_envelope(self):
        for entry in self.report["entries"]:
            self.assertEqual(entry["status"], "pass", entry["id"])
            self.assertLessEqual(entry["outputs"], E.MAX_OUTPUTS)
            self.assertLessEqual(entry["inputs"], E.MAX_INPUTS)
            self.assertLessEqual(entry["uniform_rows"], E.MAX_UNIFORMS)
            self.assertLessEqual(entry["instructions"], 512)
            self.assertEqual(entry["picasso"], "pass")
            self.assertGreater(entry["shbin_bytes"], 0)
            self.assertLessEqual(entry["max_abs_position"], E.TOLERANCE)
            self.assertLessEqual(entry["max_abs_varying"], E.TOLERANCE)

    def test_plans_mark_the_same_three_as_emitted_verified(self):
        entry_sha = {e["id"]: e["shbin_sha256"] for e in self.report["entries"]}
        verified = {e["id"] for e in self.report["entries"]}
        marked = {p["id"] for p in self.plans.values()
                  if p.get("codegen") == "emitted-verified"}
        self.assertEqual(verified, marked)
        self.assertEqual(len(marked), 3)
        for plan_id in verified:
            plan = self.plans[plan_id]
            self.assertEqual(plan["draw_enabled"], 1)
            self.assertEqual(plan["emitter"]["shbin"],
                             "generated/%s.shbin" % plan_id)
            self.assertEqual(plan["emitter"]["shbin_sha256"],
                             entry_sha[plan_id])
            self.assertTrue(plan["emitted_asm"].endswith(".v.pica"))
            self.assertIn("emitted_code_upload_path", plan["preconditions"])

    def test_all_other_contracts_stay_pending(self):
        pending = [p for p in self.plans.values()
                   if p["kind"] == "pica_vs"
                   and p["codegen"] != "emitted-verified"]
        self.assertEqual(len(pending), 80)
        for plan in pending:
            self.assertEqual(plan["codegen"], "pending")
            self.assertEqual(plan["draw_enabled"], 0)

    def test_committed_asm_matches_the_emitter(self):
        for plan_id in E.REPRESENTATIVES:
            plan = self.plans[plan_id]
            path = os.path.join(HERE, plan["emitted_asm"])
            with open(path, encoding="ascii") as handle:
                committed = handle.read()
            self.assertIn("plan %s" % plan_id, committed)
            self.assertNotIn("\n    frc ", committed)
            self.assertNotIn("\n    texld ", committed)
            self.assertNotIn("\n    loop ", committed)
            self.assertIn(".out outpos position", committed)

    def test_named_refusals(self):
        plan = self.plans[E.REPRESENTATIVES[0]]
        bad = dict(plan)
        bad["outputs"] = list(plan["outputs"]) + [{
            "reg_index": 9, "type": 6, "usage": 5, "usage_index": 0,
            "usage_name": "texcoord"}] * 8
        with self.assertRaisesRegex(E.Refusal, "too_many_outputs"):
            E.emit(bad, None)  # refused before the IR is touched
        bad = dict(plan)
        bad["inputs"] = [{"reg_index": 0, "usage": 9, "usage_index": 0,
                          "usage_name": "unsupported"}]
        with self.assertRaisesRegex(E.Refusal, "input_unsupported"):
            E.emit(bad, None)


class EmitterNumericTest(unittest.TestCase):
    def test_three_plans_regenerate_and_match(self):
        try:
            data = E.find_data()
        except E.Refusal:
            self.skipTest("retail archive absent")
        plans = E.load_plans()
        for plan_id in E.REPRESENTATIVES:
            plan = plans[plan_id]
            ir = E.find_ir(plan["sha256"], data)
            entry = E.verify(plan, ir, samples=256)[0]
            self.assertEqual(entry["status"], "pass", entry)
            self.assertLessEqual(entry["max_abs_position"], E.TOLERANCE)
            self.assertLessEqual(entry["max_abs_varying"], E.TOLERANCE)
            self.assertEqual(entry["picasso"], "pass")


if __name__ == "__main__":
    unittest.main(verbosity=2)
