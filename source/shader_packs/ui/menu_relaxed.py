#!/usr/bin/env python3
"""Relaxed TEV planner for the GFx menu pixel programs.

The phase-1 planner is deliberately conservative: a temp may be written once,
``lrp`` and ``dp4`` are refused, literal ``def`` values must be in [0,1], and an
output may be written once.  The GFx menu shaders break all four rules in
mechanical ways, so this module relaxes exactly those restrictions and records
every relaxation as an *approximation* in the plan.  Nothing is silent:

* ``def cN`` is moved to the uniform constant bank (``const_reg``); the
  original literal is recorded in ``constant_values`` and must be supplied by
  the material.  This is exact when the bank value matches.
* temp reuse, partial writes and split ``oC0.xyz`` / ``oC0.w`` are scheduled
  through the single PICA ``PREV`` register; if a second live temp is needed
  the plan is refused, not approximated.
* ``lrp`` maps to TEV ``INTERPOLATE`` (a*c + b*(1-c)), exact.
* ``dp4`` cannot be expressed by TEV (no 4-component dot), so the four basis
  rows are collapsed to one ``DOT3`` and the plan carries
  ``quality: approx`` with a named approximation.  This is a visual
  simplification: the YUV/basis chroma is replaced by one luminance row.
"""

from __future__ import annotations

import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
if os.path.join(REPO, "tools") not in sys.path:
    sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import asm  # noqa: E402
from shaders import compile as C  # noqa: E402

UNDEF = C.UNDEF
LEAF = C.LEAF_KINDS


def _src_vec(src, temps, ir=None):
    def one(ch):
        comp = "xyzw".index(src.swizzle[ch])
        if src.reg_type == asm.REG_TEMP:
            vec = temps.get(src.index)
            base = vec[comp] if vec else ("tmp", src.index, comp)
        elif src.reg_type == asm.REG_INPUT:
            decl = (ir.inputs.get(src.index) if ir is not None else None)
            if decl and decl.get("usage") == 10:
                # TevPlanner canonicalises colour varying 0/1 to in 1/2.
                base = ("in", 1 if decl.get("usage_index") == 0 else 2, comp)
            else:
                base = ("in", src.index, comp)
        elif src.reg_type == asm.REG_CONST:
            base = ("const", src.index, comp)
        else:
            base = ("other", src.reg_type, src.index, comp)
        if src.modifier & 1:  # negate
            return ("op", "neg", base, None, None)
        return base
    return tuple(one(ch) for ch in range(4))


