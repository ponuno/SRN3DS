#!/usr/bin/env python3
"""Pack tests for the BSA `ui/interface` plan catalogue (offline).

Run with::

    python -m unittest discover -s source\\shader_packs\\ui -p "test_*.py"

These tests do not need the retail archive: they check the committed catalog,
the emitter report and the `.shbin` pins.  `bsa_pack_plans.py verify
--symbolic` re-derives the plans from `Skyrim - Shaders.bsa`.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for _p in (HERE, REPO, os.path.join(REPO, "tools"),
           os.path.join(REPO, "tools", "shaders")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import bsa_pack_plans as P  # noqa: E402


def _load(name):
    with open(os.path.join(HERE, name), encoding="utf-8") as handle:
        return json.load(handle)


class BsaPlanCatalog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = _load("plans.json")
        cls.manifest = _load("manifest.json")
        cls.report = _load("bsa_vs_report.json")
        cls.hashes = {row[0]: row for row in cls.manifest["hashes"]}
        cls.bsa = [e for e in cls.doc["entries"]
                   if e.get("id", "").startswith("ui.")]
        cls.ps = [e for e in cls.bsa if e["kind"] == "tev"]
        cls.vs = [e for e in cls.bsa if e["kind"] == "pica_vs"]

    def test_every_manifest_hash_has_exactly_one_plan(self):
        shas = [e["sha256"] for e in self.bsa]
        self.assertEqual(len(shas), len(set(shas)))
        self.assertEqual(set(shas), set(self.hashes))
        self.assertEqual(len(self.bsa), 113)
        self.assertEqual(len(self.ps), 73)
        self.assertEqual(len(self.vs), 40)

    def test_verify_document(self):
        report = P.verify()
        self.assertTrue(report["ok"], report["errors"])
        self.assertEqual(report["coverage"]["translated"], 113)
        self.assertEqual(report["coverage"]["refused"], 0)

    def test_quality_and_approximation(self):
        for entry in self.bsa:
            self.assertIn(entry.get("quality"), ("exact", "approx"),
                          entry["id"])
            if entry["quality"] == "approx":
                self.assertTrue(entry.get("approximation"), entry["id"])
        # A def outside RGBA8 must be derived, not clamped: every literal the
        # sanitised planner carries over is preserved bit-for-bit, and at
        # least one plan proves the >1 path (the retail UI contrast constant).
        derived = [e for e in self.ps if e.get("constant_values")]
        self.assertTrue(derived)
        out_of_range = [v for e in derived
                        for values in e["constant_values"].values()
                        for v in values if v < 0.0 or v > 1.0]
        self.assertTrue(out_of_range)
        # The retail UI contrast slope is 1.5/2.0; it must be preserved, not
        # clamped to 1.0.
        self.assertTrue(any(abs(v) >= 1.5 for v in out_of_range))
        self.assertLess(max(abs(v) for v in out_of_range), 16.0)

    def test_ps_contract_and_preconditions(self):
        for entry in self.ps:
            self.assertGreaterEqual(entry["stages"], 1)
            self.assertLessEqual(entry["stages"], 6)
            self.assertEqual(len(entry["stage_data"]), entry["stages"])
            self.assertTrue(entry.get("preconditions"), entry["id"])
            contract = entry.get("material_contract") or {}
            self.assertTrue(contract.get("complete"), entry["id"])
            if entry.get("quality") == "approx":
                self.assertTrue(contract.get("simplified"), entry["id"])
            self.assertEqual(entry["runtime"], 1, entry["id"])
        dp4 = [e for e in self.ps
               if "normal_tangent_basis_prebaked" in e["preconditions"]]
        self.assertEqual(len(dp4), 22)
        self.assertTrue(all(e["quality"] == "approx" for e in dp4))

    def test_vs_emitted_proof_is_pinned(self):
        passed = [row for row in self.report["entries"]
                  if row.get("status") == "pass"]
        self.assertEqual(len(passed), 40)
        self.assertEqual(self.report["coverage"]["emitted"], 40)
        for row in passed:
            entry = next(e for e in self.vs if e["sha256"] == row["sha256"])
            self.assertEqual(entry["codegen"], "emitted-verified")
            self.assertEqual(entry["draw_enabled"] in (0, 1), True)
            self.assertEqual(entry["draw_enabled"] == 1,
                             entry["runtime"] == 1)
            emitter = entry["emitter"]
            self.assertEqual(emitter["bones"], 0)
            self.assertGreaterEqual(emitter["samples"], 1)
            self.assertEqual(emitter["picasso"], "pass")
            self.assertLessEqual(emitter["max_abs_position"], 1e-4)
            self.assertTrue(
                emitter["max_abs_varying"] <= 1e-4
                or emitter.get("max_rel_varying", 1.0) <= 1e-6,
                entry["id"])
            shbin = os.path.join(HERE, emitter["shbin"])
            self.assertTrue(os.path.exists(shbin))
            with open(shbin, "rb") as handle:
                self.assertEqual(hashlib.sha256(handle.read()).hexdigest(),
                                 emitter["shbin_sha256"])
            asm = os.path.join(HERE, entry["emitted_asm"])
            self.assertTrue(os.path.exists(asm))

    def test_vs_runtime_subset_is_certified(self):
        runtime = [e for e in self.vs if e.get("runtime") == 1]
        self.assertEqual(len(runtime), 6)
        self.assertTrue(all(e["draw_enabled"] == 1
                            and e["codegen"] == "emitted-verified"
                            and e["quality"] == "exact" for e in runtime))
        self.assertTrue(all(e["draw_enabled"] == 0
                            for e in self.vs if e.get("runtime") != 1))

    def test_runtime_table_covers_every_emitted_program(self):
        for name, macro, count in (
                ("d3d9_pica_vs_ui_table.inc",
                 "#define D9_PICA_VS_UI_ENTRY_COUNT", 16),
                ("d3d9_pica_vs_contract_table.inc",
                 "#define D9_PICA_VS_CONTRACT_ENTRY_COUNT", 6)):
            path = os.path.join(REPO, "source", name)
            self.assertTrue(os.path.exists(path), name)
            with open(path, encoding="ascii") as handle:
                text = handle.read()
            self.assertIn(macro, text)
            self.assertEqual(text.count('.v.pica"'), count, name)

    def test_manifest_coverage_matches_catalog(self):
        coverage = self.manifest["coverage"]
        self.assertEqual(coverage["translated"],
                         sum(1 for e in self.bsa if e["draw_enabled"] == 0))
        self.assertEqual(coverage["draw-enabled"],
                         sum(1 for e in self.bsa if e["draw_enabled"] == 1))
        self.assertEqual(coverage["refused"], 0)
        self.assertEqual(len(self.manifest["plans"]), 113)


if __name__ == "__main__":
    unittest.main()
