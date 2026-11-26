#!/usr/bin/env python3
"""Host tests for the offline FXP -> PICA200 compiler (variant B).

Synthetic fixtures are assembled here from semantic fields; the real-archive
cases read the local retail ``Skyrim - Shaders.bsa`` read-only and are skipped
when it is absent.  No game bytes are committed.
"""

from __future__ import annotations

import json
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import compile as C  # noqa: E402
import fxp_compile  # noqa: E402
import fxp_census  # noqa: E402

PS_VERSION = 0xFFFF0300
VS_VERSION = 0xFFFE0300
END = 0x0000FFFF
IDENTITY = 0xE4
SW_X, SW_Y, SW_Z, SW_W = 0x00, 0x55, 0xAA, 0xFF


def reg_type(token):
    return ((token >> 28) & 7) | ((token & 0x1800) >> 8)


def dst(type_, reg, mask=0xF, mod=0):
    return (0x80000000 | ((type_ & 7) << 28) | ((type_ & 0x18) << 8)
            | (reg & 0x7FF) | ((mask & 0xF) << 16) | ((mod & 0xF) << 20))


def src(type_, reg, sw=IDENTITY, mod=0):
    return (0x80000000 | ((type_ & 7) << 28) | ((type_ & 0x18) << 8)
            | (reg & 0x7FF) | ((sw & 0xFF) << 16) | ((mod & 0xF) << 24))


def op(opcode, operands):
    return opcode | (operands << 24)


def fbits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def dcl_input(usage, usage_index, reg):
    return [op(0x1F, 2), (usage | (usage_index << 16)), dst(1, reg)]


def dcl_sampler(reg, tex_type=2):
    return [op(0x1F, 2), tex_type << 27, dst(10, reg)]


def dcl_output(usage, usage_index, reg):
    return [op(0x1F, 2), (usage | (usage_index << 16)), dst(6, reg)]


def def_const(reg, values):
    return [op(0x51, 5), dst(2, reg)] + [fbits(v) for v in values]


def texld(dst_reg, coord_reg, sampler, out=False, mask=0xF):
    return [op(0x42, 3), dst(8 if out else 0, dst_reg, mask),
            src(1, coord_reg), src(10, sampler)]


def alu(opcode, dst_tok, *srcs):
    return [op(opcode, 1 + len(srcs)), dst_tok] + list(srcs)


def program(version, *parts):
    words = [version]
    for part in parts:
        words.extend(part)
    words.append(END)
    return struct.pack("<%dI" % len(words), *words)


def assemble_copy():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_sampler(0),
                   texld(0, 0, 0, out=True))


def assemble_tint():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_input(10, 0, 1),
                   dcl_sampler(0), texld(0, 0, 0),
                   alu(0x05, dst(8, 0), src(0, 0), src(1, 1)))


def assemble_mad():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_input(10, 0, 1),
                   dcl_sampler(0), texld(0, 0, 0),
                   alu(0x04, dst(8, 0), src(0, 0), src(2, 2), src(1, 1)))


def assemble_clamp():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_sampler(0),
                   def_const(0, (1.0, 1.0, 1.0, 1.0)), texld(0, 0, 0),
                   alu(0x0A, dst(8, 0), src(0, 0), src(2, 0)))


def assemble_rcp():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_sampler(0),
                   texld(0, 0, 0), alu(0x06, dst(8, 0), src(0, 0)))


def assemble_dp4():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_sampler(0),
                   texld(0, 0, 0),
                   alu(0x09, dst(8, 0), src(0, 0), src(2, 0)))


def assemble_dependent():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_sampler(0),
                   texld(0, 0, 0),
                   [op(0x42, 3), dst(8, 0, 0xF), src(0, 0), src(10, 0)])


def assemble_out_of_range():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_sampler(0),
                   def_const(0, (1.0, -1.0, 0.0, 0.0)), texld(0, 0, 0),
                   alu(0x05, dst(8, 0), src(0, 0), src(2, 0)))


