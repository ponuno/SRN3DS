"""Offline SM3 -> PICA200 compiler (variant B).

Decodes a shipped ``.fxp`` chunk, builds a small straight-line SM3 IR,
classifies it against the PICA200 TEV / vertex-shader envelope, and emits a
**native plan descriptor** (``D9TevProgram`` fields or a PICA VS contract).

The planner is intentionally narrow: it accepts only expression shapes it can
prove stage-equivalent and refuses everything else by name.  A generated
descriptor is not drawn unless the runtime table marks it certified.

No game bytes are stored here; the census writes derived metadata only.
"""

from __future__ import annotations

import hashlib
import struct
from typing import Dict, List, Optional, Sequence, Tuple

from . import asm

# ---------------------------------------------------------------------------
# PICA200 limits (mirror source/d3d9_tev.h and docs/SHADERS.md section 6)
# ---------------------------------------------------------------------------

TEV_MAX_STAGES = 6
CONST_OP_NAMES = {"pack": 0, "add": 1, "mul": 2, "lerp": 3}
TEV_MAX_TEXTURES = 3
TEV_MAX_CONSTANTS = 8
VS_MAX_INPUTS = 16
VS_MAX_OUTPUTS = 7
VS_MAX_CONSTANTS = 96
VS_MAX_INSTRUCTIONS = 512

#: Direct planner opcodes.  ``dp4`` and ``lrp`` are intentionally absent:
#: TEV has no dot4, and the bridge's lrp mapping is not part of this pilot.
PS_DIRECT_OPS = {
    0x00,  # nop
    0x01,  # mov
    0x02,  # add
    0x03,  # sub
    0x04,  # mad
    0x05,  # mul
    0x08,  # dp3
    0x0A,  # min (clamp forms only)
    0x0B,  # max (clamp forms only)
    0x1F,  # dcl
    0x42,  # texld
    0x51,  # def
}
PS_BASIS_OPS = {0x09}  # dp4: needs the normal/basis prebake contract
PS_NEVER = set(range(0x00, 0x61)) - PS_DIRECT_OPS - PS_BASIS_OPS

VS_PICA_OPS = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x0B, 0x0C, 0x0D, 0x13, 0x1F, 0x20, 0x25, 0x2E, 0x51,
}
VS_EXPANDABLE_OPS = {0x23, 0x24, 0x12}  # abs, nrm, lrp
VS_FLOW = {0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x26, 0x27, 0x28, 0x29,
           0x2A, 0x2B, 0x2C, 0x2D, 0x60}
VS_NEVER = (set(range(0x40, 0x61)) - {0x51}) | {
    0x0E, 0x0F, 0x10, 0x11, 0x14, 0x15, 0x16, 0x17, 0x18, 0x21, 0x22,
}

# ---------------------------------------------------------------------------
# TEV descriptor constants (numeric values from <3ds/gpu/enums.h>)
# ---------------------------------------------------------------------------

SRC_PRIMARY = 0x00
SRC_FRAG_PRIMARY = 0x01
SRC_FRAG_SECONDARY = 0x02
SRC_TEX0 = 0x03
SRC_TEX1 = 0x04
SRC_TEX2 = 0x05
SRC_CONST = 0x0E
SRC_PREV = 0x0F

RGB_COLOR, RGB_1M_COLOR = 0x00, 0x01
RGB_ALPHA, RGB_1M_ALPHA = 0x02, 0x03
RGB_R, RGB_1M_R = 0x04, 0x05
RGB_G, RGB_1M_G = 0x08, 0x09
RGB_B, RGB_1M_B = 0x0C, 0x0D

A_ALPHA, A_1M_ALPHA = 0x00, 0x01
A_R, A_1M_R = 0x02, 0x03
A_G, A_1M_G = 0x04, 0x05
A_B, A_1M_B = 0x06, 0x07

FUNC_REPLACE = 0
FUNC_MODULATE = 1
FUNC_ADD = 2
FUNC_ADD_SIGNED = 3
FUNC_INTERPOLATE = 4
FUNC_SUBTRACT = 5
FUNC_DOT3_RGB = 6
FUNC_DOT3_RGBA = 7
FUNC_MULTIPLY_ADD = 8
FUNC_ADD_MULTIPLY = 9

FUNC_NAMES = {
    FUNC_REPLACE: "replace", FUNC_MODULATE: "modulate", FUNC_ADD: "add",
    FUNC_ADD_SIGNED: "add_signed", FUNC_INTERPOLATE: "interpolate",
    FUNC_SUBTRACT: "subtract", FUNC_DOT3_RGB: "dot3_rgb",
    FUNC_DOT3_RGBA: "dot3_rgba", FUNC_MULTIPLY_ADD: "multiply_add",
    FUNC_ADD_MULTIPLY: "add_multiply",
}

#: Menu-light reference numbers quoted from ``d9_menu_light_plan()`` in
#: source/d3d9_menu_light.c.  The pilot test asserts plan equality.
MENU_LIGHT_PLAN = {
    "stages": 5, "textures": 2, "constants": [3, 4, 7],
    "input": [(10, 0, 1, SRC_PRIMARY), (10, 1, 2, SRC_FRAG_PRIMARY)],
}

# ---------------------------------------------------------------------------
# IR
# ---------------------------------------------------------------------------


def _u32(data: bytes, pos: int) -> int:
    return struct.unpack_from("<I", data, pos)[0]


def version_kind(token: int) -> str:
    return "vs" if (token >> 16) == 0xFFFE else "ps" if (token >> 16) == 0xFFFF else "?"


def version_text(token: int) -> str:
    kind = "vs" if (token >> 16) == 0xFFFE else "ps" if (token >> 16) == 0xFFFF else "unknown"
    return "%s_%d_%d" % (kind, (token >> 8) & 0xFF, token & 0xFF)