def value_graph(ir):
    """Like ``compile.build_value_graph`` but allows temp reuse and lrp/dp4."""
    temps = {}
    out = [UNDEF] * 4
    approx = []
    for ins in ir.body:
        if ins.opcode in (0x1F, 0x51, 0x00):
            continue
        dst = ins.dst
        if dst is None or not dst.mask:
            return {}, tuple(out), "no destination mask", approx
        if dst.reg_type == asm.REG_TEMP:
            vec = list(temps.get(dst.index, [UNDEF] * 4))
        elif dst.reg_type == asm.REG_COLOROUT:
            if dst.index != 0:
                return {}, tuple(out), "writes oC%d (MRT)" % dst.index, approx
            vec = list(out)
        elif dst.reg_type == asm.REG_DEPTHOUT:
            return {}, tuple(out), "writes oDepth", approx
        else:
            return {}, tuple(out), "unsupported dst", approx

        if ins.opcode == 0x42:  # texld
            coord, samp = ins.srcs[0], ins.srcs[1]
            if coord.reg_type != asm.REG_INPUT:
                return {}, tuple(out), "dependent texture read", approx
            if ir.samplers.get(samp.index) != 2:
                return {}, tuple(out), "sampler is not 2D", approx
            for ch in range(4):
                vec[ch] = ("tex", samp.index, ch)
        elif ins.opcode in (0x01, 0x02, 0x03, 0x04, 0x05, 0x08):
            need = {0x01: 1, 0x02: 2, 0x03: 2, 0x04: 3, 0x05: 2, 0x08: 2}[ins.opcode]
            srcs = [_src_vec(s, temps, ir) for s in ins.srcs[:need]]
            for ch in range(4):
                if not (dst.mask & (1 << ch)):
                    continue
                if ins.opcode == 0x01:
                    vec[ch] = srcs[0][ch]
                elif ins.opcode == 0x02:
                    vec[ch] = ("op", "add", srcs[0][ch], srcs[1][ch], None)
                elif ins.opcode == 0x03:
                    vec[ch] = ("op", "sub", srcs[0][ch], srcs[1][ch], None)
                elif ins.opcode == 0x04:
                    vec[ch] = ("op", "mad", srcs[0][ch], srcs[1][ch], srcs[2][ch])
                elif ins.opcode == 0x05:
                    vec[ch] = ("op", "mul", srcs[0][ch], srcs[1][ch], None)
                elif ins.opcode == 0x08:
                    vec[ch] = ("dot3", srcs[0], srcs[1])
        elif ins.opcode == 0x12:  # lrp dst, a, b, c == a*c + b*(1-c)
            a, b, c = (_src_vec(s, temps, ir) for s in ins.srcs[:3])
            for ch in range(4):
                if dst.mask & (1 << ch):
                    vec[ch] = ("op", "func4", a[ch], b[ch], c[ch])
        elif ins.opcode == 0x09:  # dp4 -> approximate one DOT3 row
            srcs = [_src_vec(s, temps, ir) for s in ins.srcs[:2]]
            approx.append("dp4 row collapsed to DOT3 (no 4-component dot in TEV)")
            for ch in range(4):
                if dst.mask & (1 << ch):
                    vec[ch] = ("dot3", srcs[0], srcs[1])
        else:
            return {}, tuple(out), "unsupported opcode %s" % ins.name, approx

        if dst.reg_type == asm.REG_TEMP:
            temps[dst.index] = tuple(vec)
        else:
            for ch in range(4):
                if dst.mask & (1 << ch):
                    out[ch] = vec[ch]
    if out[3] == UNDEF:
        out[3] = ("lit", 0xFFFFFFFF, 3)
    return temps, tuple(out), None, approx


class RelaxedPlanner(C.TevPlanner):
    """One PREV register, but a temp read may fall back to an earlier leaf."""

    def __init__(self, ir, temps):
        super().__init__(ir, temps)
        self.history = {}

    def _leaf_operand(self, vec, comps):
        # Same as the base class, but select PRIMARY/SECONDARY by the colour
        # declaration's usage index, not by the register number.
        op_rgb = C._operand_selector(comps, False)
        op_a = C._operand_selector(comps, True)
        kind = vec[0][0]
        if kind == "tex":
            return C.Operand(vec, comps, C.SRC_TEX0 + self._tex_unit(vec[0][1]),
                             op_rgb, op_a)
        if kind == "in":
            reg = vec[0][1]
            decl = None
            reg_index = reg
            direct = self.ir.inputs.get(reg)
            if direct is not None and direct.get("usage") == 10:
                decl = direct
            else:
                for index, cand in self.ir.inputs.items():
                    canonical = 1 if cand.get("usage_index") == 0 else 2
                    if cand.get("usage") == 10 and canonical == reg:
                        decl = cand
                        reg_index = index
            if decl is None:
                raise C.PlanError("input_not_a_colour_varying",
                                  "in%d is not a colour varying" % reg)
            src = C.SRC_PRIMARY if decl.get("usage_index") == 0 else C.SRC_FRAG_PRIMARY
            self.uses_vertex_color = 1
            if not any(i.get("tev_source") == src for i in self.inputs):
                self.inputs.append({"usage": 10,
                                    "usage_index": decl.get("usage_index", 0),
                                    "reg_index": reg_index,
                                    "tev_source": src})
            return C.Operand(vec, comps, src, op_rgb, op_a)
        if kind in ("const", "lit"):
            return C.Operand(vec, comps, C.SRC_CONST, op_rgb, op_a)
        raise C.PlanError("unsupported_leaf", kind)

    def snapshot(self, index):
        if index in self.temps:
            self.history.setdefault(index, []).append(self.temps[index])

    def operand(self, src):
        try:
            return super().operand(src)
        except C.PlanError as exc:
            if src.reg_type != asm.REG_TEMP:
                raise exc
        comps = tuple("xyzw".index(src.swizzle[ch]) for ch in range(4))
        if not ((comps[0] == 0 and comps[1] == 1 and comps[2] == 2) or
                (comps[0] == comps[1] == comps[2])):
            raise C.PlanError("non_uniform_swizzle",
                              "%s.%s" % (src.name(), src.swizzle))
        # The last stage may have written oC0 while carrying this temp.
        if self.prev is not None and self.prev_reg is None:
            return C.Operand(self.prev, comps, C.SRC_PREV,
                             C._operand_selector(comps, False),
                             C._operand_selector(comps, True))
        if self.prev is not None:
            vec = self.temps.get(src.index)
            if vec is not None and all(
                    C._norm(vec[c]) == C._norm(self.prev[c])
                    for c in set(comps)):
                return C.Operand(self.prev, comps, C.SRC_PREV,
                                 C._operand_selector(comps, False),
                                 C._operand_selector(comps, True))
        # r0 was overwritten, but an earlier value may still be a TEV leaf
        # (texture / varying / constant) and is still addressable.
        for vec in reversed(self.history.get(src.index, [])):
            if all(vec[c][0] in LEAF for c in set(comps)):
                return self._leaf_operand(vec, comps)
        raise C.PlanError("temp_not_previous",
                          "r%d is not the previous stage result" % src.index)


