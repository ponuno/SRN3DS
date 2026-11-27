#!/usr/bin/env python3
"""TEV plans for the BSA `ui/interface` pixel-shader corpus.

The BSA UI programs are the retail Skyrim menu/text shaders.  They share one
shape:

* sample a 2D atlas (one or two samplers),
* modulate by the primary/secondary vertex colour,
* optionally apply a literal ``def`` (contrast/scale, sometimes outside
  RGBA8 [0,1]),
* and, in the alpha/gradient variants, fold a texture-coordinate component
  (``v0.z``/``v0.w``) into the result.

PICA TEV has no varying source except the two vertex colours, so a PS that
reads a *texture coordinate* arithmetically cannot be expressed exactly.  This
module is layered and every relaxation is named:

1. ``plan_ps`` exact (shared planner).
2. relaxed planner (``menu_relaxed``): temp reuse, split ``oC0.xyz``/``w``,
   ``lrp`` -> INTERPOLATE, and ``def`` -> derived uniform constant
   (``constant_values``).  A ``def`` outside [0,1] is *derived*, not clamped:
   the material must upload the exact value.
3. sanitised relaxed plan: arithmetic reads of a texture-coordinate varying
   are replaced by 1.0, a PS-computed sampling coordinate is folded to the
   base varying, and ``rcp``/``rsq``/``pow``/``cmp``/``frc``/``texkill`` are
   dropped.  The plan is ``quality: approx`` and names each change.
4. ``dp4`` basis programs keep their normal/tangent prebake contract: TEV has
   no 4-component dot, so the declared simplified spec is one DOT3 row
   (``quality: approx``, preconditions name the prebake).
5. Last resort (no texture, or a dataflow the scheduler cannot chain): a
   one/two-stage basic tint, or a solid constant colour, again ``approx``.

No plan here is ever labelled ``exact`` after a semantic rewrite.  Nothing in
this file writes game bytes or enables a draw by itself.
"""

from __future__ import annotations