def assemble_seven_stages():
    parts = [dcl_input(5, 0, 0), dcl_sampler(0), texld(0, 0, 0)]
    for i in range(1, 7):
        parts.append(alu(0x05, dst(0, i), src(0, i - 1), src(2, 0)))
    parts.append(alu(0x05, dst(8, 0), src(0, 6), src(2, 0)))
    return program(PS_VERSION, *parts)


def assemble_branch():
    return program(PS_VERSION, dcl_input(5, 0, 0), dcl_sampler(0),
                   [op(0x28, 0)], texld(0, 0, 0), texld(0, 0, 0, out=True))


def assemble_vs():
    return program(VS_VERSION, dcl_input(0, 0, 0), dcl_input(5, 0, 1),
                   dcl_output(0, 0, 0), dcl_output(5, 0, 1),
                   def_const(0, (1.0, 1.0, 1.0, 1.0)),
                   alu(0x01, dst(6, 1), src(1, 1)),
                   alu(0x04, dst(6, 0), src(1, 0), src(2, 0), src(2, 0)))


class SyntheticTests(unittest.TestCase):
    def test_parse_copy(self):
        ir = C.build_ir(assemble_copy())
        self.assertEqual(ir.kind, "ps")
        self.assertEqual(ir.version_name, "ps_3_0")
        self.assertIsNone(ir.error)
        self.assertEqual(sorted(ir.inputs), [0])
        self.assertEqual(ir.samplers, {0: 2})
        self.assertEqual(len(ir.body), 1)
        self.assertEqual(len(ir.sha256), 64)
        self.assertEqual(ir.sha256, C.build_ir(assemble_copy()).sha256)

    def test_plan_copy(self):
        plan, cls, why = C.plan_ps(C.build_ir(assemble_copy()))
        self.assertEqual(cls, "tev_direct")
        self.assertIsNone(why)
        self.assertEqual(plan["stages"], 1)
        self.assertEqual(plan["textures"],
                         [{"texcoord_reg": 0, "sampler_reg": 0, "sampler_type": 2}])
        self.assertEqual(plan["constants"], [])
        stage = plan["stage_data"][0]
        self.assertEqual(stage["func_rgb"], C.FUNC_REPLACE)
        self.assertEqual(stage["func_a"], C.FUNC_REPLACE)
        self.assertEqual(stage["src_rgb"][0], C.SRC_TEX0)
        self.assertEqual(stage["src_a"][0], C.SRC_TEX0)

    def test_plan_tint(self):
        plan, cls, why = C.plan_ps(C.build_ir(assemble_tint()))
        self.assertEqual((cls, why), ("tev_direct", None))
        self.assertEqual(plan["stages"], 2)
        self.assertEqual(plan["uses_vertex_color"], 1)
        self.assertEqual(plan["inputs"], [{"usage": 10, "usage_index": 0,
                                           "reg_index": 1,
                                           "tev_source": C.SRC_PRIMARY}])
        self.assertEqual(plan["stage_data"][1]["func_rgb"], C.FUNC_MODULATE)
        self.assertEqual(plan["stage_data"][1]["src_rgb"][0], C.SRC_PREV)
        self.assertEqual(plan["stage_data"][1]["src_rgb"][1], C.SRC_PRIMARY)

    def test_plan_mad(self):
        plan, cls, why = C.plan_ps(C.build_ir(assemble_mad()))
        self.assertEqual((cls, why), ("tev_direct", None))
        self.assertEqual(plan["stages"], 2)
        self.assertEqual(plan["constants"], [2])
        stage = plan["stage_data"][1]
        self.assertEqual(stage["func_rgb"], C.FUNC_MULTIPLY_ADD)
        self.assertEqual(stage["const_reg"], 2)
        self.assertEqual(stage["src_rgb"][2], C.SRC_PRIMARY)

    def test_plan_clamp_folds_to_replace(self):
        plan, cls, why = C.plan_ps(C.build_ir(assemble_clamp()))
        self.assertEqual((cls, why), ("tev_direct", None))
        self.assertEqual(plan["stages"], 2)
        self.assertEqual(plan["stage_data"][1]["func_rgb"], C.FUNC_REPLACE)

    def test_refusals_are_named(self):
        cases = [
            (assemble_rcp(), "unsupported_opcode: rcp"),
            (assemble_dependent(), "dataflow: dependent texture read"),
            (assemble_out_of_range(), "def_literal_out_of_range: c0"),
            (assemble_seven_stages(), "too_many_stages"),
            (assemble_branch(), "unsupported_opcode: if"),
        ]
        for blob, expected in cases:
            plan, cls, why = C.plan_ps(C.build_ir(blob))
            self.assertIsNone(plan, expected)
            self.assertIsNone(cls, expected)
            self.assertIn(expected, why or "", expected)

    def test_dp4_is_prebake_not_software_success(self):
        plan, cls, why = C.plan_ps(C.build_ir(assemble_dp4()))
        self.assertIsNone(plan)
        self.assertEqual(cls, "tev_prebake")
        self.assertIn("requires_prebake", why)

    def test_plan_vs_descriptor(self):
        plan, cls, why = C.plan_vs(C.build_ir(assemble_vs()))
        self.assertEqual((cls, why), ("pica_vs", None))
        self.assertEqual(plan["instructions"], 2)
        self.assertEqual(plan["codegen"], "pending")
        self.assertEqual([(i["usage"], i["reg_index"]) for i in plan["inputs"]],
                         [(0, 0), (5, 1)])
        self.assertEqual(len(plan["outputs"]), 2)

    def test_deterministic_plan_and_table(self):
        blob = assemble_mad()
        first = C.plan_ps(C.build_ir(blob))[0]
        second = C.plan_ps(C.build_ir(blob))[0]
        self.assertEqual(json.dumps(first, sort_keys=True),
                         json.dumps(second, sort_keys=True))
        entry = dict(first, id="synthetic", note="synthetic", draw_enabled=0)
        table_a = C.emit_c_table([entry])
        table_b = C.emit_c_table([entry])
        self.assertEqual(table_a, table_b)
        self.assertNotIn("BSPS", table_a)
        self.assertNotIn("BSVS", table_a)


