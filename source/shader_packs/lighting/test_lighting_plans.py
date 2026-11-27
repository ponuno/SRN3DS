"""Host test: lighting pack exact + approximate recipes against the retail IR.

Run: ``python source/shader_packs/lighting/test_lighting_plans.py``

The test needs the host corpus (``E:\\host_sm3``) for the IR half.  Without it
the committed JSON/coverage checks still run.
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
        cls.tev = [e for e in cls.doc["entries"] if e.get("kind") == "tev"]
        cls.vs = [e for e in cls.doc["entries"]
                  if e.get("kind") == "pica_vs"]
        cls.exact = [e for e in cls.tev
                     if e.get("quality") == "exact"]
        cls.approx = [e for e in cls.tev
                      if e.get("quality") == "approx"]

    def test_exact_recipes_and_full_contract(self):
        self.assertEqual(len(self.exact), 5)
        self.assertEqual({e["recipe"] for e in self.exact},
                         {"base_lit", "base_lit_shadow", "base_lit_contract",
                          "direct_4_stage"})
        for entry in self.exact:
            self.assertEqual(entry["contract"], "full", entry["id"])
        chunks = {r["sha256"]: r["chunks"] for r in self.doc["report"]}
        self.assertGreaterEqual(
            sum(chunks[e["sha256"]] for e in self.exact), 20)

    def test_approx_budget_and_classes(self):
        self.assertEqual(len(self.approx), 100)
        self.assertEqual(len(self.exact) + len(self.approx), 105)
        classes = {e["approx_class"] for e in self.approx}
        for want in ("point", "specular", "envmap", "subsurface", "backlight"):
            self.assertIn(want, classes)
        loops = [e for e in self.approx
                 if e["approximation"]["loop_truncated_to"] > 0]
        self.assertGreaterEqual(len(loops), 10)
        for entry in loops:
            self.assertIn(entry["approximation"]["loop_truncated_to"], (1, 2))
        with_rcp = [e for e in self.approx
                    if e["approximation"]["rcp_rsq"]]
        self.assertGreaterEqual(len(with_rcp), 50)
        for entry in with_rcp:
            for row in entry["approximation"]["rcp_rsq"]:
                self.assertIn(row["mode"], ("linear", "constant"))
                self.assertEqual(row["samples"], 64)
                self.assertEqual(len(row["domain"]), 2)
        for entry in self.approx:
            self.assertTrue(entry["approximation"]["def_literals"])
            self.assertIn(entry["approximation"]["control_flow"]["mode"],
                          ("straight_line", "loop_truncated",
                           "branchless_select"))

    def test_approx_rows_declare_loss_and_error(self):
        for entry in self.approx:
            self.assertEqual(entry["quality"], "approx")
            self.assertIsInstance(entry["approximation"], dict)
            self.assertTrue(entry["approximation"]["lost_terms"])
            self.assertTrue(entry["approximation"]["reason"])
            err = entry["approx_error"]
            self.assertEqual(err["samples"], 32)
            self.assertLess(err["mean_abs_error"], 0.2, entry["id"])
            self.assertLessEqual(err["max_abs_error"], 1.0, entry["id"])

    def test_no_draw_or_runtime_claim(self):
        for entry in self.tev:
            self.assertEqual(entry["draw_enabled"], 0, entry["id"])
            self.assertNotEqual(entry.get("runtime"), 1, entry["id"])
            self.assertEqual(plan_verify.derive_status(entry), "translated")
            self.assertTrue(entry["preconditions"])
        drawn = [e for e in self.vs if e["draw_enabled"] == 1]
        self.assertEqual(len(drawn), 6)
        for entry in self.vs:
            if entry["draw_enabled"] == 1:
                self.assertEqual(entry["runtime"], 1, entry["id"])
                self.assertEqual(entry["quality"], "exact", entry["id"])
                self.assertEqual(entry["codegen"], "emitted-verified",
                                 entry["id"])
            self.assertTrue(entry["preconditions"])

    def test_schema_and_limits(self):
        schema = plan_verify.load_schema()
        errors = []
        for entry in self.doc["entries"]:
            plan_verify.validate_entry(entry, errors, entry["id"])
        for entry in self.tev:
            self.assertLessEqual(entry["stages"], plan_verify.MAX_STAGES)
            self.assertLessEqual(len(entry["textures"]), C.TEV_MAX_TEXTURES)
            self.assertLessEqual(len(entry["constants"]), C.TEV_MAX_CONSTANTS)
        self.assertEqual(errors, [])
        self.assertTrue(schema)

    def test_pica_vs_contracts_are_emitted(self):
        self.assertEqual(len(self.vs), 130)
        self.assertTrue(all(e["codegen"] == "emitted-verified"
                            for e in self.vs))
        self.assertEqual(sum(1 for e in self.vs if e["draw_enabled"] == 1), 6)
        self.assertTrue(all((e.get("runtime", 0) == 1)
                            == (e["draw_enabled"] == 1) for e in self.vs))
        exact = [e for e in self.vs if e["quality"] == "exact"]
        approx = [e for e in self.vs if e["quality"] == "approx"]
        self.assertEqual(len(exact) + len(approx), 130)
        for entry in self.vs:
            emitter = entry["emitter"]
            self.assertEqual(emitter["picasso"], "pass", entry["id"])
            self.assertGreater(emitter["shbin_bytes"], 0, entry["id"])
            self.assertLessEqual(emitter["max_abs_position"], 1e-4, entry["id"])
            self.assertLessEqual(emitter["uniform_rows"], 16, entry["id"])
            self.assertLessEqual(len(entry["outputs"]), 7, entry["id"])
            path = os.path.join(HERE, emitter["shbin"])
            self.assertTrue(os.path.exists(path), path)
            if entry["quality"] == "approx":
                self.assertTrue(entry.get("approximation"), entry["id"])
            self.assertTrue(entry.get("varying_map"), entry["id"])
        with open(os.path.join(HERE, "vs_contracts_report.json"),
                  encoding="utf-8") as handle:
            report = json.load(handle)
        self.assertEqual(report["contracts"], 130)
        self.assertEqual(report["emitted"], 130)
        self.assertEqual(report["refused"], 0)

    def test_refusals_partition_the_pack(self):
        with open(os.path.join(HERE, "refusals.json"), encoding="utf-8") as h:
            refusals = json.load(h)
        summary = refusals["summary"]
        self.assertEqual(summary["refused"] + summary["translated"], 4991)
        self.assertEqual(len(refusals["hashes"]), summary["refused"])
        self.assertEqual(len({r["sha256"] for r in refusals["hashes"]}),
                         summary["refused"])

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
        self.assertGreater(forms["lit_subsurface"]["hashes"], 1000)

    @unittest.skipUnless(os.path.isdir(CORPUS), "host corpus absent")
    def test_exact_recipes_reproduce_and_verify(self):
        corpus = json.load(open(os.path.join(CORPUS, "corpus.json")))
        chunks = LP.load_chunks(os.path.join(CORPUS, "ps_chunks.bin"))
        first = {}
        counts = {}
        for row in corpus["rows"]:
            if row["kind"] != "ps" or row["family"] not in (
                    "lighting/deferred/shadows", "particles/effects"):
                continue
            first.setdefault(row["sha256"], row)
            counts[row["sha256"]] = counts.get(row["sha256"], 0) + 1
        want = {e["sha256"]: e for e in self.exact}
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
            if want[sha]["recipe"].startswith("direct"):
                rewritten = LP._ssa_rewrite(ir)
                direct, _, why = C.plan_ps(rewritten, ignore_def_range=True)
                self.assertIsNotNone(direct, why)
                self.assertEqual(direct["stage_data"], want[sha]["stage_data"])
            else:
                check = LP.verify(ir, plan, shadow)
                self.assertTrue(check["ok"], check)

    def test_approximation_negative_control(self):
        """A plan claimed as approximate must be wrong without its loss note."""
        entry = next(e for e in self.approx
                     if e["approximation"]["rcp_rsq"])
        self.assertGreater(entry["approx_error"]["mean_abs_error"], 0.0)
        self.assertTrue(entry["approximation"]["lost_terms"])
        tex = [[0.25, 0.5, 0.75, 1.0] for _ in range(3)]
        consts = {r: [0.1 * (r + 1), 0.2, 0.3, 1.0] for r in range(32)}
        primary = [0.5, 0.4, 0.3, 1.0]
        frag = [0.2, 0.1, 0.05, 0.0]
        good = LP.eval_plan(entry, tex, consts, primary, frag)
        bad = copy.deepcopy(entry)
        bad["stage_data"][1]["src_rgb"][2] = C.SRC_PREV
        broken = LP.eval_plan(bad, tex, consts, primary, frag)
        self.assertNotEqual(good, broken)

    @unittest.skipUnless(os.path.isdir(CORPUS), "host corpus absent")
    def test_contract_violation_is_not_accepted(self):
        corpus = json.load(open(os.path.join(CORPUS, "corpus.json")))
        chunks = LP.load_chunks(os.path.join(CORPUS, "ps_chunks.bin"))
        row = next(r for r in corpus["rows"]
                   if r["sha256"].startswith("8699c020"))
        ir = C.build_ir(chunks[row["index"]])
        plan, shadow = LP.recognise(ir)
        bad = copy.deepcopy(plan)
        bad["stage_data"][1]["src_rgb"][2] = C.SRC_TEX0
        self.assertFalse(LP.verify(ir, bad, shadow)["ok"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
