#!/usr/bin/env python3
"""Synthetic + catalog tests for the TEV stage scheduler.

The synthetic programs are assembled here from semantic fields; no retail
bytes are needed.  The catalog checks read the committed scheduler catalog and
coverage report.
"""

from __future__ import annotations

import json
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
for path in (REPO, os.path.join(REPO, "tools")):
    if path not in sys.path:
        sys.path.insert(0, path)

from shaders import asm, compile as C, tev_sched as S  # noqa: E402

PS_VERSION = 0xFFFF0300
END = 0x0000FFFF
IDENTITY = 0xE4
SW_X, SW_Y, SW_Z, SW_W = 0x00, 0x55, 0xAA, 0xFF


def _dst(type_, reg, mask=0xF, mod=0):
    return (0x80000000 | ((type_ & 7) << 28) | ((type_ & 0x18) << 8)
            | (reg & 0x7FF) | ((mask & 0xF) << 16) | ((mod & 0xF) << 20))


def _src(type_, reg, sw=IDENTITY, mod=0):
    return (0x80000000 | ((type_ & 7) << 28) | ((type_ & 0x18) << 8)
            | (reg & 0x7FF) | ((sw & 0xFF) << 16) | ((mod & 0xF) << 24))


def _op(opcode, operands, control=0):
    return opcode | (operands << 24) | ((control & 0xFF) << 16)


def _fbits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def _dcl_input(usage, usage_index, reg):
    return [_op(0x1F, 2), (usage | (usage_index << 16)), _dst(1, reg)]


def _dcl_sampler(reg, tex_type=2):
    return [_op(0x1F, 2), tex_type << 27, _dst(10, reg)]


def _def(reg, values):
    return [_op(0x51, 5), _dst(2, reg)] + [_fbits(v) for v in values]


def _defi(reg, value):
    return [_op(0x30, 5), _dst(7, reg), value, 0, 0, 0]


def _texld(dst_reg, coord_reg, sampler, out=False, mask=0xF):
    return [_op(0x42, 3), _dst(8 if out else 0, dst_reg, mask),
            _src(1, coord_reg), _src(10, sampler)]


def _alu(opcode, dst_tok, *srcs):
    return [_op(opcode, 1 + len(srcs)), dst_tok] + list(srcs)


def _program(*parts):
    words = [PS_VERSION]
    for part in parts:
        words.extend(part)
    words.append(END)
    return struct.pack("<%dI" % len(words), *words)


def _base():
    return [_dcl_input(5, 0, 0), _dcl_input(10, 0, 1),
            _dcl_sampler(0), _dcl_sampler(1), _texld(0, 0, 0),
            _texld(1, 0, 1)]


def _plan(blob, **kw):
    ir = C.build_ir(blob)
    return S.plan_shader(ir, family="generic", **kw)