class IR:
    __slots__ = ("sha256", "sha1", "version", "kind", "version_name", "program",
                 "inputs", "samplers", "outputs", "defs", "body", "error")

    def __init__(self, sha256, sha1, version, kind, version_name, program,
                 inputs, samplers, outputs, defs, body, error):
        self.sha256 = sha256
        self.sha1 = sha1
        self.version = version
        self.kind = kind
        self.version_name = version_name
        self.program = program
        self.inputs = inputs
        self.samplers = samplers
        self.outputs = outputs
        self.defs = defs
        self.body = body
        self.error = error


def build_ir(code: bytes) -> IR:
    program = asm.parse_program(code)
    token = _u32(code, 0) if len(code) >= 4 else 0
    inputs: Dict[int, dict] = {}
    samplers: Dict[int, int] = {}
    outputs: Dict[int, dict] = {}
    for d in program.declarations:
        if d.reg_type == asm.REG_INPUT:
            inputs[d.reg_index] = {"usage": d.usage, "usage_name": d.usage_name,
                                   "usage_index": d.usage_index}
        elif d.reg_type == asm.REG_SAMPLER:
            samplers[d.reg_index] = d.texture_type
        elif d.reg_type in (asm.REG_OUTPUT, asm.REG_COLOROUT,
                            asm.REG_RASTOUT, asm.REG_ATTROUT):
            outputs[d.reg_index] = {"usage": d.usage, "usage_name": d.usage_name,
                                    "usage_index": d.usage_index,
                                    "type": d.reg_type}
    defs: Dict[int, Tuple[float, float, float, float]] = {}
    body: List[asm.Instruction] = []
    for ins in program.instructions:
        if ins.opcode == 0x51 and ins.dst is not None and ins.dst.reg_type == asm.REG_CONST:
            defs[ins.dst.index] = tuple(ins.literal)  # type: ignore[assignment]
            body.append(ins)
        elif ins.opcode == 0x1F:
            continue
        else:
            body.append(ins)
    return IR(hashlib.sha256(code).hexdigest(), hashlib.sha1(code).hexdigest(),
              token, version_kind(token), version_text(token), program,
              inputs, samplers, outputs, defs, body, program.error)


# Symbolic channel expressions ------------------------------------------------
#   ("tex", sampler_reg, component)   ("in", v_reg, component)
#   ("const", c_reg, component)       ("lit", rgba8_packed, component)
#   ("op", name, e0, e1, e2)          ("dot3", vec0, vec1)
#   ("clamp01", e)                    ("undef", -1)

UNDEF = ("undef", -1)
LEAF_KINDS = ("tex", "in", "const", "lit")


def _pack_rgba8(vals: Sequence[float]) -> int:
    out = 0
    for i, v in enumerate(vals):
        q = int(round(max(0.0, min(1.0, v)) * 255.0))
        out |= q << (8 * i)
    return out


def _vec_from_src(src: asm.Src, temps: Dict[int, tuple], defs) -> tuple:
    def one(ch: int):
        comp = "xyzw".index(src.swizzle[ch])
        if src.reg_type == asm.REG_TEMP:
            vec = temps.get(src.index)
            return vec[comp] if vec else ("tmp", src.index, comp)
        if src.reg_type == asm.REG_INPUT:
            return ("in", src.index, comp)
        if src.reg_type == asm.REG_CONST:
            if src.index in defs:
                return ("lit", _pack_rgba8(defs[src.index]), comp)
            return ("const", src.index, comp)
        return ("other", src.reg_type, src.index, comp)
    return tuple(one(ch) for ch in range(4))


def build_value_graph(ir: IR) -> Tuple[Dict[int, tuple], tuple, Optional[str]]:
    """Return (temp vectors, final oC0 vector, error)."""
    temps: Dict[int, tuple] = {}
    out: List[tuple] = [UNDEF] * 4
    for ins in ir.body:
        if ins.opcode in (0x1F, 0x51, 0x00):
            continue
        dst = ins.dst
        if dst is None or not dst.mask:
            return {}, tuple(out), "instruction %s has no destination mask at word %d" % (
                ins.name, ins.offset)
        if dst.modifier not in (0, 1, 2):
            return {}, tuple(out), "destination modifier %d on %s at word %d" % (
                dst.modifier, ins.name, ins.offset)
        if any(s.relative for s in ins.srcs):
            return {}, tuple(out), "relative addressing on %s at word %d" % (
                ins.name, ins.offset)
        if any(s.modifier not in (0,) for s in ins.srcs):
            bad = next(s for s in ins.srcs if s.modifier)
            return {}, tuple(out), "source modifier %d on %s at word %d" % (
                bad.modifier, ins.name, ins.offset)
        if dst.reg_type == asm.REG_TEMP:
            if dst.index in temps:
                return {}, tuple(out), "temp r%d is written twice" % dst.index
            vec = [UNDEF] * 4
        elif dst.reg_type == asm.REG_COLOROUT:
            if dst.index != 0:
                return {}, tuple(out), "writes oC%d (MRT)" % dst.index
            vec = list(out)
        elif dst.reg_type == asm.REG_DEPTHOUT:
            return {}, tuple(out), "writes oDepth"
        else:
            return {}, tuple(out), "writes unsupported register type %d" % dst.reg_type
        if ins.opcode == 0x42:
            if ins.control:
                return {}, tuple(out), "texld control 0x%02x (%s) at word %d" % (
                    ins.control, ins.control_name, ins.offset)
            if len(ins.srcs) < 2:
                return {}, tuple(out), "texld without coord/sampler"
            coord, samp = ins.srcs[0], ins.srcs[1]
            if coord.reg_type != asm.REG_INPUT:
                return {}, tuple(out), "dependent texture read (coord is %s%d)" % (
                    coord.type_name, coord.index)
            if samp.reg_type != asm.REG_SAMPLER:
                return {}, tuple(out), "texld without sampler register"
            if ir.samplers.get(samp.index) != 2:
                return {}, tuple(out), "sampler s%d is not 2D" % samp.index
            if dst.mask != 0xF:
                return {}, tuple(out), "texld partial write mask 0x%x" % dst.mask
            for ch in range(4):
                vec[ch] = ("tex", samp.index, ch)
        elif ins.opcode in (0x01, 0x02, 0x03, 0x04, 0x05, 0x08):
            need = {0x01: 1, 0x02: 2, 0x03: 2, 0x04: 3, 0x05: 2, 0x08: 2}[ins.opcode]
            if len(ins.srcs) < need:
                return {}, tuple(out), "%s needs %d sources, has %d at word %d" % (
                    ins.name, need, len(ins.srcs), ins.offset)
            srcs = [_vec_from_src(s, temps, ir.defs) for s in ins.srcs[:need]]
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
        elif ins.opcode in (0x0A, 0x0B):
            if len(ins.srcs) != 2:
                return {}, tuple(out), "%s with %d sources" % (ins.name, len(ins.srcs))
            a, b = ins.srcs
            other = None
            for cand in (a, b):
                if cand.reg_type == asm.REG_CONST and cand.index in ir.defs:
                    lit = ir.defs[cand.index]
                    if ins.opcode == 0x0A and lit[:3] == (1.0, 1.0, 1.0):
                        other = b if cand is a else a
                    if ins.opcode == 0x0B and lit[:3] == (0.0, 0.0, 0.0):
                        other = b if cand is a else a
            if other is None:
                return {}, tuple(out), "%s is not a clamp-to-[0,1] form at word %d" % (
                    ins.name, ins.offset)
            svec = _vec_from_src(other, temps, ir.defs)
            for ch in range(4):
                if dst.mask & (1 << ch):
                    vec[ch] = ("clamp01", svec[ch])
        else:
            return {}, tuple(out), "unsupported opcode %s (0x%02x) at word %d" % (
                ins.name, ins.opcode, ins.offset)
        if dst.reg_type == asm.REG_TEMP:
            temps[dst.index] = tuple(vec)
        else:
            for ch in range(4):
                if dst.mask & (1 << ch):
                    out[ch] = vec[ch]
    if out[3] == UNDEF:
        out[3] = ("lit", 0xFFFFFFFF, 3)
    return temps, tuple(out), None


