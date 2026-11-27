#!/usr/bin/env python3
"""Phase-1 SM3 tests: differential fields, plan schema/verifier, pack partition.

The differential half is pure: a handful of MojoShader ``d3d`` disassembly
lines are parsed into canonical fields and compared against the project
decoder's field model, including the traps found while building it
(``_sat_pp`` stacking, ``r0_abs.x`` order, ``if_lt`` comparison encoding,
``texldp`` control bits, relative-addressed sources).

The schema half validates the committed ``docs/shader_plans.json`` and then
proves the verifier rejects tampered copies.  The pack half checks that the six
committed manifests partition all 10,475 chunks / 6,659 hashes.

If the host corpus exists (``E:\\host_sm3``) the measured differential report is
also asserted to have zero divergences; otherwise that assertion is skipped and
the test still covers the pure layers.
"""

from __future__ import annotations

import copy
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "shaders"))

import shader_packs  # noqa: E402
from shaders import asm, differential, plan_verify  # noqa: E402


class MojoShaderFieldTest(unittest.TestCase):
    def parse(self, text):
        return differential.parse_mojoshader(text)

    def test_dest_mask_and_stacked_modifiers(self):
        body = self.parse("mul_sat_pp r0.xy, c1, v2.zyw")["body"]
        self.assertEqual(len(body), 1)
        op = body[0]
        self.assertEqual(op["opcode"], asm.NAME_TO_OPCODE["mul"])
        self.assertEqual(op["dst"], {"type": 0, "index": 0, "mask": 0x3,
                                     "modifier": 3})  # sat | partial precision
        # MojoShader trims trailing duplicates: .zyw means z,y,w,w.
        self.assertEqual([s["swizzle"] for s in op["srcs"]],
                         ["xyzw", "zyww"])

    def test_abs_and_negate(self):
        op = self.parse("add r0.x, r0_abs.x, -c2.y")["body"][0]
        self.assertEqual(op["dst"]["mask"], 0x1)
        self.assertEqual(op["srcs"][0]["modifier"], 11)   # D3DSPSM_ABS
        self.assertEqual(op["srcs"][0]["swizzle"], "xxxx")
        self.assertEqual(op["srcs"][1]["type"], asm.REG_CONST)
        self.assertEqual(op["srcs"][1]["modifier"], 1)    # negate

    def test_comparison_lives_in_the_opcode_token(self):
        op = self.parse("if_lt r1.x, -r2.z")["body"][0]
        self.assertEqual(op["opcode"], 0x29)              # ifc
        self.assertIsNone(op["dst"])
        self.assertEqual(op["comp"], 4)                   # D3DSPC_LT
        self.assertEqual(len(op["srcs"]), 2)

    def test_texld_projection_control(self):
        op = self.parse("texldp r5, r6, s0")["body"][0]
        self.assertEqual(op["opcode"], 0x42)
        self.assertEqual(op["controls"], 0x01)            # D3DSI_TEXLD_PROJECT
        plain = self.parse("texld r0, v0, s0")["body"][0]
        self.assertEqual(plain["controls"], 0)

    def test_relative_source_is_not_an_operand(self):
        op = self.parse("mul r0, v2.y, c0[a0.y]")["body"][0]
        self.assertEqual(len(op["srcs"]), 2)
        self.assertIsNone(op["srcs"][0]["relative"])
        # Full tuple: (index reg type, index reg number, component, offset).
        self.assertEqual(op["srcs"][1]["relative"], (asm.REG_ADDR, 0, "y", 0))

    def test_dcl_usage_index_and_partial_precision(self):
        decls = self.parse("dcl_texcoord1_pp v1\ndcl_2d s0")["decls"]
        self.assertEqual(decls[0], (asm.REG_INPUT, 1, 5, 1, 0))
        self.assertEqual(decls[1], (asm.REG_SAMPLER, 0, 0, 0, 2))

    def test_compare_detects_each_field_class(self):
        mj = self.parse("mul_sat r0.xy, c1, v2")

        def project(mask=0x3, modifier=1, swizzle="xyzw"):
            return {"error": None, "defs": [], "decls": [],
                    "body": [{"opcode": asm.NAME_TO_OPCODE["mul"],
                              "dst": {"type": 0, "index": 0, "mask": mask,
                                      "modifier": modifier},
                              "srcs": [{"type": asm.REG_CONST, "index": 1,
                                        "swizzle": "xyzw", "modifier": 0,
                                        "relative": None},
                                       {"type": asm.REG_INPUT, "index": 2,
                                        "swizzle": swizzle, "modifier": 0,
                                        "relative": None}],
                              "comp": None, "controls": 0}]}

        self.assertEqual(differential.compare(mj, project())["status"], "equal")
        self.assertEqual(differential.compare(mj, project(mask=0x1))["status"],
                         "differ_dst")
        self.assertEqual(differential.compare(mj, project(modifier=0))["status"],
                         "differ_saturate")
        self.assertEqual(differential.compare(mj, project(swizzle="xxxx"))["status"],
                         "differ_src_swizzle")


