#!/usr/bin/env python3
"""Synthetic regressions for the three ``asm.py`` IR gaps from SM3 phase 1 §2.1.

* ``texld`` instruction control bits (``texldp``/``texldb``) survive into the IR
  together with the sampler/coordinate sources, and the planner refuses them by
  name instead of silently treating them as a plain ``texld``;
* a relative-addressed source keeps its full tuple (base register, index
  register, component, offset) and the relative token is not counted as a
  second source;
* branch/loop opcodes (``rep``/``endrep``/``else``/``endif``/``label``/``call``/
  ``ret``) have no destination in the IR.

The bytecode is built token by token, so the tests do not depend on retail data.
"""

from __future__ import annotations

import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import asm, compile as shader_compile, differential  # noqa: E402

PS_3_0 = 0xFFFF0300
END = 0x0000FFFF
XYZW = 0xE4


def _regtype_bits(reg_type: int) -> int:
    return ((reg_type & 0x7) << 28) | (((reg_type >> 3) & 0x3) << 11)


def _instr(opcode: int, operands: int, controls: int = 0) -> int:
    """One instruction token; ``operands`` is the number of following dwords."""
    return ((opcode & 0xFFFF) | ((controls & 0xFF) << 16)
            | ((operands & 0xF) << 24))


def _dst(reg_type: int, index: int, mask: int = 0xF, modifier: int = 0) -> int:
    return (index & 0x7FF) | ((mask & 0xF) << 16) | ((modifier & 0xF) << 20) \
        | _regtype_bits(reg_type)


def _src(reg_type: int, index: int, swizzle: int = XYZW, modifier: int = 0,
         relative: bool = False) -> int:
    return ((index & 0x7FF) | (1 << 31) | ((swizzle & 0xFF) << 16)
            | ((modifier & 0xF) << 24) | _regtype_bits(reg_type)
            | ((1 << 13) if relative else 0))


def _replicate(component: int) -> int:
    return component * 0x55


def _program(words) -> bytes:
    full = [PS_3_0] + list(words) + [END]
    return struct.pack("<%dI" % len(full), *full)


def _texld(control: int) -> bytes:
    return _program([
        _instr(0x42, 3, controls=control),
        _dst(asm.REG_TEMP, 0),
        _src(asm.REG_INPUT, 0),
        _src(asm.REG_SAMPLER, 0),
    ])


def _relative_program(component: int = 1) -> bytes:
    # mul r0, v2.y, c12[a0.<component>]
    return _program([
        _instr(0x05, 4),
        _dst(asm.REG_TEMP, 0),
        _src(asm.REG_INPUT, 2, _replicate(1)),
        _src(asm.REG_CONST, 12, relative=True),
        _src(asm.REG_ADDR, 0, _replicate(component)),
    ])


class TexldControlTest(unittest.TestCase):
    def test_projected_texld_keeps_control_and_sources(self):
        ins = asm.parse_program(_texld(asm.TEXLD_PROJECT)).instructions[0]
        self.assertEqual(ins.opcode, 0x42)
        self.assertEqual(ins.name, "texldp")
        self.assertEqual(ins.control, asm.TEXLD_PROJECT)
        self.assertTrue(ins.projected)
        self.assertFalse(ins.biased)
        self.assertEqual(len(ins.srcs), 2)
        self.assertEqual(ins.texture["coord"].index, 0)
        self.assertEqual(ins.texture["sampler"].index, 0)
        self.assertEqual(ins.texture["sampler"].reg_type, asm.REG_SAMPLER)
        self.assertEqual(ins.text(), "texldp r0 v0 s0")

    def test_biased_texld(self):
        ins = asm.parse_program(_texld(asm.TEXLD_BIAS)).instructions[0]
        self.assertEqual(ins.name, "texldb")
        self.assertEqual(ins.control, asm.TEXLD_BIAS)
        self.assertTrue(ins.biased)

    def test_plain_texld_has_zero_control(self):
        ins = asm.parse_program(_texld(0)).instructions[0]
        self.assertEqual(ins.name, "texld")
        self.assertEqual(ins.control, 0)
        self.assertFalse(ins.projected)

    def test_feature_vector_reports_projected_sampling(self):
        features = asm.feature_vector(asm.parse_program(_texld(asm.TEXLD_PROJECT)))
        self.assertEqual(features["projected_samples"], 1)
        self.assertEqual(features["biased_samples"], 0)
        self.assertEqual(features["texture_samples"], 1)

    def test_planner_sees_texldp_by_name(self):
        ir = shader_compile.build_ir(_texld(asm.TEXLD_PROJECT))
        _temps, _final, error = shader_compile.build_value_graph(ir)
        self.assertIsNotNone(error)
        self.assertIn("texldp", error)


