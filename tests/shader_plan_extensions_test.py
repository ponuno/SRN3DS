"""Schema/verifier/emitter coverage for constant_ops and passes[].

These are the two infrastructure escapes from the old plan limits: a derived
constant such as ``c0.zwzw`` and a multi-pass plan for more than six TEV
stages.  The test builds synthetic entries only; it never needs the retail
archive.
"""

from __future__ import annotations

import json
import os
import sys
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import compile as C  # noqa: E402
from shaders import plan_verify  # noqa: E402

SHA = "a" * 64


def stage(**overrides) -> dict:
    row = {
        "func_rgb": 0, "func_a": 0,
        "src_rgb": [3, 15, 15], "src_a": [3, 15, 15],
        "op_rgb": [0, 0, 0], "op_a": [0, 0, 0],
        "const_reg": 255, "const_is_literal": 0, "const_rgba8": 0,
        "scale_rgb": 0, "scale_a": 0,
    }
    row.update(overrides)
    return row


def entry(**overrides) -> dict:
    row = {
        "id": "unit.extensions.ps",
        "sha256": SHA,
        "stage": "ps_3_0",
        "kind": "tev",
        "draw_enabled": 1,
        "note": "unit plan",
        "stages": 1,
        "stage_data": [stage()],
        "textures": [],
        "constants": [0],
        "version": "0xffff0300",
        "instructions": 1,
        "family": "generic",
    }
    row.update(overrides)
    return row


class ExtensionsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = plan_verify.load_schema()

    def errors(self, row: dict) -> list:
        found: list = []
        plan_verify.validate_entry(row, found, "unit")
        return found

    def test_derived_c0_zwzw_is_accepted(self):
        row = entry(constant_ops=[{
            "dst_reg": 7,
            "op": "pack",
            "src": [{"reg": 0, "swizzle": "zwzw", "negate": 0}],
        }], constants=[0, 7])
        self.assertEqual(self.errors(row), [])

    def test_high_derived_slots_and_dst_alias(self):
        # GFx packs use derived c9..c16; earlier plans spell the destination
        # "dst" instead of "dst_reg".
        row = entry(constant_ops=[{
            "dst": 9,
            "op": "pack",
            "src": [{"reg": 0, "swizzle": "zwzw"}],
        }], constants=[0, 1, 9], stages=3,
            stage_data=[stage(const_reg=0), stage(const_reg=1),
                        stage(const_reg=9)])
        self.assertEqual(self.errors(row), [])
        row = entry(constant_ops=[{
            "dst_reg": 16,
            "op": "pack",
            "src": [{"reg": 0, "swizzle": "xyzw"}],
        }], constants=[0, 16])
        self.assertEqual(self.errors(row), [])
        too_high = entry(constant_ops=[{
            "dst_reg": 32, "op": "pack",
            "src": [{"reg": 0, "swizzle": "xyzw"}],
        }], constants=[0])
        self.assertTrue(any("0..31" in e for e in self.errors(too_high)))

    def test_derived_ops_reject_bad_swizzle_and_source(self):
        bad_swizzle = entry(constant_ops=[{
            "dst_reg": 7, "op": "pack",
            "src": [{"reg": 0, "swizzle": "xyza"}],
        }], constants=[0, 7])
        self.assertTrue(any("swizzle" in e for e in self.errors(bad_swizzle)))
        bad_source = entry(constant_ops=[{
            "dst_reg": 7, "op": "pack",
            "src": [{"reg": 6, "swizzle": "xyzw"}],
        }], constants=[0, 7])
        self.assertTrue(any("provides" in e for e in self.errors(bad_source)))
        wrong_arity = entry(constant_ops=[{
            "dst_reg": 7, "op": "add",
            "src": [{"reg": 0, "swizzle": "xyzw"}],
        }], constants=[0, 7])
        self.assertTrue(any("needs 2 source" in e for e in self.errors(wrong_arity)))

    def test_seven_stage_two_pass_plan(self):
        pass0 = {
            "stage_data": [stage() for _ in range(6)],
            "textures": [], "constants": [0],
            "blend": {"enable": 0, "src": 2, "dst": 1, "op": 1},
            "depth_write": 0,
        }
        pass1 = {
            "stage_data": [stage(func_rgb=1)],
            "textures": [], "constants": [0],
            "blend": {"enable": 1, "src": 2, "dst": 1, "op": 1},
            "depth_write": 1,
        }
        row = entry(**{
            "stages": 6,
            "stage_data": pass0["stage_data"],
            "passes": [pass0, pass1],
        })
        self.assertEqual(self.errors(row), [])

    def test_pass_limits_are_named(self):
        pass0 = {
            "stage_data": [stage()], "textures": [], "constants": [0],
            "blend": {"enable": 0, "src": 2, "dst": 1, "op": 1},
            "depth_write": 0,
        }
        seven = dict(pass0, stage_data=[stage() for _ in range(7)])
        row = entry(passes=[pass0, seven])
        self.assertTrue(any("pass 1 stage_data" in e for e in self.errors(row)))
        bad_blend = dict(pass0, blend={"enable": 1, "src": 0, "dst": 1, "op": 1})
        row = entry(passes=[pass0, bad_blend])
        self.assertTrue(any("blend.src" in e for e in self.errors(row)))
        row = entry(passes=[pass0])
        self.assertTrue(any("2..4" in e for e in self.errors(row)))

    def test_emit_c_contains_derived_and_pass_fields(self):
        row = entry(constant_ops=[{
            "dst_reg": 7, "op": "pack",
            "src": [{"reg": 0, "swizzle": "zwzw", "negate": 0}],
        }], constants=[0, 7])
        text = C.emit_c_table([row])
        self.assertIn("{7, 0, 1, {{0,{2,3,2,3},0}}}", text)

        pass0 = {
            "stage_data": [stage()], "textures": [], "constants": [0],
            "blend": {"enable": 0, "src": 2, "dst": 1, "op": 1},
            "depth_write": 0,
        }
        pass1 = dict(pass0, blend={"enable": 1, "src": 1, "dst": 0, "op": 1})
        row = entry(passes=[pass0, pass1])
        text = C.emit_c_table([row])
        self.assertIn("    2,\n", text)
        self.assertEqual(text, C.emit_c_table([row]))

    def test_legacy_entry_emits_unchanged_table(self):
        text = C.emit_c_table([entry()])
        # No new trailing fields for old plans: the initializer closes right
        # after the TEV descriptor.
        self.assertNotIn("constant_ops", text)
        self.assertNotIn("pass_count", text)
        self.assertTrue(text.rstrip().endswith("#define D9_SHADER_PLAN_ENTRY_COUNT "
                              "(sizeof(d9_shader_plan_entries)/sizeof(d9_shader_plan_entries[0]))"))


if __name__ == "__main__":
    unittest.main()