class PlanSchemaTest(unittest.TestCase):
    def setUp(self):
        self.schema = plan_verify.load_schema()
        with open(plan_verify.PLANS_PATH, encoding="utf-8") as handle:
            self.doc = json.load(handle)

    def test_committed_plans_pass(self):
        self.assertEqual(plan_verify.verify_document(self.doc, self.schema), [])
        status = plan_verify.status_table(self.doc)
        self.assertEqual(sum(status.values()), len(self.doc["entries"]))
        # The canonical copy is always draw-enabled; pack catalogs may add
        # exact runtime descriptors on top of it.
        self.assertGreaterEqual(status["draw-enabled"], 1)
        for entry in self.doc["entries"]:
            if entry.get("draw_enabled") == 1:
                self.assertEqual(entry["kind"], "tev", entry["id"])
                if entry.get("quality") is not None:
                    self.assertEqual(entry["quality"], "exact", entry["id"])

    def test_status_is_derived_not_trusted(self):
        doc = copy.deepcopy(self.doc)
        doc["entries"][0]["status"] = "draw-enabled"   # it is draw_enabled=0
        errors = plan_verify.verify_document(doc, self.schema)
        self.assertTrue(any("contradicts derived" in e for e in errors), errors)

    def test_duplicate_hash_is_rejected(self):
        doc = copy.deepcopy(self.doc)
        doc["entries"][1]["sha256"] = doc["entries"][0]["sha256"]
        errors = plan_verify.verify_document(doc, self.schema)
        self.assertTrue(any("duplicate sha256" in e for e in errors), errors)

    def test_pica_vs_draw_requires_verified_emitted_codegen(self):
        doc = copy.deepcopy(self.doc)
        synthetic = {
            "id": "synthetic.vs",
            "sha256": "ab" * 32,
            "stage": "vs_3_0",
            "kind": "pica_vs",
            "draw_enabled": 1,
            "note": "synthetic",
            "inputs": [{"reg_index": 0, "usage": 0, "usage_index": 0,
                        "usage_name": "position"}],
            "outputs": [{"reg_index": 0, "type": 6, "usage": 0,
                         "usage_index": 0, "usage_name": "position"}],
            "preconditions": ["exact_hash_descriptor_match"],
        }
        doc["entries"].append(dict(synthetic))
        errors = plan_verify.verify_document(doc, self.schema)
        self.assertTrue(any("emitted-verified" in e for e in errors), errors)
        self.assertTrue(any("emitter block" in e for e in errors), errors)

        # A verified entry that carries the hardware-upload contract may be
        # draw-enabled (the pack's three emitted programs do exactly this).
        rows = [{"row": row,
                 "d3d": row if row < 12 else 180 + (row - 12) if row < 16 else 184 + (row - 16)}
                for row in range(16)]
        verified = dict(synthetic)
        verified["codegen"] = "emitted-verified"
        verified["emitted_asm"] = "generated/synthetic.vs.v.pica"
        verified["emitter"] = {
            "samples": 256, "tolerance": 1e-4, "max_abs_position": 0.0,
            "max_abs_varying": 0.0, "picasso": "pass", "bones": 4,
            "uniform_rows": 16, "instructions": 35,
            "shbin": "generated/synthetic.vs.shbin",
            "shbin_sha256": "cd" * 32, "shbin_bytes": 388,
            "inputs": [{"usage": 0, "usage_name": "position", "reg_index": 0}],
            "uniforms": rows,
        }
        doc["entries"][-1] = verified
        errors = plan_verify.verify_document(doc, self.schema)
        self.assertEqual(errors, [], errors)

    def test_schema_rules_match_the_loaded_file(self):
        # The verifier mirrors the schema; guard the finite sets both ways.
        rgb = set(self.schema["$defs"]["stage"]["properties"]["op_rgb"]
                  ["items"]["enum"])
        self.assertEqual(rgb, plan_verify.TEV_RGB_OPS)
        self.assertEqual(
            set(self.schema["$defs"]["family"]["enum"]),
            plan_verify.FAMILIES)


class PackPartitionTest(unittest.TestCase):
    def test_committed_manifests_partition_the_census(self):
        self.assertEqual(shader_packs.check(), [])

    def test_six_packs_and_totals(self):
        with open(shader_packs.INDEX_PATH, encoding="utf-8") as handle:
            index = json.load(handle)
        self.assertEqual(sorted(index["packs"]), sorted(shader_packs.PACKS))
        self.assertEqual(index["chunks"], 10475)
        self.assertEqual(index["unique_hashes"], 6659)
        for name, data in index["packs"].items():
            coverage = data["coverage"]
            self.assertEqual(
                coverage["translated"] + coverage["draw-enabled"]
                + coverage["refused"], data["unique_hashes"], name)
        total_draws = sum(d["coverage"]["draw-enabled"]
                          for d in index["packs"].values())
        # The canonical copy is always draw-enabled; certified one-stage
        # post-process plans may add more.
        self.assertGreaterEqual(total_draws, 1)
        runtime = json.load(open(plan_verify.PLANS_PATH, encoding="utf-8"))
        for entry in runtime["entries"]:
            if entry.get("draw_enabled") == 1:
                self.assertEqual(entry["kind"], "tev", entry["id"])
                if entry.get("quality") is not None:
                    self.assertEqual(entry["quality"], "exact", entry["id"])


class MeasuredDifferentialTest(unittest.TestCase):
    REPORT = r"E:\host_sm3\differential.json"

    def test_report_has_no_field_divergence(self):
        if not os.path.exists(self.REPORT):
            self.skipTest("host differential report absent")
        with open(self.REPORT, encoding="utf-8") as handle:
            report = json.load(handle)
        self.assertEqual(report["chunks"], 10475)
        self.assertEqual(report["unique_streams"], 6659)
        self.assertEqual(report["unique_equal"], report["unique_streams"],
                         report.get("top_divergences"))
        self.assertEqual(report["chunks_equal"], report["chunks"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
