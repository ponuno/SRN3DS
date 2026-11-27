#!/usr/bin/env python3
"""Skinned character pack tests: catalog vs manifest, schema, named refusals.

Offline only (no retail archive): the plans themselves are regenerated from
the archive by ``pack_plans.py verify --data``; this test guards the committed
catalog, the coverage partition and the draw rules.  It also re-derives every
plan when the host corpus/archive is present.

    python -m unittest discover -s source/shader_packs/skinning -p "test_*.py"
"""

from __future__ import annotations

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for path in (REPO, os.path.join(REPO, "tools"),
             os.path.join(REPO, "tools", "shaders")):
    if path not in sys.path:
        sys.path.insert(0, path)

import pack_plans  # noqa: E402
from shaders import plan_verify  # noqa: E402


class SkinningCatalogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = pack_plans.load_catalog()
        cls.manifest = pack_plans._load(pack_plans.MANIFEST)

    def test_catalog_partitions_the_pack_manifest(self):
        report = pack_plans.verify_document(self.doc, pack_plans._pack_hashes())
        self.assertEqual(report["errors"], [])

    def test_counts(self):
        coverage = self.doc["coverage"]
        self.assertEqual(coverage["translated"], 202)
        self.assertEqual(coverage["draw_enabled"], 4)
        self.assertEqual(coverage["refused"], 992)
        self.assertEqual(coverage["translated"] + coverage["refused"],
                         self.manifest["unique_hashes"])

    def test_every_plan_is_a_schema_valid_contract(self):
        schema = plan_verify.load_schema()
        for entry in self.doc["entries"]:
            errors = []
            plan_verify.validate_entry(entry, errors, entry["id"])
            self.assertEqual(errors, [], entry["id"])
            self.assertIn(entry["quality"], ("exact", "approx"), entry["id"])
            if entry["kind"] == "pica_vs":
                self.assertEqual(entry["stage"], "vs_3_0")
                self.assertLessEqual(len(entry["outputs"]), 7)
                self.assertLessEqual(len(entry["inputs"]), 16)
                self.assertLessEqual(entry["uniform_registers"], 96)
                self.assertTrue(entry["skinning"], entry["id"])
                self.assertIn(entry["codegen"], ("pending", "emitted-verified"))
                packing = entry.get("packing") or {}
                if packing.get("components"):
                    self.assertLessEqual(len(packing["components"]), 24,
                                         entry["id"])
                for drop in packing.get("dropped", []):
                    self.assertEqual(drop["usage"], 10, entry["id"])
                    self.assertEqual(drop["usage_index"], 1, entry["id"])
                if entry["quality"] == "approx":
                    self.assertTrue(entry.get("approximation"), entry["id"])
                    self.assertTrue(entry.get("contract_outputs"), entry["id"])
                if entry["codegen"] == "emitted-verified":
                    self.assertEqual(entry["draw_enabled"],
                                     1 if entry["id"] in pack_plans.DRAWN else 0)
                    if entry["draw_enabled"] == 1:
                        self.assertEqual(plan_verify.derive_status(entry),
                                         "draw-enabled", entry["id"])
                        self.assertIn("emitted_code_upload_path",
                                      entry["preconditions"])
                    self.assertEqual(entry.get("runtime"),
                                     1 if entry["draw_enabled"] else 0)
                else:
                    self.assertEqual(entry["draw_enabled"], 0, entry["id"])
            else:
                self.assertEqual(entry["kind"], "tev")
                self.assertEqual(entry["stage"], "ps_3_0")
                self.assertEqual(entry["draw_enabled"], 0, entry["id"])
                # The PS class is a documented basic-tint approximation.
                self.assertEqual(entry["quality"], "approx", entry["id"])
                self.assertTrue(entry.get("approximation"), entry["id"])
        self.assertEqual(schema["$defs"]["entry"]["properties"]["kind"]
                         ["enum"], ["tev", "pica_vs"])

    def test_every_refusal_is_named_and_classified(self):
        classes = {row[0]: row[2] for row in self.manifest["hashes"]}
        for refusal in self.doc["refusals"]:
            self.assertTrue(refusal["reason"])
            self.assertTrue(refusal["needs"])
            self.assertEqual(refusal["class"], classes[refusal["sha256"]])
        reasons = {r["reason"] for r in self.doc["refusals"]}
        classes = {r["class"] for r in self.doc["refusals"]}
        self.assertIn("def_literal_out_of_range", classes)
        self.assertIn("unsupported_opcode", classes)
        self.assertIn("ps_texture_budget", reasons)
        self.assertIn("ps_not_straight_line", reasons)

    def test_no_hash_is_both_planned_and_refused(self):
        plans = {e["sha256"] for e in self.doc["entries"]}
        refused = {r["sha256"] for r in self.doc["refusals"]}
        self.assertFalse(plans & refused)
        self.assertEqual(plans | refused,
                         {row[0] for row in self.manifest["hashes"]})

    def test_draw_is_enabled_only_with_the_full_emitter_contract(self):
        for entry in self.doc["entries"]:
            if entry["draw_enabled"] == 1:
                self.assertTrue(entry.get("preconditions"), entry["id"])
                self.assertEqual(entry["codegen"], "emitted-verified")
                emitter = entry["emitter"]
                self.assertTrue(emitter["shbin"].endswith(".shbin"))
                self.assertGreater(emitter["shbin_bytes"], 0)
                blob = open(os.path.join(HERE, emitter["shbin"]), "rb").read()
                import hashlib
                self.assertEqual(hashlib.sha256(blob).hexdigest(),
                                 emitter["shbin_sha256"], entry["id"])
                self.assertEqual(len(blob), emitter["shbin_bytes"])
        self.assertEqual(self.doc["coverage"]["draw_enabled"], 4)


class SkinningSymbolicTest(unittest.TestCase):
    DATA = r"E:\SteamLibrary\steamapps\common\Skyrim\Data"

    def test_plans_regenerate_from_the_archive(self):
        if not os.path.exists(os.path.join(self.DATA, pack_plans.ARCHIVE)):
            self.skipTest("retail archive absent")
        doc = pack_plans._load(pack_plans.PLANS)
        report = pack_plans.verify_symbolic(doc, self.DATA)
        self.assertEqual(report["status"], "pass", report["problems"][:5])
        self.assertEqual(report["checked"], len(doc["entries"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
