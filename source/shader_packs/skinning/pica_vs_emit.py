#!/usr/bin/env python3
"""Minimal skinned PICA200 VS emitter and numeric verifier (host only).

Scope: the 83 ``pica_vs`` contracts in ``plans.json`` whose body is a
straight-line bone-palette transform.  The emitter lowers the canonical
vs_3_0 IR to picasso ``.v.pica`` instructions (``mul``, ``mad``, ``mov``,
``frc``, ``mova``, ``dp4``) and refuses everything else by name.

The verifier is honest in three independent layers:

1. **Structure** — outputs <= 7, inputs <= 16, uniform rows <= 96, instruction
   slots <= ``VS_MAX_INSTRUCTIONS``, no loops/branches/texld, and the text is
   accepted by picasso (real assembler/linker) with a non-empty ``.shbin``.
2. **Reference** — a D3D vs_3_0 interpreter executes the original bytecode IR
   on 256 deterministic samples (position, weights, normalised blend indices,
   UV, colour, bone palette, WVP, ``def`` literals).
3. **Emitted code** — a PICA VS interpreter parses the emitted assembly and
   runs it on the same inputs.  ``max abs error <= 1e-4`` for position and
   each varying is required.

Only after all three pass does the plan get ``codegen: emitted-verified``.
It still keeps ``draw_enabled: 0``: ``d9-shader-plan-v1`` constrains
``pica_vs`` to ``draw_enabled = 0`` until a shared schema change and a runtime
upload/bind path exist.  That request is recorded in the pack README.

Uniform layout (4-bone palette, the representative contract)::

    c0..c11   4 bones x 3 rows   (a0 = 3*bone, bone in 0..3)
    c12..c15  D3D c180..c183 (WVP), remapped

The retail palette can be larger (up to 32 bones = 96 rows); the 4-bone bank
is a draw-time palette-window contract, not an approximation of the maths.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import re
import subprocess
import sys
import tempfile
from typing import Dict, List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for path in (REPO, os.path.join(REPO, "tools"),
             os.path.join(REPO, "tools", "shaders")):
    if path not in sys.path:
        sys.path.insert(0, path)

from shaders import asm, compile as C  # noqa: E402

PLANS = os.path.join(HERE, "plans.json")
REPORT = os.path.join(HERE, "emitter_report.json")
GENERATED = os.path.join(HERE, "generated")
ARCHIVE = "Skyrim - Shaders.bsa"
DATA_CANDIDATES = [r"F:\Skyrim\Data",
                   r"E:\SteamLibrary\steamapps\common\Skyrim\Data"]
PICASSO_CANDIDATES = ["/c/devkitPro/tools/bin/picasso.exe",
                      r"C:\devkitPro\tools\bin\picasso.exe",
                      "/opt/devkitpro/tools/bin/picasso.exe"]
MAX_UNIFORMS = 96
MAX_OUTPUTS = 7
MAX_INPUTS = 16
BONES = 4
SAMPLES = 256
TOLERANCE = 1e-4
SWZ = "xyzw"

#: The three representatives: position-only, position+UV1, position+UV0+UV2.
REPRESENTATIVES = ["skin.vs.f8ffaecd", "skin.vs.20f4db07", "skin.vs.60dbd11a"]

SUPPORTED = {"mov", "mul", "mad", "frc", "mova", "dp4"}
INPUT_USAGE = {0: "position", 1: "blendweight", 2: "blendindices",
               3: "normal", 5: "texcoord", 10: "color"}
OUTPUT_NAME = {0: "outpos"}          # usage 0
TEX_OUTPUT = {0: "outtc0", 1: "outtc1", 2: "outtc2"}


class Refusal(Exception):
    pass


# ---------------------------------------------------------------------------
# Plan / IR loading


def load_plans() -> dict:
    with open(PLANS, encoding="utf-8") as handle:
        return {entry["id"]: entry for entry in json.load(handle)["entries"]}


def find_data(explicit: str = "") -> str:
    if explicit:
        return explicit
    for candidate in DATA_CANDIDATES:
        if os.path.exists(os.path.join(candidate, ARCHIVE)):
            return candidate
    raise Refusal("retail_archive_absent")


_packages = None


def _load_packages(data_dir: str):
    global _packages
    if _packages is None:
        import fxp_compile as F
        _packages = F._load_packages(F.find_data(data_dir))
    return _packages


def find_ir(sha256: str, data_dir: str = "") -> C.IR:
    for package in _load_packages(find_data(data_dir)):
        for chunk in package.chunks:
            if chunk.kind != "vs":
                continue
            ir = C.build_ir(chunk.code)
            if ir.sha256 == sha256:
                return ir
    raise Refusal("bytecode_for_%s_not_found" % sha256[:12])


# ---------------------------------------------------------------------------
# Emitter


def _swz(swizzle: str) -> str:
    return "" if swizzle == "xyzw" else "." + swizzle


def _mask(mask: int) -> str:
    return "" if mask == 0xF else "." + "".join(
        SWZ[i] for i in range(4) if mask & (1 << i))


def _out_map(plan: dict) -> Dict[Tuple[int, int], str]:
    out = {}
    for item in plan["outputs"]:
        if item["usage"] == 0:
            out[(item["type"], item["reg_index"])] = "outpos"
        elif item["usage"] == 5:
            name = TEX_OUTPUT.get(item["usage_index"])
            if name is None:
                raise Refusal("output_texcoord%d_unmapped" % item["usage_index"])
            out[(item["type"], item["reg_index"])] = name
        elif item["usage"] == 10 and item["usage_index"] == 0:
            out[(item["type"], item["reg_index"])] = "outclr"
        else:
            raise Refusal("output_%s%d_unmapped"
                          % (item["usage_name"], item["usage_index"]))
    return out


def _d3d_const_rows(ir: C.IR, bones: int) -> Tuple[Dict[int, int], int]:
    relative = sorted({s.index for ins in ir.body for s in ins.srcs
                       if s.reg_type == asm.REG_CONST and s.relative})
    if relative and relative != [0, 1, 2]:
        raise Refusal("relative_constant_set_%s_unsupported" % relative)
    absolute = sorted({s.index for ins in ir.body for s in ins.srcs
                       if s.reg_type == asm.REG_CONST
                       and not s.relative and s.index not in ir.defs})
    rows = {index: 3 * bones + i for i, index in enumerate(absolute)}
    total = 3 * bones + len(absolute)
    if total > MAX_UNIFORMS:
        raise Refusal("uniform_rows_%d_over_%d" % (total, MAX_UNIFORMS))
    return rows, total


def emit(plan: dict, ir: C.IR, bones: int = BONES) -> str:
    """Return picasso assembly, or raise Refusal."""
    if plan.get("kind") != "pica_vs":
        raise Refusal("plan_kind_%s_not_pica_vs" % plan.get("kind"))
    if len(plan.get("outputs", [])) > MAX_OUTPUTS:
        raise Refusal("too_many_outputs_%d" % len(plan["outputs"]))
    if len(plan.get("inputs", [])) > MAX_INPUTS:
        raise Refusal("too_many_inputs_%d" % len(plan["inputs"]))
    for item in plan.get("inputs", []):
        if item["usage"] not in INPUT_USAGE:
            raise Refusal("input_%s_unsupported" % item["usage_name"])
    if ir is None or ir.sha256 != plan["sha256"]:
        raise Refusal("ir_hash_mismatch")
    out_map = _out_map(plan)
    abs_rows, uniform_rows = _d3d_const_rows(ir, bones)
    if uniform_rows > MAX_UNIFORMS:
        raise Refusal("uniform_rows_over_96")
    if plan.get("instructions", 0) > C.VS_MAX_INSTRUCTIONS:
        raise Refusal("too_many_instructions")

    defs = {ins.dst.index: tuple(ins.literal) for ins in ir.body
            if ins.opcode == 0x51 and ins.dst is not None}
    used = {ins.dst.index for ins in ir.body if ins.dst is not None
            and ins.dst.reg_type == asm.REG_TEMP}
    used |= {s.index for ins in ir.body for s in ins.srcs
             if s.reg_type == asm.REG_TEMP}
    scratch = next((("r%d" % i) for i in range(15, -1, -1)
                    if i not in used), None)
    if scratch is None:
        raise Refusal("no_free_temporary")
    scratch2 = next((("r%d" % i) for i in range(14, -1, -1)
                     if i not in used and ("r%d" % i) != scratch), None)

    lines = [
        "; Generated by source/shader_packs/skinning/pica_vs_emit.py",
        "; plan %s  sha256 %s" % (plan["id"], plan["sha256"]),
        "; %d-bone palette: c0..c%d, WVP at c%d..c%d" %
        (bones, 3 * bones - 1, 3 * bones, uniform_rows - 1),
        "; Straight-line PICA VS: no loops, branches, texld or indexed temps.",
    ]
    for index in sorted(defs):
        lines.append(".constf d%d(%s)" % (
            index, ", ".join("%.9g" % v for v in defs[index])))
    for item in plan["outputs"]:
        name = out_map[(item["type"], item["reg_index"])]
        if item["usage"] == 0:
            semantic = "position"
        elif item["usage"] == 5:
            semantic = "texcoord%d" % item["usage_index"]
        else:
            semantic = "color"
        lines.append(".out %s %s" % (name, semantic))
    lines.append("")
    lines.append(".proc main")

    def operand(src) -> Tuple[str, str]:
        if src.modifier not in (0, 1):
            raise Refusal("source_modifier_%d_unsupported" % src.modifier)
        if src.reg_type == asm.REG_TEMP:
            text, kind = "r%d" % src.index, "reg"
        elif src.reg_type == asm.REG_INPUT:
            text, kind = "v%d" % src.index, "reg"
        elif src.reg_type == asm.REG_CONST:
            if src.index in defs:
                text, kind = "d%d" % src.index, "const"
            elif src.relative:
                if src.index not in (0, 1, 2):
                    raise Refusal("relative_c%d_unsupported" % src.index)
                text, kind = "c%d[a0.x]" % src.index, "const"
            else:
                row = abs_rows.get(src.index)
                if row is None:
                    raise Refusal("constant_c%d_unmapped" % src.index)
                text, kind = "c%d" % row, "const"
        else:
            raise Refusal("source_type_%d_unsupported" % src.reg_type)
        text += _swz(src.swizzle)
        if src.modifier == 1:
            text = "-" + text
        return text, kind

    def negate(text: str) -> str:
        return text[1:] if text.startswith("-") else "-" + text

    def dst_name(dst) -> str:
        if dst is None or not dst.mask:
            raise Refusal("instruction_without_destination")
        if dst.modifier not in (0, 1, 2):
            raise Refusal("destination_modifier_%d_unsupported" % dst.modifier)
        if dst.reg_type == asm.REG_TEMP:
            base = "r%d" % dst.index
        elif dst.reg_type == asm.REG_ADDR:
            base = "a0"
        elif dst.reg_type in (asm.REG_OUTPUT, asm.REG_RASTOUT,
                              asm.REG_ATTROUT, asm.REG_COLOROUT):
            base = out_map.get((dst.reg_type, dst.index))
            if base is None:
                raise Refusal("output_%s%d_unmapped"
                              % (dst.type_name, dst.index))
        else:
            raise Refusal("destination_type_%d_unsupported" % dst.reg_type)
        return base + _mask(dst.mask)

    def binary(op: str, dst: str, a, b) -> None:
        if b[1] != "reg":
            if a[1] == "reg":
                a, b = b, a
            else:
                lines.append("    mov %s, %s" % (scratch, b[0]))
                b = (scratch, "reg")
        lines.append("    %s %s, %s, %s" % (op, dst, a[0], b[0]))

    body = [ins for ins in ir.body if ins.name not in ("def", "nop")]

    def same_src(a, b) -> bool:
        return (a.reg_type, a.index, a.swizzle, a.modifier) == (
            b.reg_type, b.index, b.swizzle, b.modifier)

    index = 0
    while index < len(body):
        ins = body[index]
        if ins.name not in SUPPORTED:
            raise Refusal("unsupported_opcode_%s" % ins.name)
        # The retail index idiom is mul x=v*c; frc f=frac(x); mad a=v*c-f.
        # That is exactly floor(v*c), and PICA VS has flr but no frc.
        if (ins.name == "mul" and index + 2 < len(body)
                and body[index + 1].name == "frc"
                and body[index + 2].name == "mad"):
            mul, frc, mad = ins, body[index + 1], body[index + 2]
            mul_ops = [s for s in mul.srcs if s.reg_type != asm.REG_ADDR]
            mad_ops = [s for s in mad.srcs if s.reg_type != asm.REG_ADDR]
            if (mul.dst.reg_type == asm.REG_TEMP
                    and frc.dst.reg_type == asm.REG_TEMP
                    and mul.dst.index == frc.dst.index == mad.dst.index
                    and frc.srcs[0].reg_type == asm.REG_TEMP
                    and frc.srcs[0].index == mul.dst.index
                    and mad.dst.reg_type == asm.REG_TEMP
                    and mad_ops[2].reg_type == asm.REG_TEMP
                    and mad_ops[2].index == mul.dst.index
                    and mad_ops[2].modifier == 1
                    and ((same_src(mul_ops[0], mad_ops[0])
                          and same_src(mul_ops[1], mad_ops[1]))
                         or (same_src(mul_ops[0], mad_ops[1])
                             and same_src(mul_ops[1], mad_ops[0])))):
                dst = dst_name(mul.dst)
                a, b = operand(mul_ops[0]), operand(mul_ops[1])
                binary("mul", dst, a, b)
                lines.append("    flr %s, %s" % (dst, dst))
                index += 3
                continue
        if ins.name == "frc":
            raise Refusal("frc_outside_index_floor_idiom")
        dst = dst_name(ins.dst)
        # asm.py counts the relative-address token as a second source (known
        # phase-1 gap).  It is not an operand: drop REG_ADDR pseudo-sources.
        ops = [operand(src) for src in ins.srcs
               if src.reg_type != asm.REG_ADDR]
        if ins.name == "mov":
            lines.append("    mov %s, %s" % (dst, ops[0][0]))
        elif ins.name == "mul":
            binary("mul", dst, ops[0], ops[1])
        elif ins.name == "mova":
            lines.append("    mova a0, %s" % ops[0][0])
        elif ins.name == "dp4":
            a, b = ops[0], ops[1]
            if b[1] != "reg":
                if a[1] == "reg":
                    a, b = b, a
                else:
                    lines.append("    mov %s, %s" % (scratch, b[0]))
                    b = (scratch, "reg")
            lines.append("    dp4 %s, %s, %s" % (dst, a[0], b[0]))
        elif ins.name == "mad":
            a, b, c = ops[0], ops[1], ops[2]
            # picasso mad: source1 (a) must be a register; source2 (b) may be
            # a constant; source3 (c) must be a register.
            if a[1] != "reg":
                if b[1] == "reg":
                    a, b = b, a
                else:
                    lines.append("    mov %s, %s" % (scratch2, a[0]))
                    a = (scratch2, "reg")
            # PICA allows no dynamic mad: an indexed uniform (cN[a0.x]) cannot
            # be a mad source.  Lower to mul through a scratch, then add.
            if "[a0." in a[0] or "[a0." in b[0]:
                if scratch2 is None:
                    raise Refusal("no_second_temporary")
                binary("mul", scratch2, a, b)
                if c[1] != "reg":
                    lines.append("    mov %s, %s" % (scratch2, c[0]))
                    c = (scratch2, "reg")
                lines.append("    add %s, %s, %s" % (dst, scratch2, c[0]))
            else:
                if c[1] != "reg":
                    lines.append("    mov %s, %s" % (scratch, c[0]))
                    c = (scratch, "reg")
                lines.append("    mad %s, %s, %s, %s" % (dst, a[0], b[0], c[0]))
        index += 1
    lines.append("    end")
    lines.append(".end")
    lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Reference: D3D vs_3_0 IR interpreter


def _read(reg_type: int, index: int, relative: bool, a0, inputs, temps,
          consts, defs):
    if reg_type == asm.REG_TEMP:
        return list(temps.get(index, [0.0] * 4))
    if reg_type == asm.REG_INPUT:
        return list(inputs[index])
    if reg_type == asm.REG_CONST:
        if index in defs:
            return list(defs[index])
        row = index + (int(a0[0]) if relative else 0)
        return list(consts.get(row, [0.0] * 4))
    raise Refusal("reference_source_type_%d" % reg_type)


def _sample_read(vec, swizzle):
    return [vec[SWZ.index(ch)] for ch in swizzle]


def interpret_ir(ir: C.IR, inputs: dict, consts: dict,
                 defs: dict) -> dict:
    temps, a0, out = {}, [0.0] * 4, {}
    for ins in ir.body:
        if ins.name in ("def", "nop"):
            continue
        ops = []
        for src in ins.srcs:
            if src.reg_type == asm.REG_ADDR:
                continue  # relative-address token, not an operand
            vec = _read(src.reg_type, src.index, src.relative, a0, inputs,
                        temps, consts, defs)
            vec = _sample_read(vec, src.swizzle)
            if src.modifier == 1:
                vec = [-v for v in vec]
            elif src.modifier != 0:
                raise Refusal("reference_source_modifier_%d" % src.modifier)
            ops.append(vec)
        if ins.name == "mov":
            result = ops[0]
        elif ins.name == "mul":
            result = [ops[0][i] * ops[1][i] for i in range(4)]
        elif ins.name == "mad":
            result = [ops[0][i] * ops[1][i] + ops[2][i] for i in range(4)]
        elif ins.name == "frc":
            result = [v - math.floor(v) for v in ops[0]]
        elif ins.name == "mova":
            value = float(int(round(ops[0][0])))
            a0 = [value] * 4
            if ins.dst is not None and ins.dst.reg_type == asm.REG_ADDR:
                continue
            result = [value] * 4
        elif ins.name == "dp4":
            dot = sum(ops[0][i] * ops[1][i] for i in range(4))
            result = [dot] * 4
        else:
            raise Refusal("reference_opcode_%s" % ins.name)
        dst = ins.dst
        if dst.reg_type == asm.REG_TEMP:
            vec = temps.setdefault(dst.index, [0.0] * 4)
        elif dst.reg_type in (asm.REG_OUTPUT, asm.REG_RASTOUT,
                              asm.REG_ATTROUT, asm.REG_COLOROUT):
            vec = out.setdefault(dst.index, [0.0] * 4)
        elif dst.reg_type == asm.REG_ADDR:
            continue
        else:
            raise Refusal("reference_destination_type_%d" % dst.reg_type)
        for ch in range(4):
            if dst.mask & (1 << ch):
                vec[ch] = result[ch]
    return out


# ---------------------------------------------------------------------------
# Emitted-code interpreter


CONSTF = re.compile(r"^\.constf\s+(\w+)\(([^)]*)\)$")
RELOFF = re.compile(r"^([cdv])(\d+)\[a0\.([xyzw])\]$")


def _parse_operand(text: str, env: dict):
    text = text.strip()
    negate = text.startswith("-")
    if negate:
        text = text[1:]
    swizzle = "xyzw"
    match = re.match(r"^(.*)\.([xyzw]{1,4})$", text)
    if match and not RELOFF.match(text):
        text, swizzle = match.group(1), match.group(2)
        if len(swizzle) < 4:
            swizzle = swizzle + swizzle[-1] * (4 - len(swizzle))
    rel = RELOFF.match(text)
    if rel:
        kind, index, comp = rel.group(1), int(rel.group(2)), rel.group(3)
        if kind == "c":
            row = index + int(env["a0"][SWZ.index(comp)])
            vec = list(env["consts"].get(row, [0.0] * 4))
        else:
            raise Refusal("relative_%s_unsupported" % kind)
    elif text.startswith("d") and text[1:].isdigit():
        vec = list(env["constf"][text])
    elif text.startswith("c") and text[1:].isdigit():
        vec = list(env["consts"].get(int(text[1:]), [0.0] * 4))
    elif text.startswith("r") and text[1:].isdigit():
        vec = list(env["temps"].get(int(text[1:]), [0.0] * 4))
    elif text.startswith("v") and text[1:].isdigit():
        vec = list(env["inputs"][int(text[1:])])
    elif text == "a0":
        vec = list(env["a0"])
    else:
        raise Refusal("operand_%r_unsupported" % text)
    vec = _sample_read(vec, swizzle)
    return [-v for v in vec] if negate else vec


def _write(dst: str, vec, env: dict) -> None:
    match = re.match(r"^(.*?)\.([xyzw]{1,4})$", dst)
    mask = "xyzw"
    if match:
        dst, mask = match.group(1), match.group(2)
    if dst == "a0":
        value = float(int(round(vec[0])))
        env["a0"] = [value] * 4
        return
    if dst.startswith("r") and dst[1:].isdigit():
        target = env["temps"].setdefault(int(dst[1:]), [0.0] * 4)
    elif dst.startswith("out"):
        target = env["outs"].setdefault(dst, [0.0] * 4)
    else:
        raise Refusal("destination_%r_unsupported" % dst)
    # A destination mask selects whole components; it is not a source swizzle.
    for ch in mask:
        comp = SWZ.index(ch)
        target[comp] = vec[comp]


def interpret_pica(text: str, inputs: dict, consts: dict,
                   constf: dict) -> dict:
    env = {"temps": {}, "a0": [0.0] * 4, "inputs": inputs,
           "consts": consts, "constf": {}, "outs": {}}
    for name, values in constf.items():
        env["constf"][name] = values
    body = False
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith(".constf"):
            match = CONSTF.match(line)
            if not match:
                raise Refusal("bad_constf_%r" % line)
            env["constf"][match.group(1)] = [
                float(v) for v in match.group(2).split(",")]
            continue
        if line == ".proc main":
            body = True
            continue
        if line in ("end", ".end") or not body or not line or line.startswith(";"):
            continue
        if line.startswith("."):
            continue
        parts = [part.strip() for part in line.split(",")]
        op, dst = parts[0].split(None, 1)
        dst = dst.strip()
        ops = [_parse_operand(part, env) for part in parts[1:]]
        if op == "mov":
            result = ops[0]
        elif op == "mul":
            result = [ops[0][i] * ops[1][i] for i in range(4)]
        elif op == "add":
            result = [ops[0][i] + ops[1][i] for i in range(4)]
        elif op == "mad":
            result = [ops[0][i] * ops[1][i] + ops[2][i] for i in range(4)]
        elif op == "frc":
            result = [v - math.floor(v) for v in ops[0]]
        elif op == "flr":
            result = [math.floor(v) for v in ops[0]]
        elif op == "mova":
            _write("a0", ops[0], env)
            continue
        elif op == "dp4":
            dot = sum(ops[0][i] * ops[1][i] for i in range(4))
            result = [dot] * 4
        else:
            raise Refusal("pica_opcode_%s" % op)
        _write(dst, result, env)
    return env["outs"]


# ---------------------------------------------------------------------------
# Inputs, constants, comparison


def build_sample(rng, plan: dict, bones: int):
    inputs = {}
    for item in plan["inputs"]:
        usage, reg = item["usage"], item["reg_index"]
        if usage == 0:
            inputs[reg] = [rng.uniform(-5.0, 5.0) for _ in range(3)] + [1.0]
        elif usage == 1:
            inputs[reg] = [rng.random() for _ in range(4)]
        elif usage == 2:
            index = rng.randrange(bones)
            inputs[reg] = [index / 255.0] * 4
        elif usage == 3:
            vec = [rng.uniform(-1.0, 1.0) for _ in range(3)]
            length = math.sqrt(sum(v * v for v in vec)) or 1.0
            inputs[reg] = [v / length for v in vec] + [0.0]
        elif usage in (5, 10):
            inputs[reg] = [rng.random() for _ in range(4)]
        else:
            raise Refusal("sample_input_%s" % item["usage_name"])
    return inputs


def build_constants(rng, bones: int, defs: dict):
    d3d = {i: [0.0] * 4 for i in range(256)}
    pica = {}
    for bone in range(bones):
        rows = [[rng.uniform(-1.5, 1.5) for _ in range(4)]
                for _ in range(3)]
        rows[2][3] += rng.uniform(-3.0, 3.0)  # translation in the row w
        for r in range(3):
            pica[3 * bone + r] = list(rows[r])
            d3d[3 * bone + r] = list(rows[r])
    wvp = [[1.2, 0.0, 0.0, 0.0], [0.0, 0.9, 0.0, 0.0],
           [0.0, 0.0, 0.5, 0.0], [0.0, 0.0, -5.0, 1.0]]
    for i in range(4):
        row = [wvp[i][j] + rng.uniform(-0.05, 0.05) for j in range(4)]
        d3d[180 + i] = list(row)
        pica[3 * bones + i] = list(row)
    for reg, values in defs.items():
        d3d[reg] = list(values)
    return d3d, pica


def _out_name(item: dict) -> str:
    if item["usage"] == 0:
        return "outpos"
    if item["usage"] == 5:
        return TEX_OUTPUT[item["usage_index"]]
    if item["usage"] == 10 and item["usage_index"] == 0:
        return "outclr"
    raise Refusal("compare_output_%s" % item["usage_name"])


def verify(plan: dict, ir: C.IR, samples: int = SAMPLES,
           bones: int = BONES, compile_asm: bool = True) -> dict:
    text = emit(plan, ir, bones)
    defs = {ins.dst.index: tuple(ins.literal) for ins in ir.body
            if ins.opcode == 0x51 and ins.dst is not None}
    rng = random.Random(0x5C1A + int(plan["sha256"][:8], 16))
    max_position = 0.0
    max_varying = 0.0
    for _ in range(samples):
        inputs = build_sample(rng, plan, bones)
        d3d, pica = build_constants(rng, bones, defs)
        ref = interpret_ir(ir, inputs, d3d, defs)
        emitted = interpret_pica(text, inputs, pica, {
            "d%d" % reg: list(values) for reg, values in defs.items()})
        for item in plan["outputs"]:
            name = _out_name(item)
            a = ref.get(item["reg_index"], [0.0] * 4)
            b = emitted.get(name, [0.0] * 4)
            error = max(abs(a[i] - b[i]) for i in range(4))
            if item["usage"] == 0:
                max_position = max(max_position, error)
            else:
                max_varying = max(max_varying, error)
    picasso = "skipped"
    shbin_bytes = 0
    if compile_asm:
        picasso, shbin_bytes = run_picasso(text)
    ok = (max_position <= TOLERANCE and max_varying <= TOLERANCE
          and picasso in ("pass", "skipped"))
    return {
        "id": plan["id"], "sha256": plan["sha256"],
        "asm": "%s.v.pica" % plan["id"],
        "bones": bones, "uniform_rows": _d3d_const_rows(ir, bones)[1],
        "instructions": len([line for line in text.splitlines()
                             if line.startswith("    ") and
                             not line.strip().startswith(";")]),
        "outputs": len(plan["outputs"]), "inputs": len(plan["inputs"]),
        "samples": samples, "tolerance": TOLERANCE,
        "max_abs_position": max_position,
        "max_abs_varying": max_varying,
        "picasso": picasso, "shbin_bytes": shbin_bytes,
        "status": "pass" if ok else "fail",
    }


def run_picasso(text: str) -> Tuple[str, int]:
    exe = os.environ.get("PICASSO")
    if not exe:
        exe = next((p for p in PICASSO_CANDIDATES if os.path.exists(p)), None)
    if not exe:
        return "skipped", 0
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "shader.v.pica")
        out = os.path.join(tmp, "shader.shbin")
        with open(src, "w", encoding="ascii", newline="\n") as handle:
            handle.write(text)
        proc = subprocess.run([exe, "-o", out, src],
                              capture_output=True, text=True)
        if proc.returncode != 0 or not os.path.exists(out):
            return "fail: %s" % (proc.stderr.strip() or proc.stdout.strip()), 0
        return "pass", os.path.getsize(out)


# ---------------------------------------------------------------------------
# CLI


def emit_all(out_dir: str = GENERATED, data_dir: str = "",
             samples: int = SAMPLES) -> dict:
    plans = load_plans()
    os.makedirs(out_dir, exist_ok=True)
    entries = []
    for plan_id in REPRESENTATIVES:
        plan = plans.get(plan_id)
        if plan is None:
            raise Refusal("representative_%s_missing" % plan_id)
        ir = find_ir(plan["sha256"], data_dir)
        text = emit(plan, ir)
        path = os.path.join(out_dir, "%s.v.pica" % plan_id)
        with open(path, "w", encoding="ascii", newline="\n") as handle:
            handle.write(text)
        entries.append(verify(plan, ir, samples))
    report = {
        "schema": "skinning-pica-vs-emitter/1",
        "samples": samples, "tolerance": TOLERANCE,
        "status": "pass" if all(e["status"] == "pass" for e in entries)
                  else "fail",
        "entries": entries,
    }
    with open(REPORT, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=1, sort_keys=True)
        handle.write("\n")
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser("emit-all")
    gen.add_argument("--data", default="")
    gen.add_argument("--out", default=GENERATED)
    gen.add_argument("--samples", type=int, default=SAMPLES)
    ver = sub.add_parser("verify")
    ver.add_argument("--data", default="")
    ver.add_argument("--samples", type=int, default=SAMPLES)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "emit-all":
            report = emit_all(args.out, args.data, args.samples)
        else:
            plans = load_plans()
            entries = []
            for plan_id in REPRESENTATIVES:
                plan = plans[plan_id]
                entries.append(verify(plan, find_ir(plan["sha256"], args.data),
                                      args.samples))
            report = {"status": "pass" if all(e["status"] == "pass"
                                              for e in entries) else "fail",
                      "entries": entries}
    except Refusal as exc:
        print("REFUSED: %s" % exc, file=sys.stderr)
        return 2
    for entry in report["entries"]:
        print("%-22s %s  pos=%.3g var=%.3g  picasso=%s  rows=%d  ins=%d"
              % (entry["id"], entry["status"], entry["max_abs_position"],
                 entry["max_abs_varying"], entry["picasso"],
                 entry["uniform_rows"], entry["instructions"]))
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
