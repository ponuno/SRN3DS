#!/usr/bin/env python3
"""Basic tint / membrane / facegen pixel-shader translation (pack-owned).

The shared TEV planner (``tools/shaders/compile.py:plan_ps``) only accepts a
strict straight-line single-temp dataflow and a literal ``def`` in RGBA8.  The
character PS corpus is a lit, multi-temp graph (``nrm``/``rcp``/``rsq``,
``cmp``, ``pow``, branches) that needs a scheduler, so it cannot be lowered
1:1 today.

This module lands the *basic* class the pack owns: shaders whose control flow
is straight-line (no ``pow``/``lrp``/``if``/``rep``/``texkill``/``abs``) and
that read at most three 2D textures.  The plan is the well-understood
base-albedo tint: each texture stage REPLACE/MODULATE, then one MODULATE by the
vertex colour.  Normal/specular/lighting maths is dropped, so the descriptor is
``quality: "approx"`` and is never draw-enabled.

``def`` components outside [0, 1] never reach the descriptor: the basic plan
uses no literal constants at all, so the RGBA8 literal limit does not apply.
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for _path in (REPO, os.path.join(REPO, "tools"),
              os.path.join(REPO, "tools", "shaders")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

from shaders import asm, compile as C  # noqa: E402

MAX_TEXTURES = 3
#: Straight-line arithmetic only.  Every opcode here is either consumed by the
#: basic albedo model or provably part of the lighting maths being dropped.
STRAIGHT_OPS = {
    0x00,  # nop
    0x01,  # mov
    0x02,  # add
    0x03,  # sub
    0x04,  # mad
    0x05,  # mul
    0x06,  # rcp
    0x07,  # rsq
    0x08,  # dp3
    0x09,  # dp4
    0x0A,  # min
    0x0B,  # max
    0x12,  # lrp
    0x1F,  # dcl
    0x20,  # pow
    0x23,  # abs
    0x24,  # nrm
    0x41,  # texkill (alpha test; dropped by the basic model)
    0x42,  # texld
    0x51,  # def
    0x58,  # cmp
}
DROPPED = ("normal/specular/lighting maths (nrm, rcp, rsq, pow, dp3, dp4, "
           "mad, mul, lrp, abs, min, max, cmp) and the texkill alpha test")


def _stage(func_rgb, func_a, src_rgb, src_a, op_rgb, op_a, texture=None):
    stage = {
        "func_rgb": func_rgb, "func_a": func_a,
        "src_rgb": list(src_rgb), "src_a": list(src_a),
        "op_rgb": list(op_rgb), "op_a": list(op_a),
        "scale_rgb": 0, "scale_a": 0, "const_reg": 255,
        "const_is_literal": 0, "const_rgba8": 0,
    }
    if texture is not None:
        stage["instruction"] = "texld"
        stage["word"] = texture
    return stage


def plan(ir: "C.IR"):
    """Return ``(descriptor, None)`` or ``(None, named_reason)``."""
    if ir.error:
        return None, "parse_error"
    if ir.kind != "ps" or ir.version != 0xFFFF0300:
        return None, "not_ps_3_0"
    if not ir.body:
        return None, "empty_program"
    ops = {ins.opcode for ins in ir.body}
    if ops - STRAIGHT_OPS:
        bad = sorted(ops - STRAIGHT_OPS)[0]
        return None, "ps_not_straight_line: %s" % asm.OPCODE_NAMES.get(
            bad, "0x%02x" % bad)
    texlds = [ins for ins in ir.body if ins.opcode == 0x42]
    if not texlds:
        return None, "ps_no_texture_read"
    if len(texlds) > MAX_TEXTURES:
        return None, "ps_texture_budget: %d > %d" % (len(texlds), MAX_TEXTURES)
    if any(ins.control for ins in texlds):
        return None, "ps_texld_control"
    coord_regs, sampler_regs = [], []
    for ins in texlds:
        if len(ins.srcs) < 2:
            return None, "ps_texld_without_sampler"
        coord, samp = ins.srcs[0], ins.srcs[1]
        if coord.reg_type != asm.REG_INPUT:
            return None, "ps_dependent_texture_read"
        if samp.reg_type != asm.REG_SAMPLER:
            return None, "ps_texld_without_sampler"
        if ir.samplers.get(samp.index) != 2:
            return None, "ps_non_2d_sampler"
        coord_regs.append(coord.index)
        sampler_regs.append(samp.index)
    colour = None
    for reg in sorted(ir.inputs):
        if ir.inputs[reg]["usage"] == 10:
            colour = reg
            break

    stages = []
    textures = []
    for unit, (coord, samp) in enumerate(zip(coord_regs, sampler_regs)):
        textures.append({"texcoord_reg": coord, "sampler_reg": samp,
                         "sampler_type": 2})
        src = C.SRC_TEX0 + unit
        if unit == 0:
            stages.append(_stage(C.FUNC_REPLACE, C.FUNC_REPLACE,
                                 [src, C.SRC_PREV, C.SRC_PREV],
                                 [src, C.SRC_PREV, C.SRC_PREV],
                                 [C.RGB_COLOR] * 3, [C.A_ALPHA] * 3,
                                 texlds[unit].offset))
        else:
            stages.append(_stage(C.FUNC_MODULATE, C.FUNC_MODULATE,
                                 [src, C.SRC_PREV, C.SRC_PREV],
                                 [src, C.SRC_PREV, C.SRC_PREV],
                                 [C.RGB_COLOR] * 3, [C.A_ALPHA] * 3,
                                 texlds[unit].offset))
    inputs = []
    uses_vertex_color = 0
    if colour is not None:
        tev_source = C.SRC_PRIMARY if colour == 1 else C.SRC_FRAG_PRIMARY
        inputs.append({"usage": 10,
                       "usage_index": ir.inputs[colour]["usage_index"],
                       "reg_index": colour, "tev_source": tev_source})
        uses_vertex_color = 1
        stages.append(_stage(C.FUNC_MODULATE, C.FUNC_MODULATE,
                             [C.SRC_PREV, tev_source, C.SRC_PREV],
                             [C.SRC_PREV, tev_source, C.SRC_PREV],
                             [C.RGB_COLOR] * 3, [C.A_ALPHA] * 3))

    plan = {
        "kind": "tev",
        "family": "skinning",
        "stage": "ps_3_0",
        "sha256": ir.sha256,
        "sha1": ir.sha1,
        "version": "0x%08x" % ir.version,
        "instructions": len(ir.body),
        "stages": len(stages),
        "stage_data": stages,
        "textures": textures,
        "inputs": inputs,
        "constants": [],
        "uses_vertex_color": uses_vertex_color,
        "requires_alpha_fixup": 0,
        "ldr_clamped": 1,
        "quality": "approx",
        "approximation": (
            "basic base-albedo tint: %d texture stage(s) modulated by the "
            "vertex colour; %s dropped; def literals outside RGBA8 [0,1] are "
            "not baked (no literal constants in this plan)"
            % (len(textures), DROPPED)),
        "preconditions": ["basic_albedo_tint_approximation",
                          "material_constants", "texture0_2d"],
    }
    if len(stages) > C.TEV_MAX_STAGES:
        return None, "ps_stage_budget"
    return plan, None


__all__ = ["plan", "MAX_TEXTURES", "STRAIGHT_OPS"]