# ---------------------------------------------------------------------------
# Plan generation
# ---------------------------------------------------------------------------


class PlanError(Exception):
    def __init__(self, reason: str, detail: str = ""):
        super().__init__(reason if not detail else "%s: %s" % (reason, detail))


def _channel_selector(comp: int, alpha: bool) -> int:
    if alpha:
        return {0: A_R, 1: A_G, 2: A_B, 3: A_ALPHA}[comp]
    return {0: RGB_R, 1: RGB_G, 2: RGB_B, 3: RGB_ALPHA}[comp]


def _operand_selector(comps, alpha: bool) -> int:
    if not alpha and comps[0] == 0 and comps[1] == 1 and comps[2] == 2:
        return RGB_COLOR
    if alpha:
        return {0: A_R, 1: A_G, 2: A_B, 3: A_ALPHA}[comps[3]]
    return {0: RGB_R, 1: RGB_G, 2: RGB_B, 3: RGB_ALPHA}[comps[0]]


class Operand:
    """A stage source plus its PICA per-source selector."""

    __slots__ = ("vec", "comps", "source", "op_rgb", "op_a")

    def __init__(self, vec, comps, source, op_rgb, op_a):
        self.vec = vec
        self.comps = comps
        self.source = source
        self.op_rgb = op_rgb
        self.op_a = op_a