def _data_dir():
    try:
        return fxp_census.find_data()
    except SystemExit:
        return None


class RealArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = _data_dir()
        if not cls.data:
            raise unittest.SkipTest("Skyrim - Shaders.bsa not present")

    def test_menu_logo_compiles_to_certified_plan(self):
        pkgs = fxp_compile._load_packages(self.data)
        found = False
        for package in pkgs:
            for chunk in package.chunks:
                if chunk.kind != "ps":
                    continue
                ir = C.build_ir(chunk.code)
                if ir.sha256 != fxp_compile.LOGO_HASH:
                    continue
                found = True
                plan = C.menu_light_plan(ir)
                self.assertIsNotNone(plan)
                self.assertEqual(plan["stages"], 5)
                self.assertEqual(plan["constants"], [3, 4, 7])
                self.assertEqual(plan["textures"][1]["sampler_reg"], 1)
                self.assertEqual(plan["inputs"],
                                 [{"usage": 10, "usage_index": 0, "reg_index": 1,
                                   "tev_source": C.SRC_PRIMARY},
                                  {"usage": 10, "usage_index": 1, "reg_index": 2,
                                   "tev_source": C.SRC_FRAG_PRIMARY}])
                stages = plan["stage_data"]
                self.assertEqual((stages[0]["func_rgb"], stages[0]["func_a"]),
                                 (C.FUNC_DOT3_RGBA, C.FUNC_DOT3_RGBA))
                self.assertEqual(stages[0]["src_rgb"], [C.SRC_TEX1, C.SRC_CONST,
                                                        C.SRC_CONST])
                self.assertEqual(stages[0]["const_reg"], 3)
                self.assertEqual(stages[4]["op_rgb"], [C.RGB_COLOR, C.RGB_ALPHA,
                                                       C.RGB_COLOR])
                self.assertEqual(stages[4]["func_a"], C.FUNC_REPLACE)
                self.assertEqual(plan["preconditions"][:2],
                                 ["normal_prebaked", "tangent_basis_prebaked"])
        self.assertTrue(found, "certified logo hash absent from the archive")

    def test_menu_copy_compiles_to_one_stage(self):
        pkgs = fxp_compile._load_packages(self.data)
        found = False
        for package in pkgs:
            for chunk in package.chunks:
                if chunk.kind != "ps":
                    continue
                ir = C.build_ir(chunk.code)
                if ir.sha256 != fxp_compile.COPY_HASH:
                    continue
                found = True
                plan, cls, why = C.plan_ps(ir)
                self.assertEqual((cls, why), ("tev_direct", None))
                self.assertEqual(plan["stages"], 1)
                self.assertEqual(plan["stage_data"][0]["func_rgb"], C.FUNC_REPLACE)
                self.assertEqual(plan["stage_data"][0]["src_rgb"][0], C.SRC_TEX0)
                self.assertEqual(plan["textures"][0]["sampler_reg"], 0)
        self.assertTrue(found, "certified copy hash absent from the archive")

    def test_committed_table_matches_generator(self):
        manifest = fxp_compile.build_manifest(self.data)
        self.assertEqual(len(manifest["entries"]), 4)
        entries = manifest["entries"]
        self.assertEqual(entries[0]["id"], "menu.logo.ps")
        self.assertEqual(entries[1]["id"], "menu.composite.ps")
        self.assertEqual(entries[0]["sha256"], fxp_compile.LOGO_HASH)
        self.assertEqual(entries[1]["sha256"], fxp_compile.COPY_HASH)
        self.assertTrue(entries[2]["id"].startswith("game.ps."))
        self.assertTrue(entries[3]["id"].startswith("game.vs."))
        self.assertEqual(entries[2]["sha256"],
                         "f5239354171a6bc8166e1bfc59a0f46b90a3f0bc37b936f0b33ff6b91e9cc9ce")
        self.assertEqual(entries[3]["sha256"],
                         "0ccc7542aa06b2e20a41283acc2d5473602fc03f5495cc95166ec3b01820d2f7")
        self.assertEqual([e["draw_enabled"] for e in entries], [0, 1, 0, 0])
        generated = C.emit_c_table(entries)
        path = os.path.join(REPO, "source", "d3d9_shader_plan_table.inc")
        with open(path, "r", encoding="utf-8") as handle:
            committed = handle.read()
        self.assertEqual(generated, committed,
                         "source/d3d9_shader_plan_table.inc is stale; run "
                         "tools/fxp_compile.py manifest + emit-c")

    def test_census_menu_hashes_and_coverage(self):
        result = fxp_census.census(self.data)
        notable = result["notable"]
        self.assertIn("menu.logo.ps (p1200005)", notable)
        self.assertIn("menu.composite.ps (copy)", notable)
        self.assertEqual(notable["menu.logo.ps (p1200005)"]["class"], "tev_menu_light")
        self.assertEqual(notable["menu.composite.ps (copy)"]["class"], "tev_direct")
        coverage = result["coverage"]
        # Regression numbers measured 2026-11-26 over the 122-package base
        # archive.  A change here is a real coverage change, not noise.
        self.assertEqual(result["packages"], 122)
        self.assertEqual(result["chunks"], 10475)
        self.assertEqual(result["distinct_hashes"], 6659)
        self.assertEqual(result["ps"]["distinct_hashes"], 6350)
        self.assertEqual(result["vs"]["distinct_hashes"], 309)
        self.assertGreaterEqual(coverage["ps_tev_direct_hashes"], 7)
        self.assertGreaterEqual(coverage["ps_tev_direct_chunks"], 16)
        self.assertGreaterEqual(coverage["ps_tev_prebake_hashes"], 7)
        self.assertGreaterEqual(coverage["vs_pica_hashes"], 276)


if __name__ == "__main__":
    unittest.main(verbosity=2)