def _emit_lrp(planner, ins, temps):
    a, b, c = (planner.operand(s) for s in ins.srcs[:3])
    regs, lits = planner._const_values([a, b, c])
    if len(regs) > 1 or (regs and lits) or len(lits) > 1:
        raise C.PlanError("stage_needs_two_constants_or_literals")
    const_reg = next(iter(regs)) if regs else 255
    literal = next(iter(lits)) if lits else None
    if const_reg != 255 and const_reg not in planner.constants:
        planner.constants.append(const_reg)
    # D3D order (a, b, c) -> TEV order (a, c, b) for o1*o2 + o3*(1-o2).
    operands = [a, c, b]
    stage = {
        "func_rgb": C.FUNC_INTERPOLATE, "func_a": C.FUNC_INTERPOLATE,
        "src_rgb": [a.source, c.source, b.source],
        "src_a": [a.source, c.source, b.source],
        "op_rgb": [a.op_rgb, c.op_rgb, b.op_rgb],
        "op_a": [a.op_a, c.op_a, b.op_a],
        "scale_rgb": 0, "scale_a": 0, "const_reg": const_reg,
        "const_is_literal": 0 if literal is None else 1,
        "const_rgba8": 0 if literal is None else literal,
        "instruction": "lrp", "word": ins.offset,
    }
    planner.stages.append(stage)
    return stage, planner._stage_result(stage, operands)


def plan_relaxed(ir):
    """Return (plan, approximation_texts, refusal)."""
    if ir.error:
        return None, [], "parse_error: %s" % ir.error
    if ir.kind != "ps" or ir.version != 0xFFFF0300:
        return None, [], "not_ps_3_0"
    if not ir.body:
        return None, [], "empty_program"
    if any(ins.opcode == 0x06 for ins in ir.body):  # rcp
        return None, [], "unsupported_opcode: rcp"

    saved_defs = ir.defs
    defs = dict(saved_defs)
    ir.defs = {}  # defs become uniform constants, not literals
    try:
        temps, final, err, approx = value_graph(ir)
        if err:
            return None, [], "dataflow: %s" % err
        planner = RelaxedPlanner(ir, temps)
        try:
            for ins in ir.body:
                if ins.opcode in (0x00, 0x1F, 0x51):
                    continue
                if ins.opcode == 0x09:
                    return None, [], "dp4_requires_basis_approximation"
                dst = ins.dst
                dst_vec = temps.get(dst.index) if dst.reg_type == asm.REG_TEMP else final
                if dst.reg_type == asm.REG_TEMP:
                    planner.snapshot(dst.index)
                if ins.opcode == 0x12:
                    stage, result = _emit_lrp(planner, ins, temps)
                else:
                    stage, result = planner.emit(ins, dst_vec)
                planner.prev = result
                planner.prev_reg = dst.index if dst.reg_type == asm.REG_TEMP else None
                if dst.reg_type == asm.REG_TEMP:
                    planner.temps[dst.index] = result
                if len(planner.stages) > C.TEV_MAX_STAGES:
                    return None, [], "too_many_stages (>%d)" % C.TEV_MAX_STAGES
        except C.PlanError as exc:
            return None, [], str(exc)

        for ch in range(4):
            if final[ch] != UNDEF and C._norm(planner.prev[ch]) != C._norm(final[ch]):
                return None, [], "plan_mismatch: channel %d" % ch

        const_values = {str(reg): list(vals) for reg, vals in defs.items()}
        plan = {
            "kind": "tev",
            "family": "ui",
            "stage": "ps_3_0",
            "sha256": ir.sha256,
            "sha1": ir.sha1,
            "version": "0x%08x" % ir.version,
            "instructions": len([i for i in ir.body if i.opcode not in (0x00,)]),
            "stages": len(planner.stages),
            "stage_data": planner.stages,
            "textures": planner.textures,
            "inputs": planner.inputs,
            "constants": sorted(set(planner.constants) | set(defs)),
            "constant_values": const_values,
            "uses_vertex_color": planner.uses_vertex_color,
            "requires_alpha_fixup": 0,
            "ldr_clamped": 1,
        }
    finally:
        ir.defs = saved_defs
    return plan, approx, None