class TevPlanner:
    def __init__(self, ir: IR, temps: Dict[int, tuple]):
        self.ir = ir
        self.temps = temps
        self.stages: List[dict] = []
        self.textures: List[dict] = []
        self.tex_unit: Dict[int, int] = {}
        self.inputs: List[dict] = []
        self.constants: List[int] = []
        self.uses_vertex_color = 0
        self.prev: Optional[tuple] = None
        self.prev_reg: Optional[int] = None

    def _tex_unit(self, sampler: int) -> int:
        if sampler not in self.tex_unit:
            if len(self.tex_unit) >= TEV_MAX_TEXTURES:
                raise PlanError("too_many_textures", "sampler s%d" % sampler)
            self.tex_unit[sampler] = len(self.tex_unit)
            self.textures.append({"texcoord_reg": 0, "sampler_reg": sampler,
                                  "sampler_type": 2})
        return self.tex_unit[sampler]

    def operand(self, src: asm.Src) -> Operand:
        comps = tuple("xyzw".index(src.swizzle[ch]) for ch in range(4))
        if not ((comps[0] == 0 and comps[1] == 1 and comps[2] == 2) or
                (comps[0] == comps[1] == comps[2])):
            raise PlanError("non_uniform_swizzle",
                            "%s.%s" % (src.name(), src.swizzle))
        if src.reg_type == asm.REG_TEMP:
            if self.prev is not None and self.prev_reg == src.index:
                return Operand(self.prev, comps, SRC_PREV,
                               _operand_selector(comps, False),
                               _operand_selector(comps, True))
            alias = self.temps.get(src.index)
            if alias is not None and all(e[0] in LEAF_KINDS for e in alias):
                return self._leaf_operand(alias, comps)
            raise PlanError("temp_not_previous",
                            "r%d is not the previous stage result" % src.index)
        if src.reg_type == asm.REG_INPUT:
            return self._leaf_operand(tuple(("in", src.index, c) for c in range(4)),
                                      comps)
        if src.reg_type == asm.REG_CONST:
            if src.index in self.ir.defs:
                packed = _pack_rgba8(self.ir.defs[src.index])
                return self._leaf_operand(tuple(("lit", packed, c) for c in range(4)),
                                          comps)
            return self._leaf_operand(
                tuple(("const", src.index, c) for c in range(4)), comps)
        raise PlanError("unsupported_source_register",
                        "%s%d" % (src.type_name, src.index))

    def _leaf_operand(self, vec, comps) -> Operand:
        op_rgb = _operand_selector(comps, False)
        op_a = _operand_selector(comps, True)
        kind = vec[0][0]
        if kind == "tex":
            return Operand(vec, comps, SRC_TEX0 + self._tex_unit(vec[0][1]), op_rgb, op_a)
        if kind == "in":
            reg = vec[0][1]
            decl = self.ir.inputs.get(reg)
            if decl is None:
                raise PlanError("input_register_unmapped", "v%d has no dcl" % reg)
            if decl["usage"] != 10:  # D3DDECLUSAGE_COLOR
                raise PlanError("input_not_a_colour_varying",
                                "v%d is %s%d and TEV has no such varying" % (
                                    reg, decl["usage_name"], decl["usage_index"]))
            src = SRC_PRIMARY if reg == 1 else SRC_FRAG_PRIMARY
            self.uses_vertex_color = 1
            if not any(i["reg_index"] == reg for i in self.inputs):
                self.inputs.append({"usage": 10, "usage_index": decl["usage_index"],
                                    "reg_index": reg, "tev_source": src})
            return Operand(vec, comps, src, op_rgb, op_a)
        if kind in ("const", "lit"):
            return Operand(vec, comps, SRC_CONST, op_rgb, op_a)
        raise PlanError("unsupported_leaf", kind)

    @staticmethod
    def _const_values(operands: Sequence[Operand]):
        regs, lits = set(), set()
        for o in operands:
            if o.source != SRC_CONST:
                continue
            for e in o.vec:
                if e[0] == "const":
                    regs.add(e[1])
                elif e[0] == "lit":
                    lits.add(e[1])
        return regs, lits

    def emit(self, ins: asm.Instruction, dst_vec: tuple) -> Tuple[dict, tuple]:
        mask = ins.dst.mask
        if ins.opcode == 0x42:
            if ins.control:
                raise PlanError("texld_control_unsupported",
                                "%s at word %d" % (ins.control_name, ins.offset))
            sampler = ins.srcs[1].index
            unit = self._tex_unit(sampler)
            self.textures[unit]["texcoord_reg"] = ins.srcs[0].index
            stage = {
                "func_rgb": FUNC_REPLACE, "func_a": FUNC_REPLACE,
                "src_rgb": [SRC_TEX0 + unit, SRC_PREV, SRC_PREV],
                "src_a": [SRC_TEX0 + unit, SRC_PREV, SRC_PREV],
                "op_rgb": [RGB_COLOR] * 3, "op_a": [A_ALPHA] * 3,
                "scale_rgb": 0, "scale_a": 0, "const_reg": 255,
                "const_is_literal": 0, "const_rgba8": 0,
                "instruction": "texld", "word": ins.offset,
            }
            result = tuple(("tex", sampler, ch) for ch in range(4))
            self.stages.append(stage)
            return stage, result

        op_name = {0x01: "mov", 0x02: "add", 0x03: "sub", 0x04: "mad",
                   0x05: "mul", 0x08: "dp3", 0x0A: "clamp", 0x0B: "clamp"}[ins.opcode]
        if op_name == "clamp":
            a, b = ins.srcs
            which = 0
            for i, cand in enumerate((a, b)):
                lit = self.ir.defs.get(cand.index) if cand.reg_type == asm.REG_CONST else None
                if (ins.opcode == 0x0A and lit and lit[:3] == (1.0, 1.0, 1.0)) or (
                        ins.opcode == 0x0B and lit and lit[:3] == (0.0, 0.0, 0.0)):
                    which = 1 - i
                    break
            operands = [self.operand(ins.srcs[which])]
        elif op_name == "dp3":
            operands = [self.operand(ins.srcs[0]), self.operand(ins.srcs[1])]
            if any(o.comps != (0, 1, 2, 3) for o in operands):
                raise PlanError("dp3_operand_swizzle",
                                "dp3 operands must be unswizzled")
        else:
            n = 1 if op_name == "mov" else (3 if op_name == "mad" else 2)
            operands = [self.operand(ins.srcs[i]) for i in range(n)]

        regs, lits = self._const_values(operands)
        if len(regs) > 1:
            raise PlanError("stage_needs_two_constants", str(sorted(regs)))
        if regs and lits:
            raise PlanError("stage_mixes_literal_and_constant")
        if len(lits) > 1:
            raise PlanError("stage_needs_two_literals")
        const_reg = next(iter(regs)) if regs else 255
        literal = next(iter(lits)) if lits else None
        if const_reg != 255 and const_reg not in self.constants:
            self.constants.append(const_reg)

        func = {"mov": FUNC_REPLACE, "mul": FUNC_MODULATE, "add": FUNC_ADD,
                "sub": FUNC_SUBTRACT, "mad": FUNC_MULTIPLY_ADD,
                "clamp": FUNC_REPLACE, "dp3": FUNC_DOT3_RGBA}[op_name]
        func_rgb = func_a = func
        src_rgb = [o.source for o in operands] + [SRC_PREV] * (3 - len(operands))
        src_a = [o.source for o in operands] + [SRC_PREV] * (3 - len(operands))
        op_rgb = [o.op_rgb for o in operands] + [RGB_COLOR] * (3 - len(operands))
        op_a = [o.op_a for o in operands] + [A_ALPHA] * (3 - len(operands))

        if op_name == "dp3":
            if len(operands) != 2:
                raise PlanError("dp3_shape", ins.name)
            src_rgb = [operands[0].source, operands[1].source, SRC_PREV]
            src_a = [operands[0].source, operands[1].source, SRC_PREV]
            op_rgb = [RGB_COLOR, RGB_COLOR, RGB_COLOR]
            op_a = [A_ALPHA, A_ALPHA, A_ALPHA]
            if mask == 0x7:
                func_rgb, func_a = FUNC_DOT3_RGB, FUNC_REPLACE
                src_a = [SRC_PREV] * 3
                op_a = [A_ALPHA] * 3

        # Carry an unwritten side with REPLACE(PREV) on that side only; the
        # descriptor has independent RGB and alpha source arrays.
        if func_rgb not in (FUNC_DOT3_RGBA, FUNC_DOT3_RGB) and not any(
                mask & (1 << ch) for ch in (0, 1, 2)):
            func_rgb = FUNC_REPLACE
            src_rgb[0], op_rgb[0] = SRC_PREV, RGB_COLOR
        if func_a != FUNC_DOT3_RGBA and not (mask & 0x8):
            func_a = FUNC_REPLACE
            src_a[0], op_a[0] = SRC_PREV, A_ALPHA

        stage = {
            "func_rgb": func_rgb, "func_a": func_a,
            "src_rgb": src_rgb, "src_a": src_a,
            "op_rgb": op_rgb, "op_a": op_a,
            "scale_rgb": 0, "scale_a": 0,
            "const_reg": const_reg,
            "const_is_literal": 0 if literal is None else 1,
            "const_rgba8": 0 if literal is None else literal,
            "instruction": ins.name, "word": ins.offset,
        }
        self.stages.append(stage)
        return stage, self._stage_result(stage, operands)

    def _stage_result(self, stage: dict, operands: Sequence[Operand]) -> tuple:
        # Use the descriptor's own source arrays so a planner bug shows up in
        # the final structural comparison.
        vecs = {o.source: o.vec for o in operands
                if SRC_TEX0 <= o.source <= SRC_TEX2}

        def val(src, op, ch, alpha):
            comp = _selector_comp(op, ch, alpha)
            if SRC_TEX0 <= src <= SRC_TEX2:
                vec = vecs.get(src)
                return vec[comp] if vec else ("tex_select", src, comp)
            if src == SRC_PREV:
                return self.prev[comp] if self.prev is not None else UNDEF
            if src == SRC_CONST:
                for o in operands:
                    if o.source == SRC_CONST:
                        return o.vec[comp]
                return ("undef_const", comp)
            if src == SRC_PRIMARY:
                return ("in", 1, comp)
            if src == SRC_FRAG_PRIMARY:
                return ("in", 2, comp)
            return ("src_unknown", src)

        out = []
        for ch in range(4):
            if ch < 3:
                if stage["func_rgb"] in (FUNC_DOT3_RGBA, FUNC_DOT3_RGB):
                    out.append(("dot3", operands[0].vec, operands[1].vec))
                    continue
                v = [val(stage["src_rgb"][i], stage["op_rgb"][i], ch, False)
                     for i in range(3)]
                out.append(_combine_expr(stage["func_rgb"], v))
            else:
                if stage["func_a"] == FUNC_DOT3_RGBA:
                    out.append(("dot3", operands[0].vec, operands[1].vec))
                    continue
                v = [val(stage["src_a"][i], stage["op_a"][i], 3, True)
                     for i in range(3)]
                out.append(_combine_expr(stage["func_a"], v))
        return tuple(out)


