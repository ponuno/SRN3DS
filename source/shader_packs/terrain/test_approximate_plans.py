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
        self.assertEqual(len(self.plans), 19)
        self.assertEqual(sum(self.rows[plan["sha256"]]["chunks"]
                             for plan in self.plans), 49)
        self.assertEqual(collections.Counter(plan["approximation"]["policy"]
                                             for plan in self.plans), {
            "unlit_diffuse_baseline": 6,
            "unlit_diffuse_overlay_pass": 7,
            "near_three_equal": 6,
        })
        self.assertEqual(collections.Counter((plan["stages"], len(plan["textures"]))
                                             for plan in self.plans), {
            (1, 1): 13, (3, 3): 6,
        })

    def test_honesty_fields_and_runtime_gate(self):
        for plan in self.plans:
            self.assertEqual(plan["quality"], "approx")
            self.assertIn("dropped_policy", plan["approximation"])
            self.assertIn("expected_visual", plan["approximation"])
            self.assertEqual((plan["draw_enabled"], plan["runtime"]), (0, 0))
            self.assertEqual(plan["pass"], 1)
            self.assertLessEqual(plan["stages"], 6)
            self.assertLessEqual(len(plan["textures"]), 3)
            self.assertEqual(plan["material_contract"]["status"], "measured_static")
            self.assertTrue(plan["draw_blockers"])
        self.assertEqual(sum(plan["runtime_candidate"] for plan in self.plans), 5)

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
            "translated": 19, "draw-enabled": 0, "refused": 228,
            "contract_only": 0,
        })
        self.assertEqual({entry["sha256"] for entry in manifest["plans"]},
                         {plan["sha256"] for plan in self.plans})
        refusals = load(PACK_DIR / "refusals.json")["hashes"]
        refused = {row["sha256"] for row in refusals}
        self.assertEqual(len(refused), 228)
        self.assertFalse(refused & {plan["sha256"] for plan in self.plans})

    def test_regeneration(self):
        corpus = Path(r"E:\host_sm3")
        if not (corpus / "corpus.json").exists():
            self.skipTest("host canonical IR absent")
        regenerated, _ = approximate.build_plans(corpus, PACK_DIR / "manifest.json",
                                                  r"F:\Skyrim\Data")
        self.assertEqual(regenerated, self.doc)
        self.assertEqual(approximate.build_report(self.plans, PACK_DIR / "terrain_classes.json"),
                         load(PACK_DIR / "approximations.json"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