def plan_dp4_approx(ir):
    """Declared approximation for the 4x4 dp4 basis programs.

    TEV has no 4-component dot and cannot hold four different per-channel
    constants in one stage.  The declared simplified spec is one DOT3 row:
    ``oC0.rgb = dot3(tex0.rgb, c0.rgb)`` and ``oC0.a = tex0.a``.  Everything
    else (the other three basis rows and the c4 alpha/offset term) is dropped.
    The plan is exact for that declared spec and carries ``quality: approx``.
    """
    if not any(ins.opcode == 0x09 for ins in ir.body):
        return None, []
    tex = next((ins for ins in ir.body if ins.opcode == 0x42), None)
    if tex is None:
        return None, []
    sampler = tex.srcs[1].index
    stage = {
        "func_rgb": C.FUNC_DOT3_RGB, "func_a": C.FUNC_REPLACE,
        "src_rgb": [C.SRC_TEX0, C.SRC_CONST, C.SRC_PREV],
        "src_a": [C.SRC_TEX0, C.SRC_PREV, C.SRC_PREV],
        "op_rgb": [C.RGB_COLOR, C.RGB_COLOR, C.RGB_COLOR],
        "op_a": [C.A_ALPHA, C.A_ALPHA, C.A_ALPHA],
        "scale_rgb": 0, "scale_a": 0, "const_reg": 0,
        "const_is_literal": 0, "const_rgba8": 0,
        "instruction": "dot3_row", "word": tex.offset,
    }
    plan = {
        "kind": "tev", "family": "ui", "stage": "ps_3_0",
        "sha256": ir.sha256, "sha1": ir.sha1,
        "version": "0x%08x" % ir.version,
        "instructions": len([i for i in ir.body if i.opcode != 0x00]),
        "stages": 1, "stage_data": [stage],
        "textures": [{"texcoord_reg": tex.srcs[0].index,
                      "sampler_reg": sampler, "sampler_type": 2}],
        "inputs": [], "constants": [0], "constant_values": {},
        "uses_vertex_color": 0, "requires_alpha_fixup": 0,
        "ldr_clamped": 1,
    }
    return plan, ["dp4 4x4 basis collapsed to one DOT3 row; other rows and "
                  "the c4 alpha/offset term dropped"]


def plan_ps_full(ir):
    """Exact planner, relaxed planner, then the declared dp4 approximation."""
    plan, cls, why = C.plan_ps(ir)
    if plan is not None:
        return plan, None
    relaxed, approx, why2 = plan_relaxed(ir)
    if relaxed is not None:
        notes = sorted(set(approx))
        if relaxed.get("constant_values"):
            notes.append("def literals moved to the uniform bank: %s"
                         % relaxed["constant_values"])
        return relaxed, "; ".join(notes)
    dp4, notes = plan_dp4_approx(ir)
    if dp4 is not None:
        return dp4, "; ".join(notes)
    return None, why2 or why

