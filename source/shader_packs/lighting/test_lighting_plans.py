"""Host test: lighting pack recipes against the retail IR.

Run: ``python source/shader_packs/lighting/test_lighting_plans.py``

The test needs the host corpus (``E:\\host_sm3``) for the IR half.  Without it
the bytecode checks are skipped and the committed JSON/coverage checks still
run.  It is intentionally pack-local: the shared host suite enumerates its own
test list and this agent does not edit that list.
"""

from __future__ import annotations

import copy
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, HERE)

from tools.shaders import compile as C  # noqa: E402
from tools.shaders import plan_verify  # noqa: E402
import lighting_plans as LP  # noqa: E402

CORPUS = r"E:\host_sm3"
PLANS = os.path.join(HERE, "plans.json")
COVERAGE = os.path.join(HERE, "coverage.json")


class CommittedPlansTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(PLANS, encoding="utf-8") as handle:
            cls.doc = json.load(handle)
        with open(COVERAGE, encoding="utf-8") as handle:
            cls.coverage = json.load(handle)

    def test_two_recipes_and_ten_chunks(self):
        self.assertEqual(len(self.doc["entries"]), 2)
        self.assertEqual(self.doc["summary"]["hashes"], 2)
        self.assertEqual(self.doc["summary"]["chunks"], 10)
        self.assertEqual({e["recipe"] for e in self.doc["entries"]},
                         {"base_lit", "base_lit_shadow"})

    def test_no_draw_claim(self):
        for entry in self.doc["entries"]:
            self.assertEqual(entry["draw_enabled"], 0, entry["id"])
            self.assertEqual(plan_verify.derive_status(entry), "translated")
            self.assertTrue(entry["preconditions"])

    def test_schema_and_limits(self):
        schema = plan_verify.load_schema()
        errors = []
        for entry in self.doc["entries"]:
            plan_verify.validate_entry(entry, errors, entry["id"])
            self.assertLessEqual(entry["stages"], plan_verify.MAX_STAGES)
            self.assertLessEqual(len(entry["textures"]),
                                 C.TEV_MAX_TEXTURES)
            self.assertLessEqual(len(entry["constants"]),
                                 C.TEV_MAX_CONSTANTS)
        self.assertEqual(errors, [])

    def test_coverage_sums_and_base_lit_is_small(self):
        self.assertEqual(self.coverage["unique_hashes"], 4991)
        self.assertEqual(self.coverage["chunks"], 7459)
        forms = {row["form"]: row for row in self.coverage["forms"]}
        self.assertEqual(sum(row["hashes"] for row in self.coverage["forms"]),
                         self.coverage["unique_hashes"])
        self.assertEqual(sum(row["chunks"] for row in self.coverage["forms"]),
                         self.coverage["chunks"])
        self.assertEqual(forms["base_lit"]["hashes"], 2)
        self.assertEqual(forms["base_lit"]["chunks"], 6)
        self.assertEqual(forms["base_lit_shadow"]["hashes"], 1)
        self.assertEqual(forms["base_lit_shadow"]["chunks"], 5)
        # The pack is procedural: loops/optional maps dominate.
        self.assertGreater(forms["lit_subsurface"]["hashes"], 1000)

    @unittest.skipUnless(os.path.isdir(CORPUS), "host corpus absent")
    def test_recipes_reproduce_and_verify(self):
        with open(os.path.join(CORPUS, "corpus.json"),
                  encoding="utf-8") as handle:
            corpus = json.load(handle)
        chunks = LP.load_chunks(os.path.join(CORPUS, "ps_chunks.bin"))
        first = {}
        counts = {}
        for row in corpus["rows"]:
            if row["kind"] != "ps" or row["family"] not in (
                    "lighting/deferred/shadows", "particles/effects"):
                continue
            first.setdefault(row["sha256"], row)
            counts[row["sha256"]] = counts.get(row["sha256"], 0) + 1
        want = {e["sha256"]: e for e in self.doc["entries"]}
        chunks_by_sha = {r["sha256"]: r["chunks"] for r in self.doc["report"]}
        for sha, row in first.items():
            if sha not in want:
                continue
            ir = C.build_ir(chunks[row["index"]])
            plan, shadow = LP.recognise(ir)
            self.assertIsNotNone(plan, sha)
            self.assertEqual(plan["sha256"], sha)
            self.assertEqual(plan["stages"], want[sha]["stages"])
            self.assertEqual(plan["stage_data"], want[sha]["stage_data"])
            self.assertEqual(counts[sha], chunks_by_sha[sha])
            check = LP.verify(ir, plan, shadow)
            self.assertTrue(check["ok"], check)

    @unittest.skipUnless(os.path.isdir(CORPUS), "host corpus absent")
    def test_contract_violation_is_not_accepted(self):
        with open(os.path.join(CORPUS, "corpus.json"),
                  encoding="utf-8") as handle:
            corpus = json.load(handle)
        chunks = LP.load_chunks(os.path.join(CORPUS, "ps_chunks.bin"))
        row = next(r for r in corpus["rows"]
                   if r["sha256"].startswith("8699c020"))
        ir = C.build_ir(chunks[row["index"]])
        plan, shadow = LP.recognise(ir)
        bad = copy.deepcopy(plan)
        # Drop the ambient/emissive contract: the plan's addend is no longer v2.
        bad["stage_data"][1]["src_rgb"][2] = C.SRC_TEX0
        self.assertFalse(LP.verify(ir, bad, shadow)["ok"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