def _selector_comp(op: int, out_ch: int, alpha: bool) -> int:
    if alpha:
        return {A_ALPHA: 3, A_1M_ALPHA: 3, A_R: 0, A_1M_R: 0,
                A_G: 1, A_1M_G: 1, A_B: 2, A_1M_B: 2}.get(op, 3)
    if op in (RGB_COLOR, RGB_1M_COLOR):
        return out_ch
    return {RGB_ALPHA: 3, RGB_1M_ALPHA: 3, RGB_R: 0, RGB_1M_R: 0,
            RGB_G: 1, RGB_1M_G: 1, RGB_B: 2, RGB_1M_B: 2}.get(op, out_ch)


def _norm(expr):
    """Strip clamp01: the LDR-clamped model clamps every stage anyway."""
    if expr[0] == "clamp01":
        return _norm(expr[1])
    if expr[0] == "op":
        return ("op", expr[1], _norm(expr[2]),
                _norm(expr[3]) if expr[3] is not None else None,
                _norm(expr[4]) if expr[4] is not None else None)
    if expr[0] == "dot3":
        return ("dot3", tuple(_norm(x) for x in expr[1]),
                tuple(_norm(x) for x in expr[2]))
    return expr


#: Named combine expressions used by the structural verifier.  The lambda
#: shape mirrors ``combine()`` in source/d3d9_tev.c; ``SELECT`` stands for the
#: pre-combine operand selector and is folded by ``_combine_expr``.
def _combine_expr(func: int, v: Sequence[tuple]) -> tuple:
    if func == FUNC_REPLACE:
        return v[0]
    if func == FUNC_MODULATE:
        return ("op", "mul", v[0], v[1], None)
    if func == FUNC_ADD:
        return ("op", "add", v[0], v[1], None)
    if func == FUNC_SUBTRACT:
        return ("op", "sub", v[0], v[1], None)
    if func == FUNC_MULTIPLY_ADD:
        return ("op", "mad", v[0], v[1], v[2])
    if func == FUNC_DOT3_RGBA:
        return ("dot3", v[0], v[1])
    return ("op", "func%d" % func, v[0], v[1], v[2] if len(v) > 2 else None)


def _output_write_indices(ir: IR) -> List[int]:
    return [i for i, ins in enumerate(ir.body)
            if ins.opcode not in (0x1F, 0x51, 0x00) and ins.dst is not None
            and ins.dst.reg_type == asm.REG_COLOROUT]


