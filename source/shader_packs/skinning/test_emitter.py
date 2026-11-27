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
import random
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

    def test_report_has_the_verified_programs(self):
        self.assertEqual(self.report["status"], "pass")
        self.assertEqual(len(self.report["entries"]), 108)
        self.assertEqual(self.report["samples"], 256)
        self.assertEqual(self.report["tolerance"], 1e-4)
        for entry in self.report["entries"]:
            self.assertEqual(entry["status"], "pass", entry["id"])

    def test_each_verified_program_is_inside_the_pica_envelope(self):
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

    def test_plans_mark_the_same_programs_as_emitted_verified(self):
        entry_sha = {e["id"]: e["shbin_sha256"] for e in self.report["entries"]}
        verified = {e["id"] for e in self.report["entries"]}
        marked = {p["id"] for p in self.plans.values()
                  if p.get("codegen") == "emitted-verified"}
        self.assertEqual(verified, marked)
        self.assertEqual(len(marked), 108)
        drawn = {p["id"] for p in self.plans.values()
                 if p.get("draw_enabled") == 1}
        self.assertEqual(drawn, set(E.REPRESENTATIVES))
        for plan_id in verified:
            plan = self.plans[plan_id]
            self.assertIn(plan["quality"], ("exact", "approx"))
            self.assertEqual(plan["emitter"]["shbin"],
                             "generated/%s.shbin" % plan_id)
            self.assertEqual(plan["emitter"]["shbin_sha256"],
                             entry_sha[plan_id])
            self.assertTrue(plan["emitted_asm"].endswith(".v.pica"))
            self.assertLessEqual(len(plan["outputs"]), E.MAX_OUTPUTS)
            packing = plan.get("packing") or {}
            self.assertLessEqual(len(packing.get("components", [])), 24)
            for drop in packing.get("dropped", []):
                self.assertEqual((drop["usage"], drop["usage_index"]), (10, 1))
            if plan["quality"] == "approx":
                self.assertTrue(plan.get("approximation"))
                self.assertTrue(packing.get("dropped"))
            if plan_id in drawn:
                self.assertIn("emitted_code_upload_path", plan["preconditions"])
                self.assertEqual(plan.get("runtime"), 1)
            else:
                self.assertEqual(plan.get("runtime"), 0)

    def test_no_contract_is_left_pending(self):
        pending = [p for p in self.plans.values()
                   if p["kind"] == "pica_vs"
                   and p["codegen"] != "emitted-verified"]
        self.assertEqual(pending, [])

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
    def test_four_weights_use_four_distinct_bone_indices(self):
        """The retail idiom writes all four a0 components and the address token
        selects one per influence.  a0.x-only lowering collapses them onto the
        first bone; this test makes the four matrices distinguishable."""
        plans = E.load_plans()
        plan = plans["skin.vs.f8ffaecd"]
        try:
            ir = E.find_ir(plan["sha256"])
        except E.Refusal:
            self.skipTest("retail archive absent")
        text = E.emit(plan, ir)
        # a0.x = v2.z, a0.y = v2.y, a0.z = v2.x, a0.w = v2.w (zyxw swizzle).
        inputs = {
            0: [1.0, 2.0, 3.0, 1.0],
            1: [0.4, 0.3, 0.2, 0.1],
            2: [2 / 255.0, 1 / 255.0, 0 / 255.0, 3 / 255.0],
        }
        d3d = {i: [0.0] * 4 for i in range(256)}
        pica = {}
        for bone in range(4):
            rows = [[1.0 + bone, 0.1 * bone, 0.0, 2.0 * bone],
                    [0.0, 1.0 + 2.0 * bone, 0.0, 3.0 * bone],
                    [0.0, 0.0, 1.0 + 3.0 * bone, 4.0 * bone]]
            for r in range(3):
                d3d[3 * bone + r] = list(rows[r])
                pica[3 * bone + r] = list(rows[r])
        wvp = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0],
               [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]]
        for i in range(4):
            d3d[180 + i] = list(wvp[i])
            pica[12 + i] = list(wvp[i])
        defs = {184: tuple(ir.defs[184])}
        d3d[184] = list(defs[184])
        ref = E.interpret_ir(ir, inputs, d3d, defs)
        emitted = E.interpret_pica(text, inputs, pica,
                                   {"d184": list(defs[184])})
        expected = E.pica_clip(ref[0])
        got = emitted["outpos"]
        self.assertLess(max(abs(expected[i] - got[i]) for i in range(4)), 1e-5)
        # A model that uses bone 0 for every weight must disagree, otherwise the
        # sample does not distinguish the four indices.
        pos = [1.0, 2.0, 3.0, 1.0]
        wrong = []
        for r in range(3):
            row = d3d[r]
            wrong.append(sum(row[c] * pos[c] for c in range(4)))
        wrong = E.pica_clip(wrong + [1.0])
        self.assertGreater(max(abs(expected[i] - wrong[i]) for i in range(4)),
                           1e-3)
        for comp in ("xxxx", "yyyy", "zzzz", "wwww"):
            self.assertIn("mova a0, r12.%s" % comp, text)

    def test_c184_is_a_real_uniform_row(self):
        """The 17-row program's UV transform lives in c184.  It must be filled
        with a non-zero value, otherwise a shader that drops the transform
        still compares as 0 == 0."""
        plans = E.load_plans()
        plan = plans["skin.vs.60dbd11a"]
        try:
            ir = E.find_ir(plan["sha256"])
        except E.Refusal:
            self.skipTest("retail archive absent")
        abs_rows, rows = E._d3d_const_rows(ir, E.BONES)
        self.assertIn(184, abs_rows)
        self.assertEqual(rows, 17)
        defs = {ins.dst.index: tuple(ins.literal) for ins in ir.body
                if ins.opcode == 0x51 and ins.dst is not None}
        rng = random.Random(7)
        d3d, pica = E.build_constants(rng, E.BONES, defs, abs_rows)
        self.assertTrue(any(abs(v) > 1e-6 for v in pica[abs_rows[184]]),
                        "c184 must carry the UV transform")
        inputs = E.build_sample(rng, plan, E.BONES)
        text = E.emit(plan, ir)
        emitted = E.interpret_pica(text, inputs, pica,
                                   {"d%d" % k: list(v)
                                    for k, v in defs.items()})
        self.assertTrue(any(abs(v) > 1e-6 for v in emitted["outtc0"]),
                        "UV output must not be the zero default")

    def test_empty_or_unassembled_run_is_not_verified(self):
        plans = E.load_plans()
        plan = plans["skin.vs.f8ffaecd"]
        try:
            ir = E.find_ir(plan["sha256"])
        except E.Refusal:
            self.skipTest("retail archive absent")
        empty, _ = E.verify(plan, ir, samples=0)
        self.assertEqual(empty["status"], "fail")
        unassembled, _ = E.verify(plan, ir, samples=4, compile_asm=False)
        self.assertEqual(unassembled["status"], "fail")
        self.assertEqual(unassembled["picasso"], "skipped")

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
