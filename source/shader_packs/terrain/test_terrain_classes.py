#!/usr/bin/env python3
"""Pack-local coverage and canonical-IR regression tests for terrain."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import unittest
from pathlib import Path

PACK_DIR = Path(__file__).resolve().parent
REPO = PACK_DIR.parents[2]
SPEC = importlib.util.spec_from_file_location("terrain_classify", PACK_DIR / "classify.py")
classify = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(classify)


def load(path: Path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


class TerrainClassTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = load(PACK_DIR / "terrain_classes.json")
        cls.manifest = load(PACK_DIR / "manifest.json")
        cls.rows = cls.table["hashes"]

    def test_manifest_partition_is_exact(self):
        counts = {row[0]: row[3] for row in self.manifest["hashes"]}
        self.assertEqual(len(counts), 247)
        self.assertEqual(sum(counts.values()), 327)
        self.assertEqual({row["sha256"]: row["chunks"] for row in self.rows}, counts)

    def test_class_coverage(self):
        expected = {
            "terrain.height_layers": (120, 160),
            "terrain.multitexture_12_13": (60, 80),
            "terrain.multitexture_14_15_lod": (60, 80),
            "terrain.lod_noise_overlay": (1, 1),
            "terrain.world_map_overlay": (5, 5),
            "terrain.lod_world_map_overlay": (1, 1),
        }
        actual = {row["id"]: (row["hashes"], row["chunks"])
                  for row in self.table["classes"]}
        self.assertEqual(actual, expected)
        self.assertEqual(sum(value[0] for value in actual.values()), 247)
        self.assertEqual(sum(value[1] for value in actual.values()), 327)

    def test_no_false_acceptance(self):
        self.assertEqual(self.table["totals"], {
            "hashes": 247, "chunks": 327, "translated": 0,
            "draw_enabled": 0, "refused": 247, "simple_subset_hashes": 0,
            "needs_multipass_hashes": 228, "needs_multipass_chunks": 300,
            "stage_budget_hashes": 19, "stage_budget_chunks": 27,
            "control_flow_hashes": 54, "hard_def_hashes": 247,
        })
        plans = load(REPO / "docs" / "shader_plans.json")["entries"]
        pack_plans = load(PACK_DIR / "plans.json")["entries"]
        self.assertEqual(pack_plans, [])
        terrain_hashes = {row["sha256"] for row in self.rows}
        self.assertFalse(terrain_hashes & {entry["sha256"] for entry in plans})
        self.assertFalse(terrain_hashes & {entry["sha256"] for entry in pack_plans})
        for row in self.rows:
            self.assertEqual((row["translated"], row["draw_enabled"]), (0, 0))

    def test_stage_lower_bound_and_named_refusals(self):
        for row in self.rows:
            self.assertEqual(row["stage_demand_min"],
                             row["instructions"] - row["opcodes"]["texld"])
            self.assertGreater(row["stage_demand_min"], 6)
            if row["texture_layers"] > 3:
                self.assertEqual(row["refusal"]["name"], "terrain.needs_multipass")
                self.assertEqual(row["refusal"]["minimum_passes"],
                                 (row["texture_layers"] + 2) // 3)
            else:
                self.assertEqual(row["refusal"]["name"], "terrain.stage_budget")

    def test_def_range_is_not_the_only_blocker(self):
        for row in self.rows:
            self.assertTrue(row["def_hard_values"])
            self.assertIn("terrain.normal_basis_prebake_required", row["blockers"])
            self.assertIn("terrain.single_accumulator_scheduler_required",
                          row["blockers"])
            self.assertIn("terrain.material_contract_missing", row["blockers"])
        self.assertEqual(sum(bool(row["control_flow"]) for row in self.rows), 54)

    def test_overlay_blend_is_a_separate_contract(self):
        overlays = [row for row in self.rows
                    if row["features"]["alpha_blend"] == "required_by_overlay_contract"]
        self.assertEqual(len(overlays), 7)
        self.assertEqual({row["class"] for row in overlays}, {
            "terrain.lod_noise_overlay", "terrain.world_map_overlay",
            "terrain.lod_world_map_overlay",
        })
        self.assertTrue(all("not encoded in PS" in row["features"]["blend_evidence"]
                            for row in overlays))

    def test_named_refusal_catalog(self):
        refusals = load(PACK_DIR / "refusals.json")
        self.assertEqual(refusals["counts"], {
            "terrain.needs_multipass": 228, "terrain.stage_budget": 19,
        })
        self.assertEqual(refusals["source_table_sha256"],
                         hashlib.sha256((PACK_DIR / "terrain_classes.json").read_bytes()).hexdigest())
        self.assertEqual(len(refusals["hashes"]), 247)
        for refusal, row in zip(refusals["hashes"], self.rows):
            self.assertEqual(refusal["sha256"], row["sha256"])
            self.assertEqual(refusal["name"], row["refusal"]["name"])
            self.assertEqual(refusal["blockers"], row["blockers"])

    def test_canonical_ir_regeneration(self):
        corpus = Path(r"E:\host_sm3")
        if not (corpus / "corpus.json").exists() or not (corpus / "ps_ir.bin").exists():
            self.skipTest("host canonical IR absent")
        regenerated = classify.build_table(corpus, PACK_DIR / "manifest.json")
        self.assertEqual(regenerated, self.table)


if __name__ == "__main__":
    unittest.main(verbosity=2)