def plan_ps(ir: IR, ignore_def_range: bool = False) -> Tuple[Optional[dict], Optional[str], Optional[str]]:
    """Return (plan, class, named refusal)."""
    if ir.error:
        return None, None, "parse_error: %s" % ir.error
    if ir.kind != "ps" or ir.version != 0xFFFF0300:
        return None, None, "not_ps_3_0 (%s)" % ir.version_name
    if not ir.body:
        return None, None, "empty_program"
    if not ignore_def_range:
        for reg, vals in ir.defs.items():
            if any(not (v == v) or v < 0.0 or v > 1.0 for v in vals):
                return None, None, "def_literal_out_of_range: c%d" % reg
    ops = [ins.opcode for ins in ir.body]
    if any(op in PS_NEVER for op in ops):
        bad = next(op for op in ops if op in PS_NEVER)
        return None, None, "unsupported_opcode: %s" % asm.OPCODE_NAMES.get(bad, "0x%02x" % bad)
    if any(op in PS_BASIS_OPS for op in ops):
        return None, "tev_prebake", ("dp4_basis_requires_prebake: normal/tangent "
                                     "basis must be prebaked")
    temps, final, err = build_value_graph(ir)
    if err:
        return None, None, "dataflow: %s" % err

    writes = _output_write_indices(ir)
    if not writes:
        return None, None, "no_output_write"
    if len(writes) > 1:
        return None, None, "multiple_output_writes (%d)" % len(writes)
    last = len(ir.body) - 1
    while last >= 0 and ir.body[last].opcode in (0x00, 0x1F, 0x51):
        last -= 1
    if writes[0] != last:
        return None, None, "final_output_not_last_write"
    if ir.body[last].dst.mask != 0xF:
        return None, None, "final_output_partial_mask_0x%x" % ir.body[last].dst.mask

    planner = TevPlanner(ir, temps)
    try:
        for ins in ir.body:
            if ins.opcode in (0x00, 0x1F, 0x51):
                continue
            dst = ins.dst
            dst_vec = temps[dst.index] if dst.reg_type == asm.REG_TEMP else final
            stage, result = planner.emit(ins, dst_vec)
            planner.prev = result
            planner.prev_reg = dst.index if dst.reg_type == asm.REG_TEMP else None
            if len(planner.stages) > TEV_MAX_STAGES:
                return None, None, "too_many_stages (>%d)" % TEV_MAX_STAGES
        # Alpha fixup, exactly like the C translator.
        if final[3] == ("lit", 0xFFFFFFFF, 3) and planner.prev[3] != final[3]:
            if len(planner.stages) >= TEV_MAX_STAGES:
                return None, None, "too_many_stages (alpha fixup)"
            stage = {
                "func_rgb": FUNC_REPLACE, "func_a": FUNC_REPLACE,
                "src_rgb": [SRC_PREV] * 3, "src_a": [SRC_PREV, SRC_CONST, SRC_PREV],
                "op_rgb": [RGB_COLOR] * 3, "op_a": [A_ALPHA] * 3,
                "scale_rgb": 0, "scale_a": 0, "const_reg": 255,
                "const_is_literal": 1, "const_rgba8": 0xFFFFFFFF,
                "instruction": "alpha_fixup", "word": -1,
            }
            planner.stages.append(stage)
            planner.prev = planner.prev[:3] + (final[3],)
    except PlanError as exc:
        return None, None, str(exc)

    for ch in range(4):
        if final[ch] != UNDEF and _norm(planner.prev[ch]) != _norm(final[ch]):
            return None, None, "plan_mismatch: channel %d" % ch
    plan = {
        "kind": "tev",
        "family": "generic",
        "stage": "ps_3_0",
        "sha256": ir.sha256,
        "sha1": ir.sha1,
        "version": "0x%08x" % ir.version,
        "instructions": len([i for i in ir.body if i.opcode not in (0x00,)]),
        "stages": len(planner.stages),
        "stage_data": planner.stages,
        "textures": planner.textures,
        "inputs": planner.inputs,
        "constants": sorted(planner.constants),
        "uses_vertex_color": planner.uses_vertex_color,
        "requires_alpha_fixup": 1 if planner.stages[-1]["instruction"] == "alpha_fixup" else 0,
        "ldr_clamped": 1,
    }
    return plan, "tev_direct", None


def plan_vs(ir: IR) -> Tuple[Optional[dict], Optional[str], Optional[str]]:
    """Return (pica-vs descriptor, class, named refusal)."""
    if ir.error:
        return None, None, "parse_error: %s" % ir.error
    if ir.kind != "vs" or ir.version != 0xFFFE0300:
        return None, None, "not_vs_3_0 (%s)" % ir.version_name
    ops = [ins.opcode for ins in ir.body]
    if any(op in VS_FLOW for op in ops):
        bad = next(op for op in ops if op in VS_FLOW)
        return None, None, "control_flow: %s" % asm.OPCODE_NAMES.get(bad, "0x%02x" % bad)
    if any(op in VS_NEVER for op in ops):
        bad = next(op for op in ops if op in VS_NEVER)
        return None, None, "unsupported_opcode: %s" % asm.OPCODE_NAMES.get(bad, "0x%02x" % bad)
    expandable = sorted({asm.OPCODE_NAMES.get(op, "0x%02x" % op)
                         for op in ops if op in VS_EXPANDABLE_OPS})
    unsupported = sorted({asm.OPCODE_NAMES.get(op, "0x%02x" % op)
                          for op in ops if op not in VS_PICA_OPS | VS_EXPANDABLE_OPS})
    if unsupported:
        return None, None, "unsupported_opcode: %s" % unsupported[0]
    consts = set()
    for ins in ir.body:
        for src in ins.srcs:
            if src.reg_type == asm.REG_CONST:
                consts.add(src.index)
    if len(consts) > VS_MAX_CONSTANTS:
        return None, None, "too_many_vs_constants (%d > %d)" % (len(consts), VS_MAX_CONSTANTS)
    if len(ir.inputs) > VS_MAX_INPUTS:
        return None, None, "too_many_vs_inputs (%d > %d)" % (len(ir.inputs), VS_MAX_INPUTS)
    outputs = [d for d in ir.program.declarations
               if d.reg_type in (asm.REG_OUTPUT, asm.REG_RASTOUT, asm.REG_ATTROUT)]
    varyings = [d for d in ir.program.declarations
                if d.reg_type == asm.REG_OUTPUT and d.usage != 0]
    if len(varyings) > VS_MAX_OUTPUTS:
        return None, None, "too_many_vs_varyings (%d > %d)" % (
            len(varyings), VS_MAX_OUTPUTS)
    if len(ir.body) > VS_MAX_INSTRUCTIONS:
        return None, None, "too_many_vs_instructions (%d > %d)" % (
            len(ir.body), VS_MAX_INSTRUCTIONS)
    if not any(d.usage == 0 and d.reg_type in (asm.REG_OUTPUT, asm.REG_RASTOUT)
               for d in outputs):
        return None, None, "missing_vs_position_output"
    inputs = []
    for reg in sorted(ir.inputs):
        d = ir.inputs[reg]
        inputs.append({"reg_index": reg, "usage": d["usage"],
                       "usage_name": d["usage_name"], "usage_index": d["usage_index"]})
    outs = []
    for d in outputs:
        outs.append({"reg_index": d.reg_index, "type": d.reg_type,
                     "usage": d.usage, "usage_name": d.usage_name,
                     "usage_index": d.usage_index})
    insns = []
    for ins in ir.body:
        if ins.opcode in (0x1F, 0x51):
            continue
        insns.append({"op": ins.name, "text": ins.text()})
    plan = {
        "kind": "pica_vs",
        "stage": "vs_3_0",
        "sha256": ir.sha256,
        "sha1": ir.sha1,
        "version": "0x%08x" % ir.version,
        "instructions": len(insns),
        "inputs": inputs,
        "outputs": outs,
        "constants": sorted(consts),
        "uniform_registers": len(consts),
        "expandable": expandable,
        "skinning": any(i["usage"] in (1, 2) for i in inputs),
        "codegen": "pending",
    }
    return plan, "pica_vs", None