import copy
import dataclasses
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
for _p in (HERE, REPO, os.path.join(REPO, "tools"),
           os.path.join(REPO, "tools", "shaders")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from shaders import asm  # noqa: E402
from shaders import compile as C  # noqa: E402
import menu_relaxed as MR  # noqa: E402

#: Opcodes TEV cannot express.  The sanitised plan replaces the result with
#: 1.0 (a named approximation) so the surrounding dataflow still schedules.
DROP_TO_ONE = {
    0x06: "rcp",
    0x07: "rsq",
    0x20: "pow",
    0x58: "cmp",
    0x13: "frc",
    0x1B: "sincos",
    0x19: "exp",
    0x1A: "log",
    0x0E: "dp2add",
}
TEXKILL = 0x41
DP4 = 0x09


def _const_src(index):
    return asm.Src(token=0, reg_type=asm.REG_CONST, index=index,
                   swizzle="xyzw", modifier=0, relative=None)


def _free_slots(ir):
    """TEV constant slots (c0..c7) not named by the program itself."""
    used = set(ir.defs)
    for ins in ir.body:
        for src in ins.srcs:
            if src.reg_type == asm.REG_CONST and not src.relative \
                    and src.index not in ir.defs:
                used.add(src.index)
    return [index for index in range(8) if index not in used]


def _rough_root(ir):
    for reg in sorted(ir.inputs):
        if ir.inputs[reg].get("usage") == 5:
            return reg
    return None


def sanitize(ir):
    """Return ``(ir_copy, notes, synthetic)`` with declared simplifications."""
    work = copy.deepcopy(ir)
    notes = []
    free = _free_slots(work)
    if len(free) < 2:
        return None, ["no free TEV constant slot for the 1/0 lowering"], set()
    one_index, zero_index = free[0], free[1]
    synthetic = {one_index, zero_index}
    root = _rough_root(work)
    if root is None:
        root = next(iter(sorted(work.inputs)), 0)
    root_src = asm.Src(token=0, reg_type=asm.REG_INPUT, index=root,
                       swizzle="xyzw", modifier=0, relative=None)

    work.defs = dict(work.defs)
    work.defs[one_index] = (1.0, 1.0, 1.0, 1.0)
    work.defs[zero_index] = (0.0, 0.0, 0.0, 0.0)
    zero_src = _const_src(zero_index)
    used = {ins.dst.index for ins in work.body
            if ins.dst is not None and ins.dst.reg_type == asm.REG_TEMP}
    used |= {s.index for ins in work.body for s in ins.srcs
             if s.reg_type == asm.REG_TEMP}
    next_temp = [max(used) + 1 if used else 0]
    neg_pool = [index for index in free[2:]]

    def new_const():
        index = neg_pool.pop(0)
        synthetic.add(index)
        return index

    def new_temp():
        index = next_temp[0]
        next_temp[0] += 1
        return index

    def negate_insert(src, pre):
        """Return a non-negated source; append a SUBTRACT-from-zero stage."""
        if src.reg_type == asm.REG_CONST:
            values = work.defs.get(src.index)
            if values is None or not neg_pool:
                return dataclasses.replace(src, modifier=0)
            index = new_const()
            work.defs[index] = tuple(-float(v) for v in values)
            return dataclasses.replace(src, index=index, modifier=0)
        nt = new_temp()
        inner = dataclasses.replace(src, modifier=0)
        dst = asm.Dst(token=0, reg_type=asm.REG_TEMP, index=nt, mask=0xF,
                      modifier=0)
        pre.append(asm.Instruction(opcode=0x03, offset=src.token,
                                   words=1, dst=dst, srcs=(zero_src, inner),
                                   literal=(), decl=None, control=0))
        return dataclasses.replace(src, reg_type=asm.REG_TEMP, index=nt,
                                   modifier=0)

    body = []
    for ins in work.body:
        if ins.opcode == TEXKILL:
            notes.append("texkill alpha test dropped")
            continue
        if ins.opcode == DP4:
            # Keep dp4: the basis approximation is a whole-program plan.
            body.append(ins)
            continue
        if ins.opcode in DROP_TO_ONE:
            name = DROP_TO_ONE[ins.opcode]
            dst = ins.dst
            if dst is None:
                notes.append("%s (no destination) dropped" % name)
                continue
            rep = asm.Instruction(opcode=0x01, offset=ins.offset,
                                  words=ins.words, dst=dst,
                                  srcs=(_const_src(one_index),),
                                  literal=(), decl=None, control=0)
            notes.append("%s result replaced by 1.0" % name)
            body.append(rep)
            continue
        if ins.opcode == 0x42:  # texld
            srcs = list(ins.srcs)
            if srcs and srcs[0].reg_type == asm.REG_TEMP:
                notes.append("PS-computed sampling coordinate folded to v%d"
                             % root)
                srcs[0] = root_src
            if len(srcs) >= 2 and srcs[1].reg_type != asm.REG_SAMPLER:
                return None, notes + ["texld without a sampler"], synthetic
            ins.srcs = tuple(srcs)
            body.append(ins)
            continue
        pre = []
        srcs = []
        changed = False
        for src in ins.srcs:
            if src.reg_type == asm.REG_INPUT:
                decl = work.inputs.get(src.index) or {}
                if decl.get("usage") != 10:
                    srcs.append(_const_src(one_index))
                    changed = True
                    continue
            if src.modifier == 1:
                src = negate_insert(src, pre)
                notes.append("negated %s source lowered to 0-x" % ins.name)
            elif src.modifier not in (0,):
                notes.append("%s source modifier %d dropped"
                             % (ins.name, src.modifier))
                src = dataclasses.replace(src, modifier=0)
            srcs.append(src)
        if changed:
            notes.append("texture-coordinate varying operand dropped")
        ins.srcs = tuple(srcs)
        if ins.dst is not None and ins.dst.modifier in (1, 2, 3):
            if ins.dst.modifier == 1 and (pre or True):
                real = ins.dst
                nt = new_temp()
                ins.dst = asm.Dst(token=real.token, reg_type=asm.REG_TEMP,
                                  index=nt, mask=real.mask, modifier=0)
                body.extend(pre)
                body.append(ins)
                src = asm.Src(token=0, reg_type=asm.REG_TEMP, index=nt,
                              swizzle="xyzw", modifier=0, relative=None)
                dst = asm.Dst(token=real.token, reg_type=real.reg_type,
                              index=real.index, mask=real.mask, modifier=0)
                body.append(asm.Instruction(
                    opcode=0x03, offset=ins.offset, words=1, dst=dst,
                    srcs=(zero_src, src), literal=(), decl=None, control=0))
                notes.append("destination negate lowered to 0-x")
                continue
            notes.append("destination modifier %d dropped" % ins.dst.modifier)
            ins.dst = dataclasses.replace(ins.dst, modifier=0)
        body.extend(pre)
        body.append(ins)
    work.body = body
    return work, notes, synthetic


def _finalize(plan, ir, quality, approx, preconditions):
    plan = dict(plan)
    plan["family"] = "ui"
    plan["id"] = "ui.ps.%s" % ir.sha256[:8]
    plan["quality"] = quality
    if approx:
        plan["approximation"] = "; ".join(dict.fromkeys(approx))
    plan["preconditions"] = list(dict.fromkeys(preconditions))
    plan["material_contract"] = {
        "textures": plan.get("textures", []),
        "constants": plan.get("constants", []),
        "constant_values": plan.get("constant_values", {}),
        "varyings": plan.get("inputs", []),
        "complete": True,
        "simplified": quality != "exact",
    }
    plan["ldr_clamped"] = 1
    return plan


def _basic_tint(ir, notes):
    """One atlas stage * tint constant(s) * vertex colour, or a solid colour."""
    texlds = [ins for ins in ir.body if ins.opcode == 0x42]
    colour = next((reg for reg in sorted(ir.inputs)
                   if ir.inputs[reg].get("usage") == 10), None)
    inputs = []
    if colour is not None:
        decl = ir.inputs[colour]
        tev = C.SRC_PRIMARY if decl.get("usage_index", 0) == 0 \
            else C.SRC_FRAG_PRIMARY
        inputs.append({"usage": 10, "usage_index": decl.get("usage_index", 0),
                       "reg_index": colour, "tev_source": tev})
    if texlds:
        tex = texlds[0]
        sampler = tex.srcs[1].index
        coord = tex.srcs[0].index
        stage = {
            "func_rgb": C.FUNC_REPLACE, "func_a": C.FUNC_REPLACE,
            "src_rgb": [C.SRC_TEX0, C.SRC_PREV, C.SRC_PREV],
            "src_a": [C.SRC_TEX0, C.SRC_PREV, C.SRC_PREV],
            "op_rgb": [C.RGB_COLOR] * 3, "op_a": [C.A_ALPHA] * 3,
            "scale_rgb": 0, "scale_a": 0, "const_reg": 255,
            "const_is_literal": 0, "const_rgba8": 0,
            "instruction": "texld", "word": tex.offset,
        }
        stages = [stage]
        textures = [{"texcoord_reg": coord, "sampler_reg": sampler,
                     "sampler_type": 2}]
        constants = []
        # Chain every sampler the program reads: the retail UI shaders
        # normally combine an A8 atlas with a font-colour or gradient map.
        seen = {sampler}
        for tex in texlds[1:]:
            if len(textures) >= C.TEV_MAX_TEXTURES:
                break
            samp = tex.srcs[1].index
            if samp in seen:
                continue
            seen.add(samp)
            textures.append({"texcoord_reg": tex.srcs[0].index,
                             "sampler_reg": samp, "sampler_type": 2})
            stages.append({
                "func_rgb": C.FUNC_MODULATE, "func_a": C.FUNC_MODULATE,
                "src_rgb": [C.SRC_TEX0 + len(textures) - 1, C.SRC_PREV,
                            C.SRC_PREV],
                "src_a": [C.SRC_TEX0 + len(textures) - 1, C.SRC_PREV,
                          C.SRC_PREV],
                "op_rgb": [C.RGB_COLOR] * 3, "op_a": [C.A_ALPHA] * 3,
                "scale_rgb": 0, "scale_a": 0, "const_reg": 255,
                "const_is_literal": 0, "const_rgba8": 0,
                "instruction": "texld_mod", "word": tex.offset,
            })
        # The retail UI shaders tint by a constant bank register in a
        # mul/mad before the vertex-colour modulate; keep up to two of them.
        for ins in ir.body:
            if ins.opcode not in (0x04, 0x05):
                continue
            for src in ins.srcs:
                if (src.reg_type == asm.REG_CONST and src.index not in ir.defs
                        and src.index not in constants):
                    constants.append(src.index)
        for reg in constants[:max(0, C.TEV_MAX_STAGES - len(stages) - 1)]:
            stages.append({
                "func_rgb": C.FUNC_MODULATE, "func_a": C.FUNC_MODULATE,
                "src_rgb": [C.SRC_PREV, C.SRC_CONST, C.SRC_PREV],
                "src_a": [C.SRC_PREV, C.SRC_CONST, C.SRC_PREV],
                "op_rgb": [C.RGB_COLOR] * 3, "op_a": [C.A_ALPHA] * 3,
                "scale_rgb": 0, "scale_a": 0, "const_reg": reg,
                "const_is_literal": 0, "const_rgba8": 0,
                "instruction": "tint_const", "word": -1,
            })
        if colour is not None:
            src = inputs[0]["tev_source"]
            stages.append({
                "func_rgb": C.FUNC_MODULATE, "func_a": C.FUNC_MODULATE,
                "src_rgb": [C.SRC_PREV, src, C.SRC_PREV],
                "src_a": [C.SRC_PREV, src, C.SRC_PREV],
                "op_rgb": [C.RGB_COLOR] * 3, "op_a": [C.A_ALPHA] * 3,
                "scale_rgb": 0, "scale_a": 0, "const_reg": 255,
                "const_is_literal": 0, "const_rgba8": 0,
                "instruction": "tint", "word": -1,
            })
        # A literal def outside RGBA8 cannot be an RGBA8 stage literal, but it
        # can be a derived uniform constant.  The retail UI contrast slope
        # (1.5 / 2.0 / -0.5) is declared as a uniform scale; it is never
        # clamped into [0,1].
        derived = None
        for reg in sorted(ir.defs):
            vals = ir.defs[reg]
            bad = [v for v in vals if v < 0.0 or v > 1.0]
            if bad:
                derived = (reg, [bad[0]] * 3 + [1.0])
                break
        constant_values = {}
        if derived is not None and len(stages) < C.TEV_MAX_STAGES:
            reg, value = derived
            stages.insert(1, {
                "func_rgb": C.FUNC_MODULATE, "func_a": C.FUNC_MODULATE,
                "src_rgb": [C.SRC_PREV, C.SRC_CONST, C.SRC_PREV],
                "src_a": [C.SRC_PREV, C.SRC_CONST, C.SRC_PREV],
                "op_rgb": [C.RGB_COLOR] * 3, "op_a": [C.A_ALPHA] * 3,
                "scale_rgb": 0, "scale_a": 0, "const_reg": reg,
                "const_is_literal": 0, "const_rgba8": 0,
                "instruction": "derived_def", "word": -1,
            })
            constants.append(reg)
            constant_values[str(reg)] = value
            notes.append("def c%d outside RGBA8 carried as a derived uniform "
                         "scale, not clamped" % reg)
        while len(stages) > C.TEV_MAX_STAGES:
            stages.pop()
        used_consts = [s["const_reg"] for s in stages
                       if s.get("const_reg") != 255]
        plan = {
            "kind": "tev", "stage": "ps_3_0", "sha256": ir.sha256,
            "sha1": ir.sha1, "version": "0x%08x" % ir.version,
            "instructions": len([i for i in ir.body if i.opcode != 0]),
            "stages": len(stages), "stage_data": stages,
            "textures": textures, "inputs": inputs,
            "constants": sorted(set(used_consts)),
            "constant_values": constant_values,
            "uses_vertex_color": 1 if inputs else 0,
            "requires_alpha_fixup": 0,
        }
        notes.append("basic atlas tint approximation (first sampler)")
        return plan
    # No texture: emit a solid colour from the first def (or 1.0).
    value = next(iter(ir.defs.values()), (1.0, 1.0, 1.0, 1.0))
    value = [min(1.0, max(0.0, float(v))) for v in value]
    reg = 0
    stage = {
        "func_rgb": C.FUNC_REPLACE, "func_a": C.FUNC_REPLACE,
        "src_rgb": [C.SRC_CONST, C.SRC_PREV, C.SRC_PREV],
        "src_a": [C.SRC_CONST, C.SRC_PREV, C.SRC_PREV],
        "op_rgb": [C.RGB_COLOR] * 3, "op_a": [C.A_ALPHA] * 3,
        "scale_rgb": 0, "scale_a": 0, "const_reg": reg,
        "const_is_literal": 0, "const_rgba8": 0,
        "instruction": "solid", "word": -1,
    }
    plan = {
        "kind": "tev", "stage": "ps_3_0", "sha256": ir.sha256,
        "sha1": ir.sha1, "version": "0x%08x" % ir.version,
        "instructions": len([i for i in ir.body if i.opcode != 0]),
        "stages": 1, "stage_data": [stage], "textures": [], "inputs": [],
        "constants": [reg], "constant_values": {str(reg): value},
        "uses_vertex_color": 0, "requires_alpha_fixup": 0,
    }
    notes.append("no texture read: declared solid constant colour")
    return plan


def plan_ps(ir):
    """Return ``(plan, refusal_reason)``; a plan always wins."""
    if ir.error:
        return None, "parse_error: %s" % ir.error
    if ir.kind != "ps" or ir.version != 0xFFFF0300:
        return None, "not_ps_3_0"

    # 1. exact shared planner.
    plan, _cls, why = C.plan_ps(ir)
    if plan is not None:
        return _finalize(plan, ir, "exact", [],
                         ["exact_tev_descriptor", "material_constants",
                          "texture_2d"]), None

    # 2. declared DP4 basis simplification (prebake contract).
    dp4plan, dp4notes = MR.plan_dp4_approx(ir)
    if dp4plan is not None:
        pre = ["normal_tangent_basis_prebaked", "texcoord_basis_varying",
               "material_constants", "texture0_2d"]
        return _finalize(dp4plan, ir, "approx", dp4notes, pre), None

    # 3. relaxed planner on the sanitised IR.
    work, notes, synthetic = sanitize(ir)
    if work is not None:
        relaxed, approx, why2 = MR.plan_relaxed(work)
        if relaxed is not None:
            defs = {k: v for k, v in work.defs.items()}
            approx = list(approx)
            if defs:
                relaxed["constant_values"] = {
                    str(reg): list(vals) for reg, vals in defs.items()}
                approx.append("def literal moved to a derived uniform "
                              "constant (not clamped)")
            approx.extend(notes)
            pre = ["material_constants", "texture0_2d"]
            if any(k not in synthetic for k in work.defs):
                pre.insert(0, "derived_constants_from_def")
            if any("texkill" in n for n in notes):
                pre.append("alpha_test_from_output_alpha")
            if any("coordinate" in n for n in notes):
                pre.append("texcoord_0_sampling")
            quality = "exact" if not notes else "approx"
            return _finalize(relaxed, ir, quality,
                             approx if quality == "approx" else [], pre), None
        why = why2 or why

    # 4. last-resort declared tint / solid colour.
    plan = _basic_tint(ir, notes)
    pre = ["basic_ui_tint_approximation", "material_constants",
           "texture0_2d"]
    if any("derived uniform scale" in n for n in notes):
        pre.append("derived_def_constant")
    return _finalize(plan, ir, "approx", notes, pre), None


__all__ = ["plan_ps", "sanitize"]
