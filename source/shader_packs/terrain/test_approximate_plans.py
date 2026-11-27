#!/usr/bin/env python3
"""Regression tests for the declared port-lite terrain approximations."""
from __future__ import annotations

import collections
import json
import os
import sys
import unittest
from pathlib import Path

PACK_DIR = Path(__file__).resolve().parent
REPO = PACK_DIR.parents[2]
if str(PACK_DIR) not in sys.path:
    sys.path.insert(0, str(PACK_DIR))
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

import approximate  # noqa: E402


def load(path: Path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


class ApproximatePlanTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = load(PACK_DIR / "plans.json")
        cls.plans = cls.doc["entries"]
        cls.table = load(PACK_DIR / "terrain_classes.json")
        cls.rows = {row["sha256"]: row for row in cls.table["hashes"]}

    def test_counts_and_policies(self):
        self.assertEqual(len(self.plans), 138)
        self.assertEqual(sum(self.rows[plan["sha256"]]["chunks"]
                             for plan in self.plans), 190)
        self.assertEqual(collections.Counter(plan["approximation"]["policy"]
                                             for plan in self.plans), {
            "unlit_diffuse_baseline": 11,
            "unlit_diffuse_overlay_pass": 7,
            "dominant_three_layers": 60,
            "lod_ladder_default_near3": 60,
        })
        self.assertEqual(collections.Counter((plan["stages"], len(plan["textures"]))
                                             for plan in self.plans), {
            (1, 1): 18, (3, 3): 120,
        })

    def test_honesty_fields_and_runtime_gate(self):
        for plan in self.plans:
            self.assertEqual(plan["quality"], "approx")
            self.assertIn("dropped_policy", plan["approximation"])
            self.assertIn("expected_visual", plan["approximation"])
            self.assertIn("layers_kept", plan["approximation"])
            self.assertIn("weight_error_vs_original", plan["approximation"])
            self.assertEqual(plan["approximation"]["weight_error_vs_original"]["status"],
                             "not_measured")
            self.assertIn("def_policy", plan["approximation"])
            self.assertEqual((plan["draw_enabled"], plan["runtime"]), (0, 0))
            self.assertEqual(plan["pass"], 1)
            self.assertLessEqual(plan["stages"], 6)
            self.assertLessEqual(len(plan["textures"]), 3)
            self.assertEqual(plan["material_contract"]["status"], "measured_static")
            self.assertTrue(plan["draw_blockers"])
        self.assertEqual(sum(plan["runtime_candidate"] for plan in self.plans), 10)

    def test_layer_policies_and_lod_ladder(self):
        for plan in self.plans:
            approximation = plan["approximation"]
            row = self.rows[plan["sha256"]]
            if plan["simplified_spec"]["kind"] == "single":
                self.assertEqual(approximation["layers_kept"], 1)
                self.assertEqual(approximation["kept_samplers"], [0])
                self.assertNotIn("lod_tiers", approximation)
            else:
                self.assertEqual(approximation["layers_kept"], 3)
                self.assertEqual(approximation["kept_samplers"], [0, 1, 2])
                self.assertEqual(approximation["dropped_layers"],
                                 row["texture_layers"] - 3)
                self.assertAlmostEqual(sum(approximation["weights"]), 1.0)
                if row["class"] == "terrain.multitexture_14_15_lod":
                    self.assertEqual(approximation["policy"],
                                     "lod_ladder_default_near3")
                    self.assertEqual(approximation["lod_tiers"],
                                     {"near": 3, "mid": 2, "far": 1})
                    self.assertEqual(approximation["lod_tier_default"], "near")
                else:
                    self.assertEqual(approximation["policy"],
                                     "dominant_three_layers")
                    self.assertNotIn("lod_tiers", approximation)

    def test_def_policy_is_explicit(self):
        for plan in self.plans:
            policy = plan["approximation"]["def_policy"]
            row = self.rows[plan["sha256"]]
            expected_hard = sorted({float(value)
                                    for definition in row["defs"]
                                    for value in definition["values"]
                                    if value < 0 or value > 1})
            expected_uniform = sorted({float(value)
                                       for definition in row["defs"]
                                       for value in definition["values"]
                                       if value not in (-1, 0, 1)})
            self.assertEqual(policy["hard_values_present"], expected_hard)
            self.assertEqual(policy["uniform_or_approximated"], expected_uniform)
            self.assertEqual(policy["literal_folded"], ["0", "1", "-1"])
            self.assertTrue(all(value not in (-1.0, 0.0, 1.0)
                                for value in policy["uniform_or_approximated"]))
            if 2.0 in expected_hard:
                self.assertIn(2.0, policy["uniform_or_approximated"])


    def test_symbolic_simplified_spec(self):
        for plan in self.plans:
            kind = plan["simplified_spec"]["kind"]
            verified = approximate._verify(plan, kind)
            self.assertEqual(verified["status"], "pass")
            self.assertEqual(plan["symbolic_verify"], verified)
            for contract_texture, plan_texture in zip(
                    plan["material_contract"]["textures"], plan["textures"]):
                self.assertEqual(contract_texture["unit"],
                                 plan["textures"].index(plan_texture))
                self.assertEqual(contract_texture["sampler_reg"],
                                 plan_texture["sampler_reg"])
                self.assertEqual(contract_texture["texcoord_reg"],
                                 plan_texture["texcoord_reg"])

    def test_stage_shapes(self):
        for plan in self.plans:
            if plan["simplified_spec"]["kind"] == "single":
                self.assertEqual(plan["stages"], 1)
                self.assertEqual(plan["stage_data"][0]["func_rgb"], 0)
                self.assertEqual(plan["stage_data"][0]["src_rgb"][0], 3)
            else:
                self.assertEqual(plan["stages"], 3)
                self.assertEqual(plan["stage_data"][1]["func_rgb"], 4)
                self.assertEqual(plan["stage_data"][1]["const_rgba8"], 0x7F7F7F7F)
                self.assertEqual(plan["stage_data"][2]["const_rgba8"], 0xAAAAAAAA)
                self.assertAlmostEqual(sum(plan["approximation"]["weights"]), 1.0)
                colours = (0.1, 0.6, 0.9)
                value = colours[0]
                value = value * (127 / 255) + colours[1] * (128 / 255)
                value = value * (170 / 255) + colours[2] * (85 / 255)
                expected = sum(weight * colour for weight, colour in zip(
                    plan["approximation"]["weights"], colours))
                self.assertAlmostEqual(value, expected, places=12)

    def test_manifest_and_refusals(self):
        manifest = load(PACK_DIR / "manifest.json")
        self.assertEqual(manifest["coverage"], {
            "translated": 138, "draw-enabled": 0, "refused": 109,
            "contract_only": 0,
        })
        self.assertEqual({entry["sha256"] for entry in manifest["plans"]},
                         {plan["sha256"] for plan in self.plans})
        refusals = load(PACK_DIR / "refusals.json")["hashes"]
        refused = {row["sha256"] for row in refusals}
        self.assertEqual(len(refused), 109)
        self.assertFalse(refused & {plan["sha256"] for plan in self.plans})

    def test_regeneration(self):
        corpus = Path(r"E:\host_sm3")
        data = Path(r"F:\Skyrim\Data")
        if not (data / "Skyrim - Shaders.bsa").exists():
            self.skipTest("retail shader archive absent")
        regenerated, _ = approximate.build_plans(corpus, PACK_DIR / "manifest.json",
                                                  str(data))
        self.assertEqual(regenerated, self.doc)
        self.assertEqual(approximate.build_report(self.plans, PACK_DIR / "terrain_classes.json"),
                         load(PACK_DIR / "approximations.json"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