# ---------------------------------------------------------------------------
# Certified menu-light reference generator
# ---------------------------------------------------------------------------


def menu_light_signature(ir: IR) -> Optional[str]:
    """Recognise the exact p1200005 retail signature (C matcher is authority)."""
    body = ir.body
    if len(body) != 20 or body[0].opcode != 0x51:
        return None
    if tuple(ir.defs.get(8, ())) != (-0.5, 1.0, 0.0, 0.0):
        return None
    expected = [0x42, 0x05, 0x05, 0x42, 0x02, 0x02, 0x01, 0x09, 0x09, 0x09,
                0x08, 0x02, 0x04, 0x05, 0x05, 0x01, 0x04, 0x04, 0x05]
    if [i.opcode for i in body[1:]] != expected:
        return None
    inputs = {r: (d["usage"], d["usage_index"]) for r, d in ir.inputs.items()}
    if inputs != {0: (5, 0), 1: (10, 0), 2: (10, 1)}:
        return None
    if ir.samplers != {0: 2, 1: 2}:
        return None
    return "p1200005_ADD_NORMAL"


def _stage(func_rgb, func_a, src_rgb, src_a, op_rgb, op_a,
           const_reg=255, literal=None) -> dict:
    return {
        "func_rgb": func_rgb, "func_a": func_a,
        "src_rgb": list(src_rgb), "src_a": list(src_a),
        "op_rgb": list(op_rgb), "op_a": list(op_a),
        "scale_rgb": 0, "scale_a": 0,
        "const_reg": const_reg,
        "const_is_literal": 0 if literal is None else 1,
        "const_rgba8": 0 if literal is None else literal,
    }


def menu_light_plan(ir: IR) -> Optional[dict]:
    """Emit the exact five-stage plan of ``d9_menu_light_plan()``."""
    if menu_light_signature(ir) != "p1200005_ADD_NORMAL":
        return None
    C = [RGB_COLOR, RGB_COLOR, RGB_COLOR]
    A = [A_ALPHA, A_ALPHA, A_ALPHA]
    stages = [
        _stage(FUNC_DOT3_RGBA, FUNC_DOT3_RGBA,
               [SRC_TEX1, SRC_CONST, SRC_CONST], [SRC_TEX1, SRC_CONST, SRC_CONST],
               C, A, 3),
        _stage(FUNC_MULTIPLY_ADD, FUNC_MULTIPLY_ADD,
               [SRC_PREV, SRC_CONST, SRC_FRAG_PRIMARY],
               [SRC_PREV, SRC_CONST, SRC_FRAG_PRIMARY], C, A, 4),
        _stage(FUNC_MODULATE, FUNC_MODULATE,
               [SRC_TEX0, SRC_PREV, SRC_PREV],
               [SRC_TEX0, SRC_PRIMARY, SRC_PREV], C, A),
        _stage(FUNC_MODULATE, FUNC_MODULATE,
               [SRC_PREV, SRC_PRIMARY, SRC_PREV],
               [SRC_PREV, SRC_CONST, SRC_PREV], C, A, 4),
        _stage(FUNC_MODULATE, FUNC_REPLACE,
               [SRC_PREV, SRC_CONST, SRC_PREV],
               [SRC_PREV, SRC_PREV, SRC_PREV],
               [RGB_COLOR, RGB_ALPHA, RGB_COLOR], A, 7),
    ]
    return {
        "kind": "tev",
        "family": "menu_light",
        "stage": "ps_3_0",
        "sha256": ir.sha256,
        "sha1": ir.sha1,
        "version": "0x%08x" % ir.version,
        "instructions": 25,
        "stages": 5,
        "stage_data": stages,
        "textures": [
            {"texcoord_reg": 0, "sampler_reg": 0, "sampler_type": 2},
            {"texcoord_reg": 0, "sampler_reg": 1, "sampler_type": 2},
        ],
        "inputs": [
            {"usage": 10, "usage_index": 0, "reg_index": 1, "tev_source": SRC_PRIMARY},
            {"usage": 10, "usage_index": 1, "reg_index": 2, "tev_source": SRC_FRAG_PRIMARY},
        ],
        "constants": [3, 4, 7],
        "uses_vertex_color": 1,
        "requires_alpha_fixup": 0,
        "ldr_clamped": 1,
        "preconditions": [
            "normal_prebaked", "tangent_basis_prebaked",
            "ambient_emissive_varying", "material_alpha_c4",
            "material_constants_c0_c7", "ldr_quality",
            "texture0_2d", "texture1_2d", "vertex_color",
        ],
    }


# ---------------------------------------------------------------------------
# C table emission
# ---------------------------------------------------------------------------