class SchedulerSyntheticTest(unittest.TestCase):
    def test_exact_copy_and_tint(self):
        copy = _program(_dcl_input(5, 0, 0), _dcl_sampler(0),
                        _texld(0, 0, 0, out=True))
        p = _plan(copy)
        self.assertEqual(p["quality"], "exact")
        self.assertEqual(p["stages"], 1)
        self.assertEqual(p["stage_data"][0]["src_rgb"][0], C.SRC_TEX0)
        tint = _program(*_base(),
                        _alu(0x05, _dst(8, 0), _src(0, 0), _src(1, 1)))
        p = _plan(tint)
        self.assertEqual(p["quality"], "exact")
        self.assertEqual(p["stages"], 1)
        self.assertLess(p["verification"]["max_abs_error"], 1e-6)

    def test_exact_long_chain_splits_into_passes(self):
        parts = [_dcl_input(5, 0, 0), _dcl_sampler(0), _texld(0, 0, 0)]
        for i in range(1, 8):
            parts.append(_alu(0x05, _dst(0, i), _src(0, i - 1), _src(0, 0)))
        parts.append(_alu(0x05, _dst(8, 0), _src(0, 7), _src(0, 0)))
        p = _plan(_program(*parts))
        self.assertEqual(p["quality"], "exact")
        self.assertGreaterEqual(p.get("pass_count", 1), 2)
        self.assertLessEqual(p["stages"], 6)
        for entry in p["passes"]:
            self.assertLessEqual(len(entry["stage_data"]), 6)
            self.assertIn("enable", entry["blend"])
            self.assertIn(entry["depth_write"], (0, 1))

    def test_constant_ifc_is_folded(self):
        blob = _program(
            _dcl_input(5, 0, 0), _dcl_sampler(0), _dcl_sampler(1),
            _def(0, (0.0, 0.0, 0.0, 0.0)),
            _def(1, (1.0, 1.0, 1.0, 1.0)),
            [_op(0x29, 2, 4), _src(2, 0), _src(2, 1)],  # ifc_lt c0, c1
            _texld(0, 0, 0),
            _alu(0x01, _dst(8, 0), _src(0, 0)),
            [_op(0x2A, 0)],
            _texld(0, 0, 1),
            _alu(0x01, _dst(8, 0), _src(0, 0)),
            [_op(0x2B, 0)],
        )
        p = _plan(blob)
        self.assertEqual(p["quality"], "exact")
        self.assertEqual(p["approximation"] if "approximation" in p else None,
                         None)
        # The then arm sampled s0; the else arm sampled s1.
        self.assertEqual(p["textures"][0]["sampler_reg"], 0)

    def test_variable_ifc_is_branchless_select(self):
        blob = _program(
            _dcl_input(5, 0, 0), _dcl_input(10, 0, 1),
            _dcl_sampler(0), _dcl_sampler(1), _texld(0, 0, 0),
            [_op(0x29, 2, 4), _src(1, 1), _src(2, 0)],  # ifc_lt v1, c0
            _texld(0, 0, 1),
            _alu(0x01, _dst(8, 0), _src(0, 0)),
            [_op(0x2A, 0)],
            _alu(0x01, _dst(8, 0), _src(0, 0)),
            [_op(0x2B, 0)],
        )
        p = _plan(blob)
        self.assertEqual(p["quality"], "approx")
        self.assertIn("branchless_select_then_arm", p["preconditions"])
        self.assertEqual(p["approximation"]["control_flow"]["mode"],
                         "branchless_select")

    def test_small_loop_unrolls_and_large_loop_truncates(self):
        def loop_program(count):
            return _program(
                _dcl_input(5, 0, 0), _dcl_sampler(0), _defi(0, count),
                _def(1, (0.5, 0.5, 0.5, 0.5)), _texld(0, 0, 0),
                [_op(0x26, 1), _src(7, 0)],
                _alu(0x05, _dst(0, 1), _src(0, 1), _src(2, 1)),
                [_op(0x27, 0)],
                _alu(0x01, _dst(8, 0), _src(0, 1)),
            )
        small = _plan(loop_program(3))
        self.assertIn("loop_truncated_to", json.dumps(small))
        self.assertEqual(small["approximation"]["control_flow"]["loops"][0]
                         ["iterations"], 3)
        large = _plan(loop_program(16))
        self.assertEqual(large["approximation"]["control_flow"]["loops"][0]
                         ["loop_truncated_to"], 2)

    def test_op_library_emits_named_approximations(self):
        base = _base()
        cases = {
            "rcp": _alu(0x06, _dst(8, 0), _src(0, 0)),
            "rsq": _alu(0x07, _dst(8, 0), _src(0, 0)),
            "nrm": _alu(0x24, _dst(8, 0), _src(0, 0)),
            "abs": _alu(0x23, _dst(8, 0), _src(0, 0)),
            "frc": _alu(0x13, _dst(8, 0), _src(0, 0)),
            "cmp": _alu(0x58, _dst(8, 0), _src(0, 0), _src(1, 1),
                        _src(0, 0)),
            "slt": _alu(0x0C, _dst(8, 0), _src(0, 0), _src(1, 1)),
            "sge": _alu(0x0D, _dst(8, 0), _src(0, 0), _src(1, 1)),
        }
        for name, ins in cases.items():
            p = _plan(_program(*base, ins))
            self.assertIsNotNone(p, name)
            self.assertEqual(p["quality"], "approx", name)
            self.assertTrue(p["approximation"]["lost_terms"], name)
            self.assertGreaterEqual(p["approx_error"]["samples"], 8, name)

    def test_pow_constant_exponent_and_lrp(self):
        p = _plan(_program(*_base(), _def(2, (2.0, 2.0, 2.0, 2.0)),
                           _alu(0x20, _dst(8, 0), _src(0, 0), _src(2, 2))))
        self.assertEqual(p["quality"], "approx")
        self.assertIn("pow_expanded_to_2_muls",
                      " ".join(p["approximation"]["lost_terms"]))
        p = _plan(_program(*_base(),
                           _alu(0x12, _dst(8, 0), _src(0, 0), _src(0, 0),
                                _src(1, 1))))
        self.assertEqual(p["quality"], "exact")
        self.assertEqual(p["stage_data"][0]["func_rgb"], C.FUNC_INTERPOLATE)

    def test_schema_valid_for_every_quality(self):
        from shaders import plan_verify
        for blob in (
            _program(_dcl_input(5, 0, 0), _dcl_sampler(0),
                     _texld(0, 0, 0, out=True)),
            _program(*_base(), _alu(0x06, _dst(8, 0), _src(0, 0))),
        ):
            p = _plan(blob)
            errors = []
            plan_verify.validate_entry(p, errors, p["id"])
            self.assertEqual(errors, [], p["id"])


class CatalogCoverageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(os.path.join(REPO, "docs", "scheduler_plans.json"),
                  encoding="utf-8") as handle:
            cls.catalog = json.load(handle)
        with open(os.path.join(REPO, "docs", "shader_coverage.json"),
                  encoding="utf-8") as handle:
            cls.coverage = json.load(handle)

    def test_catalog_entries_are_translated_and_unique(self):
        entries = self.catalog["entries"]
        self.assertGreaterEqual(len(entries), 5000)
        self.assertEqual(len({e["sha256"] for e in entries}), len(entries))
        self.assertEqual(len({e["id"] for e in entries}), len(entries))
        for e in entries[:200]:
            self.assertEqual(e["draw_enabled"], 0)
            self.assertEqual(e["runtime"], 0)
            self.assertLessEqual(e["stages"], 6)

    def test_ninety_percent_of_refusals_now_have_a_plan(self):
        total = self.coverage["total"]
        covered = total["translated"] + total["draw-enabled"]
        self.assertGreaterEqual(covered / (covered + total["refused"]), 0.90)
        # Every remaining refusal is a vertex-stage PICA limit, not a PS TEV
        # stage that the scheduler failed to schedule.
        for pack in self.coverage["packs"]:
            if pack["refused"]:
                self.assertTrue(all(cls in ("pica_vs",)
                                    or cls.startswith("too_many_vs_varyings")
                                    for cls in pack["refused_classes"]),
                                pack["pack"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