class RelativeAddressingTest(unittest.TestCase):
    def test_full_tuple_and_no_phantom_source(self):
        ins = asm.parse_program(_relative_program()).instructions[0]
        self.assertEqual(len(ins.srcs), 2)
        self.assertEqual([s.reg_type for s in ins.srcs],
                         [asm.REG_INPUT, asm.REG_CONST])
        rel = ins.srcs[1].relative
        self.assertIsNotNone(rel)
        self.assertEqual((rel.base_type, rel.base_index), (asm.REG_CONST, 12))
        self.assertEqual((rel.index_type, rel.index_index),
                         (asm.REG_ADDR, 0))
        self.assertEqual((rel.component, rel.offset), ("y", 0))
        self.assertEqual(rel.as_dict(), {
            "base_type": asm.REG_CONST, "base_index": 12,
            "index_type": asm.REG_ADDR, "index_index": 0,
            "component": "y", "offset": 0,
        })
        self.assertEqual(ins.text(), "mul r0 v2.yyyy c12[a0.y]")

    def test_relative_token_is_not_a_source_or_a_constant(self):
        program = asm.parse_program(_relative_program())
        self.assertFalse(any(s.reg_type == asm.REG_ADDR
                             for ins in program.instructions for s in ins.srcs))
        features = asm.feature_vector(program)
        # Only the c12 base is a constant use; a0 is the index register.
        self.assertEqual(features["constants_used"], [12])

    def test_planner_refuses_relative_addressing(self):
        ir = shader_compile.build_ir(_relative_program())
        _temps, _final, error = shader_compile.build_value_graph(ir)
        self.assertIsNotNone(error)
        self.assertIn("relative addressing", error)


class NoDestinationTest(unittest.TestCase):
    def test_branch_and_loop_opcodes_have_no_destination(self):
        program = asm.parse_program(_program([
            _instr(0x26, 1), _src(asm.REG_CONSTINT, 0),   # rep i0
            _instr(0x2A, 0),                              # else
            _instr(0x2B, 0),                              # endif
            _instr(0x27, 0),                              # endrep
            _instr(0x1E, 1), _src(asm.REG_LABEL, 0),      # label l0
            _instr(0x19, 1), _src(asm.REG_LABEL, 0),      # call l0
            _instr(0x1C, 0),                              # ret
        ]))
        by_name = {}
        for ins in program.instructions:
            by_name.setdefault(ins.name, ins)
            self.assertIsNone(ins.dst, ins.name)
        self.assertEqual(by_name["rep"].srcs[0].reg_type, asm.REG_CONSTINT)
        self.assertEqual(by_name["rep"].srcs[0].index, 0)
        self.assertEqual(by_name["label"].srcs[0].reg_type, asm.REG_LABEL)
        self.assertEqual(by_name["call"].srcs[0].reg_type, asm.REG_LABEL)
        self.assertEqual(by_name["else"].srcs, ())
        self.assertEqual(by_name["endrep"].srcs, ())
        self.assertEqual(by_name["ret"].srcs, ())
        self.assertFalse(any(ins.dst is not None for ins in program.instructions))
        self.assertEqual(by_name["rep"].text(), "rep i0")

    def test_planner_sees_destinationless_instruction(self):
        code = _program([
            _instr(0x26, 1), _src(asm.REG_CONSTINT, 0),
            _instr(0x27, 0),
        ])
        ir = shader_compile.build_ir(code)
        _temps, _final, error = shader_compile.build_value_graph(ir)
        self.assertIsNotNone(error)
        self.assertIn("rep", error)
        self.assertIn("no destination", error)


class DifferentialFieldTest(unittest.TestCase):
    def test_texldp_control_is_compared(self):
        mj = differential.parse_mojoshader("texldp r0, v0, s0")
        self.assertEqual(differential.compare(
            mj, differential.parse_project(_texld(asm.TEXLD_PROJECT)))["status"],
            "equal")
        verdict = differential.compare(
            mj, differential.parse_project(_texld(0)))
        self.assertEqual(verdict["status"], "differ_texld_control")

    def test_relative_tuple_is_compared(self):
        mj = differential.parse_mojoshader("mul r0, v2.y, c12[a0.y]")
        self.assertEqual(differential.compare(
            mj, differential.parse_project(_relative_program(1)))["status"],
            "equal")
        verdict = differential.compare(
            mj, differential.parse_project(_relative_program(2)))
        self.assertEqual(verdict["status"], "differ_relative")

    def test_no_destination_shape_is_compared(self):
        mj = differential.parse_mojoshader("rep i0")
        self.assertEqual(differential.compare(
            mj, differential.parse_project(_program([
                _instr(0x26, 1), _src(asm.REG_CONSTINT, 0)])))["status"],
            "equal")


if __name__ == "__main__":
    unittest.main(verbosity=2)