def _tev_to_c(plan: dict, indent: str) -> str:
    """Emit one ``D9TevProgram`` initializer (shared by plans and passes)."""
    lines = []
    lines.append("%s{0x%08xu, %d, %d," % (
        indent, int(plan.get("version", "0xffff0300"), 16) & 0xFFFFFFFF,
        plan.get("instructions", 0), len(plan.get("stage_data") or [])))
    lines.append("%s    {" % indent)
    for s in plan.get("stage_data") or []:
        lines.append(
            "%s        {%d,%d,{%d,%d,%d},{%d,%d,%d},{%d,%d,%d},{%d,%d,%d},%d,%d,%d,%d,0x%08xu}," % (
                indent, s["func_rgb"], s["func_a"],
                s["src_rgb"][0], s["src_rgb"][1], s["src_rgb"][2],
                s["src_a"][0], s["src_a"][1], s["src_a"][2],
                s["op_rgb"][0], s["op_rgb"][1], s["op_rgb"][2],
                s["op_a"][0], s["op_a"][1], s["op_a"][2],
                s["scale_rgb"], s["scale_a"], s.get("const_reg", 255),
                s.get("const_is_literal", 0), s.get("const_rgba8", 0)))
    lines.append("%s    }," % indent)
    lines.append("%s%d," % (indent, len(plan.get("textures") or [])))
    lines.append("%s    {" % indent)
    for t in plan.get("textures") or []:
        lines.append("%s        {%d,%d,%d}," % (
            indent, t["texcoord_reg"], t["sampler_reg"], t["sampler_type"]))
    lines.append("%s    }," % indent)
    lines.append("%s%d," % (indent, len(plan.get("inputs") or [])))
    lines.append("%s    {" % indent)
    for i in plan.get("inputs") or []:
        lines.append("%s        {%d,%d,%d,%d}," % (
            indent, i["usage"], i["usage_index"], i["reg_index"],
            i["tev_source"]))
    lines.append("%s    }," % indent)
    consts = list(plan.get("constants") or [])
    consts += [0] * (8 - len(consts))
    lines.append("%s%d," % (indent, len(plan.get("constants") or [])))
    lines.append("%s{%s}," % (indent, ",".join(str(c) for c in consts)))
    lines.append("%s%d,%d,%d,0x%08xu," % (
        indent, plan.get("uses_vertex_color", 0),
        plan.get("requires_alpha_fixup", 0), TEV_MAX_STAGES,
        (1 if plan.get("family") == "menu_light" else 0)))
    lines.append("%s}," % indent)
    return "\n".join(lines)


def _const_ops_to_c(ops: Sequence[dict], indent: str) -> str:
    lines = [("%s%d," % (indent, len(ops))), ("%s    {" % indent)]
    for op in ops:
        srcs = []
        for s in op["src"]:
            sw = ["xyzw".index(c) for c in s["swizzle"]]
            srcs.append("{%d,{%d,%d,%d,%d},%d}" % (
                s["reg"], sw[0], sw[1], sw[2], sw[3], s.get("negate", 0)))
        lines.append("%s        {%d, %d, %d, {%s}}," % (
            indent, op["dst_reg"], CONST_OP_NAMES[op["op"]], len(op["src"]),
            ", ".join(srcs)))
    lines.append("%s    }," % indent)
    return "\n".join(lines)


def _passes_to_c(passes: Sequence[dict], plan: dict, indent: str) -> str:
    lines = [("%s%d," % (indent, len(passes))), ("%s    {" % indent)]
    for p in passes:
        sub = dict(plan)
        sub.update(p)
        sub["stage_data"] = p["stage_data"]
        sub.setdefault("version", plan.get("version", "0xffff0300"))
        lines.append("%s        {" % indent)
        lines.append(_tev_to_c(sub, indent + "            "))
        blend = p["blend"]
        lines.append("%s            %d, %d, %d, %d, %d," % (
            indent, blend["enable"], blend["src"], blend["dst"], blend["op"],
            p["depth_write"]))
        lines.append("%s        }," % indent)
    lines.append("%s    }," % indent)
    return "\n".join(lines)


def plan_to_c(plan: dict, indent: str = "    ") -> str:
    """Emit one ``D9ShaderPlan`` initializer for the runtime table."""
    lines = []
    lines.append("%s{" % indent)
    lines.append('%s    "%s",' % (indent, plan.get("id", "unnamed")))
    lines.append('%s    "%s",' % (indent, plan["sha256"]))
    stage_flag = 0 if plan["stage"] == "ps_3_0" else 1
    kind = 1 if plan["kind"] == "tev" else 2
    draw = 1 if plan.get("draw_enabled") else 0
    lines.append("%s    %d, %d, %d," % (indent, stage_flag, kind, draw))
    note = plan.get("note", "").replace('"', "'")
    lines.append('%s    "%s",' % (indent, note[:150]))
    if plan["kind"] == "tev":
        lines.append(_tev_to_c(plan, indent + "    "))
        if plan.get("constant_ops") or plan.get("passes"):
            lines.append(_const_ops_to_c(plan.get("constant_ops") or [],
                                         indent + "    "))
            lines.append(_passes_to_c(plan.get("passes") or [], plan,
                                      indent + "    "))
    else:
        lines.append("%s    {0}," % indent)
    lines.append("%s}," % indent)
    return "\n".join(lines)


def emit_c_table(plans: Sequence[dict]) -> str:
    out = []
    out.append("/* Generated by tools/fxp_compile.py emit-c.  Do not edit. */")
    out.append("/* Hash -> native pipeline descriptor.  No game blobs are stored. */")
    out.append("static const D9ShaderPlan d9_shader_plan_entries[] = {")
    for plan in plans:
        out.append(plan_to_c(plan))
    out.append("};")
    out.append("#define D9_SHADER_PLAN_ENTRY_COUNT "
               "(sizeof(d9_shader_plan_entries)/sizeof(d9_shader_plan_entries[0]))")
    out.append("")
    return "\n".join(out)


__all__ = [
    "IR", "build_ir", "build_value_graph", "plan_ps", "plan_vs",
    "menu_light_signature", "menu_light_plan", "emit_c_table", "plan_to_c",
    "TEV_MAX_STAGES", "TEV_MAX_TEXTURES", "FUNC_NAMES", "MENU_LIGHT_PLAN",
    "SRC_PRIMARY", "SRC_TEX0", "FUNC_DOT3_RGBA", "PS_DIRECT_OPS",
]
