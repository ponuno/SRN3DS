#!/usr/bin/env python3
"""Host tests for the straight-line PICA VS emitter and its hash table."""

from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import compile as C  # noqa: E402
import vs_emit  # noqa: E402

VS_VERSION = 0xFFFE0300
END = 0x0000FFFF
ID = 0xE4


def dst(type_, reg, mask=0xF):
    return (0x80000000 | ((type_ & 7) << 28) | ((type_ & 0x18) << 8)
            | (reg & 0x7FF) | ((mask & 0xF) << 16))


def src(type_, reg, sw=ID, mod=0):
    return (0x80000000 | ((type_ & 7) << 28) | ((type_ & 0x18) << 8)
            | (reg & 0x7FF) | ((sw & 0xFF) << 16) | ((mod & 0xF) << 24))


def op(code, operands):
    return code | (operands << 24)


def dcl(usage, index, reg):
    return [op(0x1F, 2), usage | (index << 16), dst(1, reg)]


def dcl_out(usage, index, reg):
    return [op(0x1F, 2), usage | (index << 16), dst(6, reg)]


def program(*parts):
    words = [VS_VERSION]
    for part in parts:
        words.extend(part)
    words.append(END)
    return struct.pack("<%dI" % len(words), *words)


def plan_for(code, **overrides):
    ir = C.build_ir(code)
    plan, cls, why = C.plan_vs(ir)
    assert plan is not None, why
    plan["id"] = "game.vs.%s" % plan["sha256"][:8]
    plan.update(overrides)
    return plan, ir


class EmitterTest(unittest.TestCase):
    def test_clip_transform_emits_picasso(self):
        code = program(
            dcl(0, 0, 0), dcl(5, 0, 1), dcl_out(0, 0, 0), dcl_out(5, 0, 1),
            [op(0x09, 3), dst(0, 0, 1), src(2, 0), src(1, 0)],
            [op(0x09, 3), dst(0, 0, 2), src(2, 1), src(1, 0)],
            [op(0x09, 3), dst(0, 0, 4), src(2, 2), src(1, 0)],
            [op(0x09, 3), dst(0, 0, 8), src(2, 3), src(1, 0)],
            [op(0x01, 2), dst(6, 0), src(0, 0)],
            [op(0x01, 2), dst(6, 1), src(1, 1)],
        )
        plan, ir = plan_for(code)
        text = vs_emit.emit_plan(plan, ir)
        self.assertIn(".fvec cu[4]", text)
        self.assertIn(".out outpos position", text)
        self.assertIn(".out outtc0 texcoord0", text)
        self.assertIn("dp4 r0.x, cu[0], v0", text)
        self.assertIn("mov outpos, r0", text)
        self.assertIn("mov outtc0, v1", text)
        self.assertNotIn("\n    texld", text)
        self.assertNotIn("\n    loop", text)

    def test_menu_generic_colour_pair(self):
        code = program(
            dcl(0, 0, 0), dcl(5, 0, 1), dcl(10, 0, 2),
            dcl_out(0, 0, 0), dcl_out(5, 0, 1), dcl_out(10, 0, 2),
            dcl_out(10, 1, 3),
            [op(0x01, 2), dst(6, 0), src(1, 0)],
            [op(0x01, 2), dst(6, 1), src(1, 1)],
            [op(0x01, 2), dst(6, 2), src(1, 2)],
            [op(0x01, 2), dst(6, 3), src(1, 2)],
        )
        plan, ir = plan_for(code)
        text = vs_emit.emit_plan(plan, ir)
        self.assertIn(".out outclr color", text)
        self.assertIn(".out outclr1 texcoord2", text)

    def test_named_refusals(self):
        code = program(dcl(0, 0, 0), dcl_out(0, 0, 0),
                       [op(0x09, 3), dst(0, 0), src(2, 0), src(1, 0)])
        plan, ir = plan_for(code)
        bad = dict(plan)
        bad["skinning"] = True
        with self.assertRaisesRegex(vs_emit.Refusal, "skinning_unsupported"):
            vs_emit.check_plan(bad)
        bad = dict(plan)
        bad["outputs"] = list(plan["outputs"]) + [{
            "reg_index": 2, "type": 6, "usage": 3, "usage_index": 0,
            "usage_name": "normal"}]
        with self.assertRaisesRegex(vs_emit.Refusal, "output_usage_normal"):
            vs_emit.check_plan(bad)
        bad = dict(plan)
        bad["expandable"] = ["lrp"]
        with self.assertRaisesRegex(vs_emit.Refusal, "expandable_opcode_lrp"):
            vs_emit.check_plan(bad)

    def test_unsupported_opcode_is_named(self):
        # The descriptor classifier accepts rcp, but this minimal emitter does
        # not lower it; the refusal must name the opcode.
        code = program(dcl(0, 0, 0), dcl_out(0, 0, 0),
                       [op(0x06, 2), dst(0, 0), src(2, 0)])
        plan, ir = plan_for(code)
        with self.assertRaisesRegex(vs_emit.Refusal,
                                    "unsupported_opcode_rcp"):
            vs_emit.emit_plan(plan, ir)

    def test_table_matches_committed_include(self):
        manifest = os.path.join(REPO, "docs", "generated_vs.json")
        text = vs_emit.emit_c_table(manifest)
        path = os.path.join(REPO, "source", "d3d9_generated_vs_table.inc")
        with open(path, "r", encoding="utf-8") as handle:
            committed = handle.read()
        self.assertEqual(text, committed,
                         "source/d3d9_generated_vs_table.inc is stale")
        with open(manifest, "r", encoding="utf-8") as handle:
            entries = json.load(handle)["entries"]
        self.assertGreaterEqual(len(entries), 3)
        for entry in entries:
            self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(entry["id"].startswith("game.vs."))
            self.assertTrue(entry["asm"].endswith(".v.pica"))

    def test_append_only_conflict(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "table.json")
            entry = {"id": "game.vs.aaaa", "sha256": "a" * 64,
                     "asm": "a.v.pica"}
            vs_emit._merge_generated(path, entry)
            vs_emit._merge_generated(path, entry)
            with self.assertRaisesRegex(vs_emit.Refusal, "append_only_conflict"):
                vs_emit._merge_generated(path, dict(entry, asm="b.v.pica"))


if __name__ == "__main__":
    unittest.main()
