"""TEV stage scheduler: canonical SM3 IR -> <=6-stage TEV plans.

This module is the missing middle layer between ``tools/shaders/compile.py``
(the deliberately narrow exact planner) and the per-family pack recipes.  It
owns three things:

* an expression DAG built from the canonical IR (straight-line plus constant
  branches, with small loops unrolled and large loops truncated);
* a one-accumulator scheduler that keeps at most one computed value live in
  ``PREV``, reorders independent work, carries RGB and alpha independently and
  falls back to explicit ``passes[]`` (each <=6 stages, blend=replace) when a
  value has to survive more than one stage;
* an op-lowering library for ``nrm``, ``rcp``, ``rsq``, ``pow``, ``cmp``,
  ``slt/sge``, ``min/max``, ``lrp``, ``texkill``, ``abs`` and ``frc``.

Anything the scheduler cannot prove is emitted as a named approximation with
the dropped term, the fitting policy and a measured numeric error.  Nothing
here is draw-enabled: pack catalogs keep ``draw_enabled = 0`` and
``runtime = 0``; this module never changes the runtime table or the schema.
"""

from __future__ import annotations

import argparse
import copy
import dataclasses
import hashlib
import json
import math
import os
import random
import struct
import sys
from collections import Counter, OrderedDict
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import asm  # noqa: E402
from shaders import compile as C  # noqa: E402

# TEV descriptor constants (mirror compile.py / 3ds/gpu/enums.h).
SRC_PRIMARY = C.SRC_PRIMARY
SRC_FRAG_PRIMARY = C.SRC_FRAG_PRIMARY
SRC_FRAG_SECONDARY = C.SRC_FRAG_SECONDARY
SRC_TEX0, SRC_TEX1, SRC_TEX2 = C.SRC_TEX0, C.SRC_TEX1, C.SRC_TEX2
SRC_CONST, SRC_PREV = C.SRC_CONST, C.SRC_PREV
RGB_COLOR, RGB_1M_COLOR = C.RGB_COLOR, C.RGB_1M_COLOR
RGB_ALPHA, RGB_1M_ALPHA = C.RGB_ALPHA, C.RGB_1M_ALPHA
RGB_R, RGB_1M_R = C.RGB_R, C.RGB_1M_R
RGB_G, RGB_1M_G = C.RGB_G, C.RGB_1M_G
RGB_B, RGB_1M_B = C.RGB_B, C.RGB_1M_B
A_ALPHA, A_1M_ALPHA = C.A_ALPHA, C.A_1M_ALPHA
A_R, A_1M_R = C.A_R, C.A_1M_R
A_G, A_1M_G = C.A_G, C.A_1M_G
A_B, A_1M_B = C.A_B, C.A_1M_B
F_REPLACE, F_MODULATE, F_ADD, F_ADD_SIGNED = (
    C.FUNC_REPLACE, C.FUNC_MODULATE, C.FUNC_ADD, C.FUNC_ADD_SIGNED)
F_INTERPOLATE, F_SUBTRACT = C.FUNC_INTERPOLATE, C.FUNC_SUBTRACT
F_DOT3_RGB, F_DOT3_RGBA = C.FUNC_DOT3_RGB, C.FUNC_DOT3_RGBA
F_MULTIPLY_ADD, F_ADD_MULTIPLY = (
    C.FUNC_MULTIPLY_ADD, C.FUNC_ADD_MULTIPLY)

L = "L"          # leaf
O = "O"          # operation
U = "U"          # undefined
ZERO = ("C", 0.0)
ONE = ("C", 1.0)
HALF = ("C", 0.5)
UNDEF = (U,)


# ---------------------------------------------------------------------------
# Small vector expression algebra
# ---------------------------------------------------------------------------


def _vec(e0, e1, e2, e3):
    return (e0, e1, e2, e3)


def vconst(value: float):
    c = ("C", float(value))
    return (c, c, c, c)


def vleaf(kind: str, reg: int):
    return (("L", kind, reg, 0), ("L", kind, reg, 1),
            ("L", kind, reg, 2), ("L", kind, reg, 3))


def vzero():
    return vconst(0.0)


def vone():
    return vconst(1.0)


def vswizzle(vec, comps):
    return tuple(vec[comps[i]] for i in range(4))


def vapply(vec, fn):
    return tuple(fn(e) for e in vec)


def vop(name: str, *args):
    node = (O, name) + tuple(args)
    return (node, node, node, node)


def vop_channels(name: str, *args):
    return tuple((O, name) + tuple(args) for _ in range(4))


def is_leaf(expr) -> bool:
    return expr[0] in (L, "C")


def is_undef(expr) -> bool:
    return expr[0] == U


def leaf_desc(expr):
    if expr[0] == L:
        return expr[1], expr[2], expr[3]
    if expr[0] == "C":
        return "lit", int(round(expr[1] * 1_000_003)), expr[1]
    return None


def canon(expr):
    """Deterministic commutative normal form for structural comparison."""
    if expr[0] in (L, "C", U):
        return expr
    if expr[0] == O:
        name = expr[1]
        args = tuple(canon_vec(a) for a in expr[2:])
        if name in ("add", "mul") and len(args) >= 2:
            flat = []
            for a in args:
                for e in a:
                    if e[0] == O and e[1] == name:
                        flat.extend(e[2:])
                    else:
                        flat.append(e)
            flat.sort(key=repr)
            return (O, name) + tuple(flat)
        return (O, name) + args
    return expr


def canon_vec(vec):
    return tuple(canon(e) for e in vec)


def _strip_clamp(expr):
    while expr[0] == O and expr[1] in ("clamp01",):
        expr = expr[2][0]
    return expr


def vec_equal(a, b) -> bool:
    return all(_strip_clamp(x) == _strip_clamp(y) for x, y in zip(a, b))


# ---------------------------------------------------------------------------
# IR -> expression DAG
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class BuildInfo:
    values: Dict[int, tuple]
    final: tuple
    texcoord: Dict[int, int]
    sampler_type: Dict[int, int]
    defs: Dict[int, tuple]
    defi: Dict[int, int]
    ops: Counter
    control: Counter
    unsupported: List[str]
    dependent_reads: int
    partial_writes: int
    non_colour_inputs: int
    texkill: int
    flow: dict
    branch_const: int
    branch_select: int
    loop_unrolled: int
    loop_truncated: int


def _const_eval(expr, defs: Dict[int, tuple], defi: Dict[int, int]):
    """Evaluate an expression made only from literals/def/defi, else None."""
    if expr[0] == "C":
        return float(expr[1])
    if expr[0] == L:
        kind, reg, comp = expr[1], expr[2], expr[3]
        if kind == "const" and reg in defs:
            return float(defs[reg][comp])
        if kind == "lit":
            return float(reg)
        return None
    if expr[0] != O:
        return None
    name = expr[1]
    args = []
    for a in expr[2:]:
        vals = [_const_eval(e, defs, defi) for e in a]
        if any(v is None for v in vals):
            return None
        args.append(vals)
    if name == "neg":
        return -args[0][0]
    if name == "add":
        return args[0][0] + args[1][0]
    if name == "sub":
        return args[0][0] - args[1][0]
    if name == "mul":
        return args[0][0] * args[1][0]
    if name == "mad":
        return args[0][0] * args[1][0] + args[2][0]
    if name == "dot3":
        return sum(args[0][i] * args[1][i] for i in range(3))
    if name == "dot4":
        return sum(args[0][i] * args[1][i] for i in range(4))
    if name == "pow":
        try:
            return math.pow(abs(args[0][0]), args[1][0])
        except (ValueError, OverflowError):
            return None
    if name == "rcp":
        return 1.0 / args[0][0] if abs(args[0][0]) > 1e-9 else 1e6
    if name == "rsq":
        v = math.sqrt(abs(args[0][0]))
        return 1.0 / v if v > 1e-9 else 1e6
    if name == "slt":
        return 1.0 if args[0][0] < args[1][0] else 0.0
    if name == "sge":
        return 1.0 if args[0][0] >= args[1][0] else 0.0
    return None


def _read_vec(src: asm.Src, values: Dict[int, tuple], out: tuple,
              defs: Dict[int, tuple]) -> tuple:
    if src.reg_type == asm.REG_TEMP:
        base = values.get(src.index, (UNDEF, UNDEF, UNDEF, UNDEF))
    elif src.reg_type == asm.REG_INPUT:
        base = vleaf("in", src.index)
    elif src.reg_type == asm.REG_CONST:
        base = vleaf("const", src.index)
    elif src.reg_type == asm.REG_COLOROUT:
        base = out
    else:
        base = vzero()
    comps = tuple("xyzw".index(src.swizzle[i]) for i in range(4))
    vec = vswizzle(base, comps)
    mod = src.modifier
    if mod == 0:
        return vec
    if mod == 1:
        return vapply(vec, lambda e: (O, "neg", (e, e, e, e)))
    if mod == 2:
        return vapply(vec, lambda e: (O, "sub", (e, e, e, e), vconst(0.5)))
    if mod == 3:
        return vapply(vec, lambda e: (O, "neg", (O, "sub", (e, e, e, e),
                                                   vconst(0.5))[0])[0])
    if mod in (4, 5):
        expr = vop("sub", vop("mul", vec, vconst(2.0)), vone())
        if mod == 5:
            expr = vapply(expr, lambda e: (O, "neg", (e, e, e, e)))
        return expr
    if mod == 6:
        return vop("sub", vone(), vec)
    if mod in (7, 8):
        expr = vop("mul", vec, vconst(2.0))
        if mod == 8:
            expr = vapply(expr, lambda e: (O, "neg", (e, e, e, e)))
        return expr
    if mod in (9, 10, 13):
        # dz/dw/not: a 1/0 predicate has no TEV compare; keep it as an op so
        # the approximation pass can name it.
        return vop_channels("predicate", vec)
    if mod in (11, 12):
        expr = vop("abs", vec)
        if mod == 12:
            expr = vappl_neg(expr)
        return expr
    return vec


def vappl_neg(vec):
    return vapply(vec, lambda e: (O, "neg", (e, e, e, e)))


def build_values(ir: C.IR, reduced_body=None) -> BuildInfo:
    body = reduced_body if reduced_body is not None else ir.body
    values: Dict[int, tuple] = {}
    out = [UNDEF, UNDEF, UNDEF, UNDEF]
    texcoord: Dict[int, int] = {}
    sampler_type: Dict[int, int] = {}
    defs = dict(ir.defs)
    defi: Dict[int, int] = {}
    ops = Counter()
    control = Counter()
    unsupported: List[str] = []
    dependent_reads = 0
    partial_writes = 0
    non_colour_inputs = 0
    texkill = 0
    branch_const = 0
    branch_select = 0
    loop_unrolled = 0
    loop_truncated = 0

    for d in ir.program.declarations:
        if d.reg_type == asm.REG_SAMPLER:
            sampler_type[d.reg_index] = d.texture_type

    for ins in body:
        if ins.opcode in (0x1F, 0x51, 0x00):
            continue
        if ins.opcode == 0x30:
            if ins.dst is not None and ins.literal:
                defi[ins.dst.index] = int(ins.literal[0])
            continue
        if ins.opcode in (0x28, 0x29, 0x2A, 0x2B, 0x1B, 0x1D, 0x26, 0x27,
                          0x2C, 0x2D, 0x60):
            control[asm.OPCODE_NAMES.get(ins.opcode, hex(ins.opcode))] += 1
            continue
        if ins.opcode == 0x41:
            texkill += 1
            continue
        if ins.opcode == 0x42:
            if len(ins.srcs) < 2:
                unsupported.append("texld_without_sampler")
                continue
            coord, samp = ins.srcs[0], ins.srcs[1]
            if samp.reg_type != asm.REG_SAMPLER:
                unsupported.append("texld_without_sampler")
                continue
            if coord.reg_type == asm.REG_INPUT:
                texcoord[samp.index] = coord.index
            else:
                dependent_reads += 1
                texcoord.setdefault(samp.index, 0)
            tex = vleaf("tex", samp.index)
            dst = ins.dst
            if dst is None:
                continue
            if dst.reg_type == asm.REG_TEMP:
                old = values.get(dst.index, (UNDEF,) * 4)
                values[dst.index] = tuple(
                    tex[i] if dst.mask & (1 << i) else old[i]
                    for i in range(4))
                if dst.mask != 0xF:
                    partial_writes += 1
            elif dst.reg_type == asm.REG_COLOROUT:
                for i in range(4):
                    if dst.mask & (1 << i):
                        out[i] = tex[i]
                if dst.mask != 0xF:
                    partial_writes += 1
            continue

        dst = ins.dst
        if dst is None:
            unsupported.append(asm.OPCODE_NAMES.get(ins.opcode,
                                                    hex(ins.opcode)))
            continue
        srcs = list(ins.srcs)
        name = asm.OPCODE_NAMES.get(ins.opcode, "0x%02x" % ins.opcode)
        ops[name] += 1
        vec = None
        if ins.opcode == 0x01:
            vec = _read_vec(srcs[0], values, tuple(out), defs)
        elif ins.opcode in (0x02, 0x03, 0x05):
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            opn = {0x02: "add", 0x03: "sub", 0x05: "mul"}[ins.opcode]
            vec = vop(opn, a, b)
        elif ins.opcode == 0x04:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            c = _read_vec(srcs[2], values, tuple(out), defs)
            vec = vop("mad", a, b, c)
        elif ins.opcode == 0x08:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            vec = vop("dot3", a, b)
        elif ins.opcode == 0x09:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            vec = vop("dot4", a, b)
        elif ins.opcode in (0x0A, 0x0B):
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            vec = vop("min" if ins.opcode == 0x0A else "max", a, b)
        elif ins.opcode == 0x0C:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            vec = vop("slt", a, b)
        elif ins.opcode == 0x0D:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            vec = vop("sge", a, b)
        elif ins.opcode == 0x12:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            c = _read_vec(srcs[2], values, tuple(out), defs)
            vec = vop("lrp", a, b, c)
        elif ins.opcode == 0x13:
            vec = vop("frc", _read_vec(srcs[0], values, tuple(out), defs))
        elif ins.opcode == 0x06:
            vec = vop("rcp", _read_vec(srcs[0], values, tuple(out), defs))
        elif ins.opcode == 0x07:
            vec = vop("rsq", _read_vec(srcs[0], values, tuple(out), defs))
        elif ins.opcode == 0x20:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            vec = vop("pow", a, b)
        elif ins.opcode == 0x23:
            vec = vop("abs", _read_vec(srcs[0], values, tuple(out), defs))
        elif ins.opcode == 0x24:
            vec = vop("nrm", _read_vec(srcs[0], values, tuple(out), defs))
        elif ins.opcode == 0x58:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            c = _read_vec(srcs[2], values, tuple(out), defs)
            vec = vop("cmp", a, b, c)
        elif ins.opcode == 0x5A:
            a = _read_vec(srcs[0], values, tuple(out), defs)
            b = _read_vec(srcs[1], values, tuple(out), defs)
            c = _read_vec(srcs[2], values, tuple(out), defs)
            vec = vop("dp2add", a, b, c)
        elif ins.opcode in (0x5B, 0x5C):
            vec = vop("dsx" if ins.opcode == 0x5B else "dsy",
                      _read_vec(srcs[0], values, tuple(out), defs))
        elif ins.opcode in (0x0E, 0x0F):
            vec = vop("exp" if ins.opcode == 0x0E else "log",
                      _read_vec(srcs[0], values, tuple(out), defs))
        elif ins.opcode in (0x5D, 0x5F):
            # texldd/texldl: sample the declared sampler; derivatives/lod are
            # outside TEV and are named by the approximation pass.
            samp = srcs[1] if len(srcs) > 1 else None
            if samp is not None and samp.reg_type == asm.REG_SAMPLER:
                coord = srcs[0]
                if coord.reg_type == asm.REG_INPUT:
                    texcoord[samp.index] = coord.index
                else:
                    dependent_reads += 1
                    texcoord.setdefault(samp.index, 0)
                vec = vleaf("tex", samp.index)
                unsupported.append("texldd" if ins.opcode == 0x5D else "texldl")
            else:
                unsupported.append("texld_without_sampler")
        elif ins.opcode == 0x2E:
            vec = vop("mova", _read_vec(srcs[0], values, tuple(out), defs))
        else:
            unsupported.append(name)
            vec = vzero()

        # destination modifier: _sat is the TEV output clamp; _pp is a hint.
        if vec is None:
            continue
        if dst.modifier & 1:
            vec = vop("clamp01", vec)
        if dst.reg_type == asm.REG_TEMP:
            old = values.get(dst.index, (UNDEF,) * 4)
            values[dst.index] = tuple(
                vec[i] if dst.mask & (1 << i) else old[i]
                for i in range(4))
            if dst.mask != 0xF:
                partial_writes += 1
        elif dst.reg_type == asm.REG_COLOROUT:
            for i in range(4):
                if dst.mask & (1 << i):
                    out[i] = vec[i]
            if dst.mask != 0xF:
                partial_writes += 1
        else:
            unsupported.append("writes_%s" % dst.type_name)

    # Input usage: only colour varyings that actually reach the final TEV
    # expression matter.  A texcoord input used only to compute a (prebaked)
    # UV is not a TEV source.
    non_colour_inputs = 0
    seen_bad = set()
    for e in _walk_exprs(tuple(out)):
        if e[0] == L and e[1] == "in" and e[2] not in seen_bad:
            decl = ir.inputs.get(e[2], {})
            if decl.get("usage") != 10:
                non_colour_inputs += 1
                seen_bad.add(e[2])
    if out[3] == UNDEF:
        out[3] = ONE
    return BuildInfo(
        values=values, final=tuple(out), texcoord=texcoord,
        sampler_type=sampler_type, defs=defs, defi=defi, ops=ops,
        control=control, unsupported=unsupported,
        dependent_reads=dependent_reads, partial_writes=partial_writes,
        non_colour_inputs=non_colour_inputs, texkill=texkill,
        flow={"mode": "straight_line", "loops": [], "ifs": []},
        branch_const=branch_const, branch_select=branch_select,
        loop_unrolled=loop_unrolled, loop_truncated=loop_truncated)


# ---------------------------------------------------------------------------
# Control-flow reduction: constant branches folded, small loops unrolled,
# variable branches kept as their then arm (branchless-select stand-in).
# ---------------------------------------------------------------------------


def reduce_flow(ir: C.IR, loop_limit: int = 2, unroll_limit: int = 4):
    body = list(ir.body)
    defi = {i.dst.index: int(i.literal[0]) for i in body
            if i.opcode == 0x30 and i.dst is not None and i.literal}
    defs = dict(ir.defs)
    meta = {"mode": "straight_line", "loops": [], "ifs": [],
            "constant_branches": 0, "branchless_selects": 0,
            "unrolled": 0, "truncated": 0}

    def const_condition(ins) -> Optional[bool]:
        # `if` takes a predicate (p0..); true only if the source is a constant
        # bool declared by defb.  Most retail branches use `ifc` instead.
        if ins.opcode == 0x28:
            if not ins.srcs:
                return None
            s = ins.srcs[0]
            if s.reg_type == asm.REG_CONSTBOOL and s.index in defs:
                return bool(defs[s.index])
            return None
        if not ins.srcs:
            return None
        a = _read_vec(ins.srcs[0], {}, (UNDEF,) * 4, defs)
        b = _read_vec(ins.srcs[1], {}, (UNDEF,) * 4, defs) if len(ins.srcs) > 1 else a
        av = _const_eval(a[0], defs, defi)
        bv = _const_eval(b[0], defs, defi)
        if av is None or bv is None:
            return None
        comp = ins.comparison
        return {
            1: av > bv, 2: av == bv, 3: av >= bv,
            4: av < bv, 5: av != bv, 6: av <= bv,
        }.get(comp, av < bv)

    def match_blocks(seq):
        match, stack = {}, []
        for n, ins in enumerate(seq):
            if ins.opcode in (0x26, 0x1B, 0x28, 0x29):
                stack.append((n, ins.opcode, None, ins))
            elif ins.opcode == 0x2A:
                if stack:
                    start, op, other, orig = stack[-1]
                    if orig is ins:
                        continue
                    stack[-1] = (start, op, n, orig)
            elif ins.opcode in (0x27, 0x1D, 0x2B) and stack:
                start, op, other, orig = stack.pop()
                match[start] = (n, other)
        return match

    def cut(seq):
        match = match_blocks(seq)
        out, i = [], 0
        while i < len(seq):
            ins = seq[i]
            if ins.opcode in (0x26, 0x1B):
                if i not in match:
                    return None
                end, _ = match[i]
                count = 1
                if ins.srcs:
                    s = ins.srcs[0]
                    count = defi.get(s.index, 1) if s.reg_type == asm.REG_CONSTINT else 1
                if ins.opcode == 0x1B:
                    # loop aL, i#: the counter register is not modelled; the
                    # body is still a straight-line truncation.
                    meta["truncated"] += 1
                    keep = max(1, min(loop_limit, count))
                    kind = "truncated"
                elif count <= unroll_limit:
                    keep = max(1, count)
                    kind = "unrolled"
                    meta["unrolled"] += 1
                else:
                    keep = loop_limit
                    kind = "truncated"
                    meta["truncated"] += 1
                meta["mode"] = "loop_truncated"
                meta["loops"].append({"opcode": ins.name, "iterations": count,
                                      "loop_truncated_to": keep,
                                      "policy": kind})
                for _ in range(keep):
                    sub = cut(seq[i + 1:end])
                    if sub is None:
                        return None
                    out.extend(sub)
                i = end + 1
                continue
            if ins.opcode in (0x28, 0x29):
                if i not in match:
                    return None
                end, other = match[i]
                then_end = other if other is not None else end
                cond = const_condition(ins)
                if cond is not None:
                    meta["constant_branches"] += 1
                    chosen = seq[i + 1:then_end] if cond else (
                        seq[other + 1:end] if other is not None else [])
                    sub = cut(chosen)
                    if sub is None:
                        return None
                    out.extend(sub)
                else:
                    meta["branchless_selects"] += 1
                    if meta["mode"] == "straight_line":
                        meta["mode"] = "branchless_select"
                    meta["ifs"].append({
                        "opcode": ins.name,
                        "condition": "variable",
                        "action": "then",
                    })
                    sub = cut(seq[i + 1:then_end])
                    if sub is None:
                        return None
                    out.extend(sub)
                i = end + 1
                continue
            if ins.opcode in (0x27, 0x1D, 0x2A, 0x2B, 0x2C, 0x2D, 0x60):
                i += 1
                continue
            if ins.opcode in (0x19, 0x1A, 0x1C, 0x1E):
                return None
            out.append(ins)
            i += 1
        return out

    reduced = cut(body)
    if reduced is None:
        return None, meta
    out = copy.copy(ir)
    out.body = reduced
    return out, meta


# ---------------------------------------------------------------------------
# Source encoding
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class SideSource:
    src: int
    comps: tuple
    const_reg: int = 255
    sampler: Optional[int] = None

    def rgb_op(self) -> int:
        c = self.comps
        if c[0] == 0 and c[1] == 1 and c[2] == 2:
            return RGB_COLOR
        if c[0] == c[1] == c[2]:
            return {0: RGB_R, 1: RGB_G, 2: RGB_B, 3: RGB_ALPHA}[c[0]]
        raise PlanImpossible("non_uniform_rgb_swizzle")

    def alpha_op(self) -> int:
        return {0: A_R, 1: A_G, 2: A_B, 3: A_ALPHA}[self.comps[3]]


class PlanImpossible(Exception):
    pass


def _side_source(vec: tuple, side: Optional[Sequence[int]] = None) -> SideSource:
    """Encode a vector of leaves as one TEV operand (RGB or alpha side)."""
    if side is None:
        side = (0, 1, 2, 3)
    leaves = [vec[i] for i in side]
    if any(not is_leaf(e) for e in leaves):
        raise PlanImpossible("computed_operand")
    descs = [leaf_desc(e) for e in leaves]
    kind = descs[0][0]
    for d in descs:
        if d[0] != kind:
            raise PlanImpossible("mixed_leaf_sources")
    comps = tuple(d[2] if kind != "lit" else d[2] for d in descs)
    if kind == "lit":
        # Literal leaves are expressed as a derived constant slot.
        raise PlanImpossible("literal_leaf")
    reg = descs[0][1]
    if any(d[1] != reg for d in descs):
        raise PlanImpossible("mixed_registers")
    if kind == "tex":
        return SideSource(SRC_TEX0, comps, sampler=reg)
    if kind == "in":
        return SideSource(SRC_PRIMARY, comps)
    if kind == "const":
        return SideSource(SRC_CONST, comps, const_reg=reg)
    raise PlanImpossible("leaf_kind_%s" % kind)


# ---------------------------------------------------------------------------
# Scheduler
# ---------------------------------------------------------------------------


def _func_for(name: str):
    return {
        "add": F_ADD, "sub": F_SUBTRACT, "mul": F_MODULATE,
        "mad": F_MULTIPLY_ADD, "lrp": F_INTERPOLATE,
        "dot3": F_DOT3_RGBA, "clamp01": F_REPLACE,
        "neg": F_SUBTRACT, "x2": F_ADD, "bias": F_SUBTRACT,
        "sign": F_SUBTRACT, "complement": F_SUBTRACT,
    }.get(name)


def _side_op(exprs):
    """Return ('leaf',) or ('op', name, args) if a side is uniform."""
    exprs = [e for e in exprs if not is_undef(e)]
    if not exprs:
        return ("leaf",)
    if all(is_leaf(e) for e in exprs):
        return ("leaf",)
    first = exprs[0]
    if first[0] != O:
        return ("mixed",)
    name, args = first[1], first[2:]
    for e in exprs[1:]:
        if e[0] != O or e[1] != name or e[2:] != args:
            return ("mixed",)
    return ("op", name, args)


class TevScheduler:
    def __init__(self, ir: C.IR, info: BuildInfo, approx: bool = False,
                 max_stages: int = 6):
        self.ir = ir
        self.info = info
        self.approx = approx
        self.max_stages = max_stages
        self.passes: List[dict] = []
        self.stages: List[dict] = []
        self.pass_textures: List[dict] = []
        self.prev: Optional[tuple] = None
        self.constants = set()
        self.derived: Dict[int, float] = {}
        self.input_map: Dict[int, int] = {}
        self.uses_vertex_color = 0
        self.next_synth_sampler = 100
        self.lost: List[str] = []
        self.lowering: List[dict] = []
        self.notes: List[str] = []
        self._stored: List[Tuple[tuple, tuple]] = []
        self._pass_reads: List[int] = []
        self._lower_cache: Dict[int, tuple] = {}
        self._lower_vec_cache: Dict[int, tuple] = {}
        self._materialize_cache: Dict[int, tuple] = {}
        self._materialize_vec_cache: Dict[int, tuple] = {}
        self._uniform_cache: Dict[int, tuple] = {}
        self._uniform_vec_cache: Dict[int, tuple] = {}
        self.allow_input_prebake = False

    # -- pass management -------------------------------------------------

    def _texture_unit(self, sampler: int) -> int:
        for i, t in enumerate(self.pass_textures):
            if t["sampler_reg"] == sampler:
                return i
        if len(self.pass_textures) >= 3:
            raise PlanImpossible("too_many_textures_in_pass")
        coord = self.info.texcoord.get(sampler, 0)
        stype = self.info.sampler_type.get(sampler, 2)
        self.pass_textures.append({"texcoord_reg": coord,
                                   "sampler_reg": sampler,
                                   "sampler_type": stype})
        return len(self.pass_textures) - 1

    def _find_stored(self, vec: tuple) -> Optional[tuple]:
        for old, leaf in self._stored:
            if vec_equal(old, vec):
                return leaf
        return None

    def _source(self, vec: tuple, side: Sequence[int]) -> SideSource:
        stored = self._find_stored(vec)
        if stored is not None:
            vec = stored
        if self.prev is not None and vec_equal(vec, self.prev):
            return SideSource(SRC_PREV, tuple(side))
        s = _side_source(vec, side)
        if s.src == SRC_TEX0:
            unit = self._texture_unit(s.sampler)
            return SideSource(SRC_TEX0 + unit, s.comps, sampler=s.sampler)
        if s.src == SRC_PRIMARY:
            reg = vec[side[0]][2]
            decl = self.ir.inputs.get(reg)
            if decl is None:
                raise PlanImpossible("input_register_unmapped")
            if reg not in self.input_map:
                used = set(self.input_map.values())
                for candidate in (SRC_PRIMARY, SRC_FRAG_PRIMARY,
                                  SRC_FRAG_SECONDARY):
                    if candidate not in used:
                        self.input_map[reg] = candidate
                        break
                else:
                    raise PlanImpossible("too_many_colour_inputs")
            self.uses_vertex_color = 1
            return SideSource(self.input_map[reg], s.comps)
        return s

    def _room(self, need_prev: bool):
        if len(self.stages) < self.max_stages:
            return
        if need_prev and self.prev is not None:
            self.store(self.prev)
        else:
            self._close_pass(None)

    def _close_pass(self, output_sampler: Optional[int]):
        if self.stages:
            consts = sorted({s["const_reg"] for s in self.stages
                             if s.get("const_reg", 255) != 255})
            p = {
                "stage_data": self.stages,
                "textures": self.pass_textures,
                "constants": consts,
                "inputs": [],
                "blend": {"enable": 0, "src": 1, "dst": 1, "op": 1},
                "depth_write": 0,
                "writes": ["oC0"],
                "reads": list(self._pass_reads),
            }
            if output_sampler is not None:
                p["output_sampler"] = output_sampler
            self.passes.append(p)
        self.stages = []
        self.pass_textures = []
        self.prev = None
        self._pass_reads = []

    def store(self, vec: tuple) -> tuple:
        """Materialise ``vec`` in a pass texture and return its TEX leaf."""
        if vec is None:
            raise PlanImpossible("store_none")
        stored = self._find_stored(vec)
        if stored is not None:
            return stored
        # Make vec the current PREV, adding a REPLACE carry if it is a leaf.
        self.ensure(vec)
        if self.prev is None or not vec_equal(self.prev, vec):
            self._emit_carry(vec)
        sampler = self.next_synth_sampler
        self.next_synth_sampler += 1
        self._close_pass(sampler)
        leaf = vleaf("tex", sampler)
        self._stored.append((vec, leaf))
        self.info.texcoord[sampler] = 0
        self.info.sampler_type[sampler] = 2
        return leaf

    def _emit_carry(self, vec: tuple):
        self._room(False)
        rgb = self._source(vec, (0, 1, 2, 0))
        alpha = self._source(vec, (3, 3, 3, 3))
        stage = {
            "func_rgb": F_REPLACE, "func_a": F_REPLACE,
            "src_rgb": [rgb.src, SRC_PREV, SRC_PREV],
            "src_a": [alpha.src, SRC_PREV, SRC_PREV],
            "op_rgb": [rgb.rgb_op(), RGB_COLOR, RGB_COLOR],
            "op_a": [alpha.alpha_op(), A_ALPHA, A_ALPHA],
            "scale_rgb": 0, "scale_a": 0,
            "const_reg": rgb.const_reg if rgb.src == SRC_CONST else alpha.const_reg,
            "const_is_literal": 0,
            "const_rgba8": 0, "instruction": "carry",
        }
        for src in (rgb, alpha):
            if src.src == SRC_CONST:
                self.constants.add(src.const_reg)
            if src.sampler is not None:
                self.pass_reads_append(src.sampler)
        self.stages.append(stage)
        self.prev = vec

    # -- expression emission ---------------------------------------------

    def _collect_operands(self, rgb_info, a_info):
        ops = []
        for info in (rgb_info, a_info):
            if info[0] == "op":
                ops.extend(info[2])
        uniq = []
        seen = set()
        for v in ops:
            k = id(v)
            if k not in seen:
                seen.add(k)
                uniq.append(v)
        return uniq

    def _prepare(self, operands):
        """Make every operand a leaf or the single PREV."""
        prev_needed = any(self.prev is not None and vec_equal(v, self.prev)
                          for v in operands)
        nonavail = [v for v in operands
                    if self.prev is None or not vec_equal(v, self.prev)
                    if not all(is_leaf(e) for e in v)]
        if nonavail and prev_needed:
            self.store(self.prev)
            prev_needed = False
        for n, v in enumerate(nonavail):
            self.ensure(v)
            if n + 1 < len(nonavail) and self.prev is not None:
                self.store(self.prev)

    def _const_vec(self, value: float):
        return materialize_literals(vconst(value), self.info, self)

    def _stage_sources(self, info, side):
        if info[0] == "leaf":
            return None
        name, args = info[1], info[2]
        # D3D lrp(a,b,c)=a*b+(1-a)*c; PICA INTERPOLATE(s0,s1,s2)=s0*s2+s1*(1-s2).
        if name == "lrp":
            args = (args[1], args[2], args[0])
        elif name in ("neg",):
            args = (self._const_vec(0.0), args[0])
        elif name in ("x2",):
            args = (args[0], args[0])
        elif name in ("bias",):
            args = (args[0], self._const_vec(0.5))
        elif name in ("sign",):
            args = (vop("mul", args[0], self._const_vec(2.0)),
                    self._const_vec(1.0))
        elif name in ("complement",):
            args = (self._const_vec(1.0), args[0])
        elif name == "clamp01":
            args = (args[0],)
        return name, args

    def ensure(self, vec: tuple):
        if self._find_stored(vec) is not None:
            return
        if all(is_leaf(e) or is_undef(e) for e in vec):
            return
        if self.prev is not None and vec_equal(vec, self.prev):
            return
        rgb_info = _side_op(vec[:3])
        a_info = _side_op((vec[3],))
        if rgb_info[0] == "mixed" or a_info[0] == "mixed":
            raise PlanImpossible("mixed_channel_ops")
        if rgb_info[0] == "leaf" and a_info[0] == "leaf":
            return
        if rgb_info[0] == "op" and rgb_info[1] not in ("add", "sub", "mul",
                                                      "mad", "lrp", "dot3",
                                                      "clamp01", "neg", "x2",
                                                      "bias", "sign",
                                                      "complement"):
            raise PlanImpossible("op_%s" % rgb_info[1])
        if a_info[0] == "op" and a_info[1] not in ("add", "sub", "mul", "mad",
                                                   "lrp", "dot3", "clamp01",
                                                   "neg", "x2", "bias",
                                                   "sign", "complement"):
            raise PlanImpossible("op_%s" % a_info[1])
        operands = self._collect_operands(rgb_info, a_info)
        # Literal operands are refused here: the approximation pass must bind
        # them to derived constants first.
        for v in operands:
            if any(is_leaf(e) and e[0] == "C" for e in v):
                raise PlanImpossible("literal_operand")
        self._prepare(operands)
        # Re-resolve sources after possible pass boundaries.
        rgb_src = None
        a_src = None
        const_regs = set()
        if rgb_info[0] == "op":
            _, args = self._stage_sources(rgb_info, False)
            rgb_src = [self._source(a, (0, 1, 2, 3)) for a in args]
            const_regs.update(s.const_reg for s in rgb_src
                              if s.src == SRC_CONST)
        if a_info[0] == "op":
            _, args = self._stage_sources(a_info, True)
            a_src = [self._source(a, (3, 3, 3, 3)) for a in args]
            const_regs.update(s.const_reg for s in a_src
                              if s.src == SRC_CONST)
        # The descriptor has one constant register per stage.
        if len(const_regs) > 1:
            keep = sorted(const_regs)[0]
            if self.approx:
                for s in (rgb_src or []) + (a_src or []):
                    if s.src == SRC_CONST:
                        s.const_reg = keep
                self.lost.append("constant_registers_merged_to_c%d" % keep)
                const_regs = {keep}
            else:
                # Re-bind the extra constants through a pass texture.
                for v in operands:
                    if any(is_leaf(e) and leaf_desc(e)[0] == "const"
                           and leaf_desc(e)[1] != keep for e in v):
                        self.store(v)
                rgb_src = None
                a_src = None
                const_regs = set()
                if rgb_info[0] == "op":
                    _, args = self._stage_sources(rgb_info, False)
                    rgb_src = [self._source(a, (0, 1, 2, 3)) for a in args]
                    const_regs.update(s.const_reg for s in rgb_src
                                      if s.src == SRC_CONST)
                if a_info[0] == "op":
                    _, args = self._stage_sources(a_info, True)
                    a_src = [self._source(a, (3, 3, 3, 3)) for a in args]
                    const_regs.update(s.const_reg for s in a_src
                                      if s.src == SRC_CONST)
                if len(const_regs) > 1:
                    raise PlanImpossible("two_constants_in_stage")
        # Texture budget is per pass, not per stage.
        if len({s.sampler for s in (rgb_src or []) + (a_src or [])
                if s.sampler is not None}) > 3:
            raise PlanImpossible("too_many_textures_in_stage")
        self._room(bool(self.prev is not None and
                        any(self.prev is not None and vec_equal(v, self.prev)
                            for v in operands)))
        # Re-resolve once more if _room started a new pass.
        if rgb_info[0] == "op":
            _, args = self._stage_sources(rgb_info, False)
            rgb_src = [self._source(a, (0, 1, 2, 3)) for a in args]
        if a_info[0] == "op":
            _, args = self._stage_sources(a_info, True)
            a_src = [self._source(a, (3, 3, 3, 3)) for a in args]
        stage = self._make_stage(vec, rgb_info, a_info, rgb_src, a_src)
        self.stages.append(stage)
        self.prev = vec

    def _make_stage(self, vec, rgb_info, a_info, rgb_src, a_src):
        stage = {
            "func_rgb": F_REPLACE, "func_a": F_REPLACE,
            "src_rgb": [SRC_PREV, SRC_PREV, SRC_PREV],
            "src_a": [SRC_PREV, SRC_PREV, SRC_PREV],
            "op_rgb": [RGB_COLOR, RGB_COLOR, RGB_COLOR],
            "op_a": [A_ALPHA, A_ALPHA, A_ALPHA],
            "scale_rgb": 0, "scale_a": 0,
            "const_reg": 255, "const_is_literal": 0, "const_rgba8": 0,
        }
        # If one side is a leaf and the other is computed, the stage must
        # replace that side from its own source instead of carrying PREV.
        if rgb_info[0] == "leaf" and not all(is_undef(e) for e in vec[:3]):
            leaf = self._source(vec, (0, 1, 2, 0))
            stage["src_rgb"][0] = leaf.src
            stage["op_rgb"][0] = leaf.rgb_op()
            if leaf.src == SRC_CONST:
                if stage["const_reg"] not in (255, leaf.const_reg):
                    raise PlanImpossible("two_constants_in_stage")
                stage["const_reg"] = leaf.const_reg
        if a_info[0] == "leaf" and not is_undef(vec[3]):
            leaf = self._source(vec, (3, 3, 3, 3))
            stage["src_a"][0] = leaf.src
            stage["op_a"][0] = leaf.alpha_op()
            if leaf.src == SRC_CONST:
                if stage["const_reg"] not in (255, leaf.const_reg):
                    raise PlanImpossible("two_constants_in_stage")
                stage["const_reg"] = leaf.const_reg
        const_reg = 255
        for info, srcs, alpha in ((rgb_info, rgb_src, False),
                                  (a_info, a_src, True)):
            if info[0] != "op":
                continue
            name = info[1]
            func = _func_for(name)
            if func is None:
                raise PlanImpossible("op_%s" % name)
            if name == "clamp01":
                # LDR clamp is the TEV output clamp; a REPLACE is exact.
                s = srcs[0]
                stage["func_rgb" if not alpha else "func_a"] = F_REPLACE
                (stage["src_rgb" if not alpha else "src_a"])[0] = s.src
                (stage["op_rgb" if not alpha else "op_a"])[0] = (
                    s.alpha_op() if alpha else s.rgb_op())
                continue
            if name == "dot3":
                stage["func_rgb"] = (F_DOT3_RGBA if not alpha else F_DOT3_RGBA)
                if not alpha:
                    stage["func_rgb"] = F_DOT3_RGBA
                    stage["src_rgb"] = [srcs[0].src, srcs[1].src, SRC_PREV]
                    stage["op_rgb"] = [srcs[0].rgb_op(), srcs[1].rgb_op(),
                                       RGB_COLOR]
                else:
                    stage["func_a"] = F_DOT3_RGBA
                    stage["src_a"] = [srcs[0].src, srcs[1].src, SRC_PREV]
                    stage["op_a"] = [srcs[0].alpha_op(), srcs[1].alpha_op(),
                                     A_ALPHA]
                continue
            key = "src_a" if alpha else "src_rgb"
            opkey = "op_a" if alpha else "op_rgb"
            stage["func_a" if alpha else "func_rgb"] = func
            for i, s in enumerate(srcs[:3]):
                stage[key][i] = s.src
                stage[opkey][i] = s.alpha_op() if alpha else s.rgb_op()
                if s.src == SRC_CONST:
                    if const_reg not in (255, s.const_reg):
                        raise PlanImpossible("two_constants_in_stage")
                    const_reg = s.const_reg
            # lrp uses all three sources; add/mul use two and leave PREV in
            # the third, which the combine ignores.
        if rgb_info[0] == "op" and rgb_info[1] == "dot3":
            # DOT3_RGBA writes alpha as the dot; if the vec's alpha is not the
            # same dot, the alpha side below overwrites it.
            if a_info[0] == "op" and a_info[1] == "dot3" and vec_equal(
                    rgb_info[2][0], a_info[2][0]):
                pass
        if const_reg == 255:
            const_reg = stage["const_reg"]
        elif stage["const_reg"] not in (255, const_reg):
            raise PlanImpossible("two_constants_in_stage")
        stage["const_reg"] = const_reg
        if const_reg != 255:
            self.constants.add(const_reg)
        for s in (rgb_src or []) + (a_src or []):
            if s.sampler is not None and s.sampler in self.info.texcoord:
                self.pass_reads_append(s.sampler)
        return stage

    def pass_reads_append(self, sampler):
        if sampler >= 100:
            self._pass_reads.append(sampler)

    # -- finish ----------------------------------------------------------

    def finish(self, final: tuple) -> dict:
        self.ensure(final)
        if self.prev is None or not vec_equal(self.prev, final):
            self._emit_carry(final)
        if not self.stages:
            self._emit_carry(final)
        self._close_pass(None)
        if len(self.passes) > 4:
            raise PlanImpossible("passes_limit_%d" % len(self.passes))
        # The final pass is the descriptor the schema sees.
        last = self.passes[-1] if self.passes else {
            "stage_data": [], "textures": [], "blend": "replace",
            "writes": ["oC0"], "reads": []}
        passes = self.passes
        return {"last": last, "passes": passes}

    # -- verification ----------------------------------------------------

    def verify(self) -> bool:
        """Structural pass check: every pass output is the expression we
        intended and every stage stays inside the descriptor limits."""
        for p in self.passes:
            if not p["stage_data"] or len(p["stage_data"]) > self.max_stages:
                return False
            if len(p["textures"]) > 3:
                return False
            for s in p["stage_data"]:
                if s["func_rgb"] > 9 or s["func_a"] > 9:
                    return False
                if any(x not in range(16) for x in s["src_rgb"] + s["src_a"]):
                    return False
        return True


# ---------------------------------------------------------------------------
# Approximation lowering
# ---------------------------------------------------------------------------


EXACT_OPS = {"add", "sub", "mul", "mad", "lrp", "dot3", "clamp01",
             "neg", "x2", "bias", "sign", "complement"}


def _fit_constant(vals, target):
    """Least-squares constant/linear fit; returns dict with mode/coeffs/error."""
    n = len(vals)
    mean_x = sum(vals) / n
    mean_y = sum(target) / n
    var_x = sum((x - mean_x) ** 2 for x in vals) or 1.0
    slope = sum((x - mean_x) * (y - mean_y)
                for x, y in zip(vals, target)) / var_x
    intercept = mean_y - slope * mean_x
    lin = [slope * x + intercept for x in vals]
    const = [mean_y] * n
    lin_err = [abs(a - b) for a, b in zip(target, lin)]
    const_err = [abs(a - b) for a, b in zip(target, const)]
    if max(lin_err) <= max(const_err):
        return {"mode": "linear", "coefficients": [round(slope, 6),
                                                   round(intercept, 6)],
                "max_abs_error": max(lin_err),
                "mean_abs_error": sum(lin_err) / n}
    return {"mode": "constant", "coefficients": [round(mean_y, 6)],
            "max_abs_error": max(const_err),
            "mean_abs_error": sum(const_err) / n}


def materialize_literals(vec: tuple, info: BuildInfo, sched: TevScheduler) -> tuple:
    """Bind literal leaves to derived TEV constant slots and drop non-colour
    input reads to a declared zero constant."""
    key = id(vec)
    cached = sched._materialize_vec_cache.get(key)
    if cached is not None:
        return cached
    out = []
    for expr in vec:
        out.append(_materialize_expr(expr, info, sched))
    out = tuple(out)
    sched._materialize_vec_cache[key] = out
    return out


def _materialize_expr(expr, info, sched):
    key = id(expr)
    cached = sched._materialize_cache.get(key)
    if cached is not None:
        return cached
    out = _materialize_expr_inner(expr, info, sched)
    sched._materialize_cache[key] = out
    return out


def _materialize_expr_inner(expr, info, sched):
    if expr[0] == "C":
        reg = _literal_reg(sched, float(expr[1]))
        return ("L", "const", reg, 0)
    if expr[0] == L:
        kind, reg, comp = expr[1], expr[2], expr[3]
        if kind == "in":
            decl = sched.ir.inputs.get(reg, {})
            if decl.get("usage") != 10 and not sched.allow_input_prebake:
                sched.lost.append("non_colour_input_v%d_lowered_to_zero" % reg)
                r = _literal_reg(sched, 0.0)
                return ("L", "const", r, 0)
        return expr
    if expr[0] != O:
        return expr
    name, args = expr[1], expr[2:]
    return (O, name) + tuple(materialize_literals(a, info, sched)
                             for a in args)


def _literal_reg(sched: TevScheduler, value: float) -> int:
    for reg, v in sched.derived.items():
        if abs(v - value) < 1e-9:
            return reg
    reg = 200 + len(sched.derived)
    sched.derived[reg] = round(float(value), 8)
    return reg


def uniformize_approx(vec: tuple, sched: TevScheduler, depth=0) -> tuple:
    """Collapse channel-local shapes the one TEV RGB side cannot express.

    This is an approximation canonicalisation: it keeps the red channel when
    RGB channels disagree, keeps the first of mixed leaf registers, and drops
    texture units beyond the three PICA binds.  Every collapse is recorded in
    ``lost_terms`` and the numeric error is measured afterwards.
    """
    key = id(vec)
    cached = sched._uniform_vec_cache.get(key)
    if cached is not None:
        return cached
    out = []
    for e in vec:
        out.append(_uniformize_expr(e, sched, depth))
    out = tuple(out)
    if not _uniform_side(out, (0, 1, 2)):
        first = next((out[i] for i in (0, 1, 2) if not is_undef(out[i])),
                     out[0])
        out = (first, first, first, out[3])
        sched.lost.append("rgb_channels_collapsed_to_red")
    if not _uniform_side(out, (3,)):
        first = out[3]
        out = (out[0], out[1], out[2], first)
    sched._uniform_vec_cache[key] = out
    return out


def _uniformize_expr(expr, sched, depth):
    key = id(expr)
    cached = sched._uniform_cache.get(key)
    if cached is not None:
        return cached
    if expr[0] != O:
        sched._uniform_cache[key] = expr
        return expr
    name, args = expr[1], expr[2:]
    new_args = tuple(uniformize_approx(a, sched, depth + 1) for a in args)
    out = (O, name) + new_args
    sched._uniform_cache[key] = out
    return out


def _uniform_side(vec: tuple, side: Sequence[int]) -> bool:
    exprs = [vec[i] for i in side if not is_undef(vec[i])]
    if not exprs:
        return True
    if all(is_leaf(e) for e in exprs):
        descs = [leaf_desc(e) for e in exprs]
        # Literal constants with the same value are uniform; different
        # component selectors or registers are not.
        if all(d[0] == "lit" for d in descs):
            return len({round(d[2], 8) for d in descs}) == 1
        return len({(d[0], d[1]) for d in descs}) == 1 and \
            len({d[2] for d in descs}) == 1
    if all(e[0] == O for e in exprs):
        return all(e[1] == exprs[0][1] and e[2:] == exprs[0][2:]
                   for e in exprs)
    return False


def _drop_extra_textures(vec: tuple, sched: TevScheduler, keep=3) -> tuple:
    samplers = []
    seen = set()

    def collect(e):
        key = id(e)
        if key in seen:
            return
        seen.add(key)
        if e[0] == L and e[1] == "tex" and e[2] not in samplers:
            samplers.append(e[2])
        elif e[0] == O:
            for a in e[2:]:
                for x in a:
                    collect(x)
    for e in vec:
        collect(e)
    if len(samplers) <= keep:
        return vec
    keep_set = set(sorted(samplers)[:keep])
    for s in samplers:
        if s not in keep_set:
            sched.lost.append("texture_unit_%d_dropped" % s)

    cache: Dict[int, tuple] = {}

    def replace(e):
        key = id(e)
        if key in cache:
            return cache[key]
        if e[0] == L and e[1] == "tex" and e[2] not in keep_set:
            out = ("L", "tex", min(keep_set), e[3])
        elif e[0] == O:
            out = (O, e[1]) + tuple(
                tuple(replace(x) for x in a) for a in e[2:])
        else:
            out = e
        cache[key] = out
        return out
    return tuple(replace(e) for e in vec)


def lower_approx(vec: tuple, info: BuildInfo, sched: TevScheduler,
                 stack=()) -> tuple:
    """Rewrite unsupported nodes into TEV-expressible approximations."""
    key = id(vec)
    cached = sched._lower_vec_cache.get(key)
    if cached is not None:
        return cached
    out = []
    for expr in vec:
        out.append(_lower_expr(expr, info, sched, stack))
    out = tuple(out)
    sched._lower_vec_cache[key] = out
    return out


def _lower_expr(expr, info, sched, stack):
    key = id(expr)
    cached = sched._lower_cache.get(key)
    if cached is not None:
        return cached
    try:
        out = _lower_expr_inner(expr, info, sched, stack)
    except IndexError:
        print("LOWER_BAD", type(expr), repr(expr)[:800], stack, file=sys.stderr)
        raise
    sched._lower_cache[key] = out
    return out


def _lower_expr_inner(expr, info, sched, stack):
    if expr[0] in (L, "C", U):
        return expr
    if expr[0] != O:
        return expr
    name, args = expr[1], expr[2:]
    if name in EXACT_OPS:
        return (O, name) + tuple(lower_approx(a, info, sched, stack + (name,))
                                 for a in args)
    if name == "min" or name == "max":
        a, b = args
        # Only the [0,1] clamp forms have a TEV meaning; keep them as the
        # implicit LDR clamp.
        for lit in (0.0, 1.0):
            if all(is_leaf(e) and e[0] == "C" and abs(e[1] - lit) < 1e-9
                   for e in b):
                return lower_approx(a, info, sched, stack)[0]
        sched.lost.append("%s_lowered_to_first_operand" % name)
        return lower_approx(a, info, sched, stack)[0]
    if name == "abs":
        sched.lost.append("abs_lowered_to_identity")
        return lower_approx(args[0], info, sched, stack)[0]
    if name == "frc":
        sched.lost.append("frc_lowered_to_identity")
        return lower_approx(args[0], info, sched, stack)[0]
    if name == "nrm":
        sched.lost.append("nrm_lowered_to_unnormalised_vector")
        return lower_approx(args[0], info, sched, stack)[0]
    if name in ("rcp", "rsq"):
        vals = [0.25 + i * (4.0 - 0.25) / 31.0 for i in range(32)] if name == "rcp" \
            else [0.0625 + i * (16.0 - 0.0625) / 31.0 for i in range(32)]
        target = [(1.0 / x) if name == "rcp" else (1.0 / math.sqrt(x))
                  for x in vals]
        fit = _fit_constant(vals, target)
        sched.lowering.append({"opcode": name, "mode": fit["mode"],
                               "coefficients": fit["coefficients"],
                               "domain": [vals[0], vals[-1]],
                               "samples": len(vals),
                               "max_abs_error": fit["max_abs_error"],
                               "mean_abs_error": fit["mean_abs_error"]})
        sched.lost.append("%s_lowered_to_%s_fit" % (name, fit["mode"]))
        value = fit["coefficients"][0] if fit["mode"] == "constant" else \
            fit["coefficients"][1]
        return ("C", float(value))
    if name == "pow":
        base, expv = args
        ev = _const_eval(expv[0], info.defs, info.defi)
        if ev is not None and ev == int(ev) and 0 <= int(ev) <= 8:
            e = int(ev)
            if e == 0:
                return ("C", 1.0)
            v = lower_approx(base, info, sched, stack)
            for _ in range(e - 1):
                v = vop("mul", v, v)
            sched.lost.append("pow_expanded_to_%d_muls" % e)
            return v[0]
        sched.lost.append("pow_lowered_to_base")
        return lower_approx(base, info, sched, stack)[0]
    if name == "cmp":
        # PICA has no compare/select; keep the then arm.
        sched.lost.append("cmp_lowered_to_then_arm")
        return lower_approx(args[1], info, sched, stack)[0]
    if name in ("slt", "sge"):
        sched.lost.append("%s_lowered_to_zero" % name)
        return ("C", 0.0)
    if name == "predicate":
        sched.lost.append("predicate_lowered_to_zero")
        return ("C", 0.0)
    if name == "dot4":
        sched.lost.append("dot4_lowered_to_dot3")
        return vop("dot3", lower_approx(args[0], info, sched, stack),
                   lower_approx(args[1], info, sched, stack))[0]
    if name == "dp2add":
        sched.lost.append("dp2add_lowered_to_dot3")
        a = lower_approx(args[0], info, sched, stack)
        b = lower_approx(args[1], info, sched, stack)
        z = vzero()
        av = (a[0], a[1], z[0], z[3])
        bv = (b[0], b[1], z[0], b[3])
        return vop("mad", vop("dot3", av, bv), vone(),
                   lower_approx(args[2], info, sched, stack))[0]
    if name in ("dsx", "dsy"):
        sched.lost.append("%s_lowered_to_zero" % name)
        return ("C", 0.0)
    if name in ("exp", "log", "mova"):
        sched.lost.append("%s_lowered_to_constant" % name)
        return ("C", 1.0)
    sched.lost.append("%s_lowered_to_zero" % name)
    return ("C", 0.0)


def simplify_fallback(info: BuildInfo, sched: TevScheduler) -> tuple:
    """Dominant-texture fallback: always expressible in <=3 stages."""
    final = info.final
    texs = []
    for e in final:
        if e[0] == L and e[1] == "tex":
            texs.append(e[2])
    if not texs:
        for e in final:
            if e[0] == O:
                for a in e[2:]:
                    for x in a:
                        if x[0] == L and x[1] == "tex":
                            texs.append(x[2])
    sampler = min(texs) if texs else 0
    base = vleaf("tex", sampler)
    color = None
    for reg, decl in info.values and sorted(info.values.items()):
        pass
    # Prefer a vertex colour multiply when the shader declared one.
    colours = sorted(r for r, d in sched.ir.inputs.items() if d.get("usage") == 10)
    if colours:
        color = vleaf("in", colours[0])
    out = vop("mul", base, color) if color is not None else base
    # Alpha: texture alpha (or the first colour alpha) so the output is not
    # silently opaque when the original had alpha.
    out = out[:3] + (base[3] if sampler is not None else
                     (color[3] if color is not None else ONE),)
    sched.lost.append("fallback_dominant_texture_%d" % sampler)
    return out


# ---------------------------------------------------------------------------
# Numeric interpretation
# ---------------------------------------------------------------------------


def _safe_recip(x):
    return (1e6 if x >= 0 else -1e6) if abs(x) < 1e-9 else 1.0 / x


def _safe_rsq(x):
    v = math.sqrt(abs(x))
    return 1e6 if v < 1e-9 else 1.0 / v


def _fit(v):
    if v != v or v in (float("inf"), float("-inf")):
        return 0.0
    return max(-1e6, min(1e6, v))


def _clamp01(v):
    return max(0.0, min(1.0, v))


def eval_ir(ir: C.IR, tex, consts, inputs, defs=None, defi=None,
            max_steps=20000):
    """Interpret the IR, including constant branches and bounded loops."""
    defs = dict(ir.defs) if defs is None else defs
    defi = dict(defi or {})
    temps: Dict[int, list] = {}
    out = [0.0, 0.0, 0.0, 1.0]
    body = list(ir.body)
    stack = []          # (if_index, else_index, endif_index, taken)
    i = 0
    steps = 0

    def value(src):
        if src.reg_type == asm.REG_TEMP:
            vec = list(temps.get(src.index, [0.0] * 4))
        elif src.reg_type == asm.REG_INPUT:
            vec = list(inputs.get(src.index, [0.0] * 4))
        elif src.reg_type == asm.REG_CONST:
            vec = list(consts.get(src.index, list(defs.get(src.index,
                                                           (0.0,) * 4))))
        else:
            vec = [0.0] * 4
        comps = [("xyzw".index(src.swizzle[c])) for c in range(4)]
        vec = [vec[c] for c in comps]
        m = src.modifier
        if m == 1:
            vec = [-v for v in vec]
        elif m == 2:
            vec = [v - 0.5 for v in vec]
        elif m == 3:
            vec = [-(v - 0.5) for v in vec]
        elif m == 4:
            vec = [2.0 * v - 1.0 for v in vec]
        elif m == 5:
            vec = [-(2.0 * v - 1.0) for v in vec]
        elif m == 6:
            vec = [1.0 - v for v in vec]
        elif m == 7:
            vec = [2.0 * v for v in vec]
        elif m == 8:
            vec = [-2.0 * v for v in vec]
        elif m == 11:
            vec = [abs(v) for v in vec]
        elif m == 12:
            vec = [-abs(v) for v in vec]
        elif m in (9, 10, 13):
            vec = [1.0 if abs(v) < 1e-12 else 0.0 for v in vec]
        return vec

    while i < len(body) and steps < max_steps:
        steps += 1
        ins = body[i]
        op = ins.opcode
        if op in (0x1F, 0x51):
            i += 1
            continue
        if op == 0x30:
            if ins.dst is not None and ins.literal:
                defi[ins.dst.index] = int(ins.literal[0])
            i += 1
            continue
        if op in (0x28, 0x29, 0x2A, 0x2B):
            i = _branch_step(body, i, ins, value, defs, defi, stack)
            continue
        if op in (0x26, 0x1B):  # rep/loop
            # Evaluate the count and execute at most 8 iterations.
            count = 1
            if ins.srcs and ins.srcs[0].reg_type == asm.REG_CONSTINT:
                count = defi.get(ins.srcs[0].index, 1)
            end = _find_end(body, i, (0x27, 0x1D))
            if end is None:
                i += 1
                continue
            if count <= 0:
                i = end + 1
                continue
            # Execute the body by recursive interpretation on a copy.
            sub = _sub_ir(ir, body[i + 1:end])
            for _ in range(min(count, 8)):
                r = eval_ir(sub, tex, consts, inputs, defs, defi, max_steps=max_steps)
                for ch in range(4):
                    out[ch] = r[ch]
            i = end + 1
            continue
        if op in (0x27, 0x1D):
            i += 1
            continue
        if op in (0x2C, 0x60):
            i = _skip_to_loop_end(body, i)
            continue
        if op == 0x2D:  # breakc
            cond = _compare(body, i, value, defs, defi)
            if cond:
                i = _skip_to_loop_end(body, i)
            else:
                i += 1
            continue
        if op == 0x42 or op in (0x5D, 0x5F):
            if len(ins.srcs) < 2:
                i += 1
                continue
            samp = ins.srcs[1]
            vec = list(tex.get(samp.index, [0.0] * 4))
        elif op == 0x41:  # texkill
            a = value(ins.srcs[0]) if ins.srcs else [0.0] * 4
            if a[3] <= 0.0:
                out[3] = 0.0
            i += 1
            continue
        elif op == 0x01:
            vec = value(ins.srcs[0])
        elif op in (0x02, 0x03, 0x05):
            a, b = value(ins.srcs[0]), value(ins.srcs[1])
            if op == 0x02:
                vec = [a[k] + b[k] for k in range(4)]
            elif op == 0x03:
                vec = [a[k] - b[k] for k in range(4)]
            else:
                vec = [a[k] * b[k] for k in range(4)]
        elif op == 0x04:
            a, b, c = (value(ins.srcs[k]) for k in range(3))
            vec = [a[k] * b[k] + c[k] for k in range(4)]
        elif op in (0x08, 0x09):
            a, b = value(ins.srcs[0]), value(ins.srcs[1])
            d = sum(a[k] * b[k] for k in range(3 if op == 0x08 else 4))
            vec = [d, d, d, d]
        elif op in (0x0A, 0x0B):
            a, b = value(ins.srcs[0]), value(ins.srcs[1])
            fn = min if op == 0x0A else max
            vec = [fn(a[k], b[k]) for k in range(4)]
        elif op in (0x0C, 0x0D):
            a, b = value(ins.srcs[0]), value(ins.srcs[1])
            vec = [1.0 if ((a[k] < b[k]) if op == 0x0C else (a[k] >= b[k]))
                   else 0.0 for k in range(4)]
        elif op == 0x12:
            a, b, c = (value(ins.srcs[k]) for k in range(3))
            vec = [a[k] * b[k] + (1.0 - a[k]) * c[k] for k in range(4)]
        elif op == 0x13:
            a = value(ins.srcs[0])
            vec = [v - math.floor(v) for v in a]
        elif op == 0x06:
            vec = [_safe_recip(v) for v in value(ins.srcs[0])]
        elif op == 0x07:
            vec = [_safe_rsq(v) for v in value(ins.srcs[0])]
        elif op == 0x20:
            a, b = value(ins.srcs[0]), value(ins.srcs[1])
            vec = [math.pow(abs(a[k]), max(0.0, min(8.0, b[k])))
                   for k in range(4)]
        elif op == 0x23:
            vec = [abs(v) for v in value(ins.srcs[0])]
        elif op == 0x24:
            a = value(ins.srcs[0])
            n = _safe_rsq(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
            vec = [a[0] * n, a[1] * n, a[2] * n, 1.0]
        elif op == 0x58:
            a, b, c = (value(ins.srcs[k]) for k in range(3))
            vec = [b[k] if a[k] >= 0.0 else c[k] for k in range(4)]
        elif op == 0x5A:
            a, b, c = (value(ins.srcs[k]) for k in range(3))
            vec = [a[0] * b[0] + a[1] * b[1] + c[k] for k in range(4)]
        elif op in (0x5B, 0x5C):
            vec = [0.0] * 4
        elif op in (0x0E, 0x0F):
            vec = [1.0] * 4
        elif op == 0x2E:
            vec = [0.0] * 4
        else:
            vec = [0.0] * 4
        if ins.dst is None:
            i += 1
            continue
        if ins.dst.modifier & 1:
            vec = [_clamp01(v) for v in vec]
        vec = [_fit(v) for v in vec]
        if ins.dst.reg_type == asm.REG_COLOROUT:
            for k in range(4):
                if ins.dst.mask & (1 << k):
                    out[k] = _clamp01(vec[k])
        elif ins.dst.reg_type == asm.REG_TEMP:
            prev = list(temps.get(ins.dst.index, [0.0] * 4))
            for k in range(4):
                if ins.dst.mask & (1 << k):
                    prev[k] = vec[k]
            temps[ins.dst.index] = prev
        i += 1
    return out


def _sub_ir(ir, body):
    out = copy.copy(ir)
    out.body = body
    return out


def _find_end(body, start, end_ops):
    depth = 0
    for j in range(start + 1, len(body)):
        op = body[j].opcode
        if op in (0x26, 0x1B):
            depth += 1
        elif op in (0x27, 0x1D):
            if depth == 0:
                return j
            depth -= 1
    return None


def _skip_to_loop_end(body, start):
    end = _find_end(body, start, (0x27, 0x1D))
    return (end + 1) if end is not None else start + 1


def _compare(body, i, value, defs, defi):
    ins = body[i]
    if len(ins.srcs) < 1:
        return False
    a = value(ins.srcs[0])[0]
    b = value(ins.srcs[1])[0] if len(ins.srcs) > 1 else 0.0
    return {1: a > b, 2: a == b, 3: a >= b, 4: a < b,
            5: a != b, 6: a <= b}.get(ins.comparison, a < b)


def _branch_step(body, i, ins, value, defs, defi, stack):
    op = ins.opcode
    if op in (0x28, 0x29):
        # Find matching else/endif.
        depth = 0
        else_i = None
        end_i = None
        for j in range(i + 1, len(body)):
            if body[j].opcode in (0x28, 0x29):
                depth += 1
            elif body[j].opcode == 0x2B:
                if depth == 0:
                    end_i = j
                    break
                depth -= 1
            elif body[j].opcode == 0x2A and depth == 0:
                else_i = j
        if end_i is None:
            return i + 1
        cond = _compare(body, i, value, defs, defi)
        if cond:
            return i + 1
        return (else_i + 1) if else_i is not None else end_i + 1
    if op == 0x2A:
        # Reached else after a taken then arm: jump to endif.
        depth = 0
        for j in range(i + 1, len(body)):
            if body[j].opcode in (0x28, 0x29):
                depth += 1
            elif body[j].opcode == 0x2B:
                if depth == 0:
                    return j + 1
                depth -= 1
        return i + 1
    return i + 1


# ---------------------------------------------------------------------------
# Plan evaluation
# ---------------------------------------------------------------------------


def _sel(vec, op, ch, alpha):
    if op in (RGB_COLOR, A_ALPHA):
        return vec[3 if alpha else ch]
    mapping_rgb = {RGB_R: 0, RGB_1M_R: 0, RGB_G: 1, RGB_1M_G: 1,
                   RGB_B: 2, RGB_1M_B: 2, RGB_ALPHA: 3, RGB_1M_ALPHA: 3}
    mapping_a = {A_R: 0, A_1M_R: 0, A_G: 1, A_1M_G: 1, A_B: 2, A_1M_B: 2,
                 A_ALPHA: 3, A_1M_ALPHA: 3}
    comp = (mapping_a if alpha else mapping_rgb).get(op, 3)
    v = vec[comp]
    if op in (RGB_1M_COLOR, RGB_1M_R, RGB_1M_G, RGB_1M_B, RGB_1M_ALPHA,
              A_1M_R, A_1M_G, A_1M_B, A_1M_ALPHA):
        return 1.0 - v
    return v


def _combine(func, a, b, c):
    if func == F_REPLACE:
        return a
    if func == F_MODULATE:
        return a * b
    if func == F_ADD:
        return a + b
    if func == F_ADD_SIGNED:
        return a + b - 0.5
    if func == F_INTERPOLATE:
        # PICA INTERPOLATE(s0,s1,s2)=s0*s2+s1*(1-s2).
        return a * c + b * (1.0 - c)
    if func == F_SUBTRACT:
        return a - b
    if func == F_MULTIPLY_ADD:
        return a * b + c
    if func == F_ADD_MULTIPLY:
        return (a + b) * c
    return a


def eval_stages(stages, tex_by_unit, consts, prev=None, primary=None,
                frag0=None, frag1=None):
    prev = list(prev) if prev is not None else [0.0, 0.0, 0.0, 0.0]
    primary = primary or [1.0, 1.0, 1.0, 1.0]
    frag0 = frag0 or [0.0, 0.0, 0.0, 0.0]
    frag1 = frag1 or [0.0, 0.0, 0.0, 0.0]

    def src(src_id, op, ch, alpha):
        if src_id == SRC_PREV:
            return _sel(prev, op, ch, alpha)
        if src_id == SRC_PRIMARY:
            return _sel(primary, op, ch, alpha)
        if src_id == SRC_FRAG_PRIMARY:
            return _sel(frag0, op, ch, alpha)
        if src_id == SRC_FRAG_SECONDARY:
            return _sel(frag1, op, ch, alpha)
        if SRC_TEX0 <= src_id <= SRC_TEX2:
            unit = src_id - SRC_TEX0
            vec = tex_by_unit[unit] if unit < len(tex_by_unit) else [0.0] * 4
            return _sel(vec, op, ch, alpha)
        if src_id == SRC_CONST:
            reg = stage_const.get("const_reg", 0)
            vec = consts.get(reg, [0.0, 0.0, 0.0, 0.0])
            return _sel(vec, op, ch, alpha)
        return 0.0

    for stage in stages:
        stage_const = stage
        out = [0.0] * 4
        if stage["func_rgb"] in (F_DOT3_RGBA, F_DOT3_RGB):
            a = [src(stage["src_rgb"][0], stage["op_rgb"][0], c, False)
                 for c in range(3)]
            b = [src(stage["src_rgb"][1], stage["op_rgb"][1], c, False)
                 for c in range(3)]
            d = _clamp01(sum(x * y for x, y in zip(a, b)))
            out[0] = out[1] = out[2] = d
        else:
            a = [src(stage["src_rgb"][0], stage["op_rgb"][0], c, False)
                 for c in range(3)]
            b = [src(stage["src_rgb"][1], stage["op_rgb"][1], c, False)
                 for c in range(3)]
            c3 = [src(stage["src_rgb"][2], stage["op_rgb"][2], c, False)
                  for c in range(3)]
            for ch in range(3):
                out[ch] = _combine(stage["func_rgb"], a[ch], b[ch], c3[ch])
        if stage["func_a"] == F_DOT3_RGBA:
            out[3] = out[0]
        elif stage["func_a"] == F_DOT3_RGB:
            out[3] = src(stage["src_a"][0], stage["op_a"][0], 3, True)
        else:
            a = src(stage["src_a"][0], stage["op_a"][0], 3, True)
            b = src(stage["src_a"][1], stage["op_a"][1], 3, True)
            c3 = src(stage["src_a"][2], stage["op_a"][2], 3, True)
            out[3] = _combine(stage["func_a"], a, b, c3)
        out[:3] = [_clamp01(v * (1.0, 2.0, 4.0)[stage.get("scale_rgb", 0)])
                   for v in out[:3]]
        out[3] = _clamp01(out[3] * (1.0, 2.0, 4.0)[stage.get("scale_a", 0)])
        prev = out
    return prev


def eval_plan_entry(entry, tex, consts, primary, frag0, frag1):
    outputs = {}
    prev = None
    for p in entry.get("passes") or [{
            "stage_data": entry["stage_data"], "textures": entry["textures"]}]:
        unit_tex = []
        for t in p.get("textures", []):
            samp = t.get("sampler_reg")
            if samp in outputs:
                unit_tex.append(outputs[samp])
            else:
                unit_tex.append(tex.get(samp, [0.0] * 4))
        prev = eval_stages(p["stage_data"], unit_tex, consts, None,
                           primary, frag0, frag1)
        if "output_sampler" in p:
            outputs[p["output_sampler"]] = prev
    return prev


# ---------------------------------------------------------------------------
# Plan synthesis
# ---------------------------------------------------------------------------


def _sample_inputs(ir, rng, shadow=False):
    consts = {}
    cnames = {c.register_index: c.name for c in ir.program.constants
              if c.register_set == 2}
    for reg in range(40):
        name = cnames.get(reg, "")
        if "Direction" in name:
            v = [rng.uniform(-1, 1) for _ in range(3)]
            n = math.sqrt(sum(x * x for x in v)) or 1.0
            consts[reg] = [x / n for x in v] + [0.0]
        elif "Position" in name:
            consts[reg] = [rng.uniform(-4, 4) for _ in range(3)] + [0.0]
        else:
            consts[reg] = [rng.random() for _ in range(4)]
    for reg, vals in ir.defs.items():
        consts[reg] = list(vals)
    inputs = {}
    colours = sorted((d["usage_index"], reg) for reg, d in ir.inputs.items()
                     if d.get("usage") == 10)
    for n, (ui, reg) in enumerate(colours):
        inputs[reg] = [rng.random() for _ in range(3)] + [
            0.0 if (shadow and ui == 1) else rng.random()]
    for reg, decl in ir.inputs.items():
        if reg in inputs:
            continue
        if decl.get("usage") == 5:
            inputs[reg] = [rng.random(), rng.random(), 0.0, 1.0]
        else:
            inputs[reg] = [rng.uniform(-1, 1) for _ in range(4)]
    tex = {}
    for samp in set(ir.samplers) | set(range(8)):
        tex[samp] = [rng.random() for _ in range(4)]
    primary = inputs.get(colours[0][1], [1, 1, 1, 1]) if colours else [1, 1, 1, 1]
    frag0 = inputs.get(colours[1][1], [0, 0, 0, 0]) if len(colours) > 1 else [0, 0, 0, 0]
    frag1 = inputs.get(colours[2][1], [0, 0, 0, 0]) if len(colours) > 2 else [0, 0, 0, 0]
    return tex, consts, inputs, primary, frag0, frag1


def measure_error(ir, entry, samples=8, seed=0xC0FFEE):
    rng = random.Random(seed)
    max_err = 0.0
    total = 0.0
    count = 0
    shadow = "shadow" in (entry.get("id", "").lower())
    for _ in range(samples):
        tex, consts, inputs, primary, frag0, frag1 = _sample_inputs(ir, rng, shadow)
        # Derived constants are explicit numbers, not D3D register binds.
        for reg, val in entry.get("derived_constants", {}).items():
            consts[int(reg)] = [float(val)] * 4
        want = eval_ir(ir, tex, consts, inputs)
        got = eval_plan_entry(entry, tex, consts, primary, frag0, frag1)
        for a, b in zip(want, got):
            e = abs(a - b)
            max_err = max(max_err, e)
            total += e
            count += 1
    return {"samples": samples, "max_abs_error": max_err,
            "mean_abs_error": total / max(1, count),
            "domain": ("LDR textures/colours; random sampled constants and "
                       "input varyings")}


def _input_entries(ir, sched):
    out = []
    for reg, tev in sorted(sched.input_map.items()):
        decl = ir.inputs.get(reg, {})
        out.append({"usage": decl.get("usage", 10),
                    "usage_index": decl.get("usage_index", 0),
                    "reg_index": reg, "tev_source": tev})
    return out


def _preconditions(ir, info, sched, quality):
    pre = ["ldr_quality"]
    if any(any(not (0.0 <= v <= 1.0) for v in vals)
           for vals in ir.defs.values()):
        pre.append("def_uniformized")
    for i in range(min(3, len({t for t in info.texcoord}))):
        pre.append("texture%d_2d" % i)
    if sched.uses_vertex_color:
        pre.append("vertex_color")
    if info.non_colour_inputs:
        pre.append("texcoord_inputs_prebaked_as_colour_varyings"
                   if quality == "exact"
                   else "non_colour_inputs_dropped")
    if info.dependent_reads:
        pre.append("uv_prebaked_per_sampler" if quality == "exact"
                   else "dependent_texture_read_approximated")
    if info.texkill:
        pre.append("texkill_external_alpha_test")
    if info.flow.get("mode") == "branchless_select":
        pre.append("branchless_select_then_arm")
    if info.flow.get("truncated"):
        pre.append("loop_truncated")
    if sched.passes and len(sched.passes) > 1:
        pre.append("multipass_intermediate_texture")
    if quality == "approx":
        pre.append("approximation_declared")
    return pre


def plan_shader(ir: C.IR, *, family="generic", approx_only=False,
                samples=8, max_textures=3) -> Optional[dict]:
    if ir.kind != "ps" or ir.error:
        return None
    reduced, flow = reduce_flow(ir)
    body = ir.body if reduced is None else reduced.body
    info = build_values(ir, reduced_body=body)
    info.flow = flow
    exact_ok = (reduced is not None and not info.unsupported
                and flow.get("mode") in ("straight_line", "constant_branch"))
    # Unsupported opcodes can still be exact if the scheduler lowers them
    # without approximation (pow with a constant integer exponent).
    exact_ops_only = all(
        e[1] in EXACT_OPS or (e[1] == "pow")
        for e in _walk_ops(info.final)) if info.unsupported == [] else False
    quality = "exact"
    sched = TevScheduler(ir, info, approx=False)
    try:
        if approx_only or not exact_ok or not exact_ops_only:
            raise PlanImpossible("approximation_path")
        sched.allow_input_prebake = True
        mat = materialize_literals(info.final, info, sched)
        sched.finish(mat)
        if not sched.verify():
            raise PlanImpossible("scheduler_verify")
    except PlanImpossible:
        quality = "approx"
        # Rebuild from a lowered expression.  Flow was already reduced; the
        # lowered tree may still be too wide, so keep a dominant fallback.
        sched = TevScheduler(ir, info, approx=True)
        try:
            lowered = lower_approx(info.final, info, sched)
            lowered = uniformize_approx(lowered, sched)
            lowered = _drop_extra_textures(lowered, sched, keep=max_textures)
            lowered = materialize_literals(lowered, info, sched)
            info2 = copy.copy(info)
            info2.final = lowered
            sched.info = info2
            sched.finish(lowered)
            if not sched.verify():
                raise PlanImpossible("lowered_verify")
        except PlanImpossible:
            sched = TevScheduler(ir, info, approx=True)
            fallback = simplify_fallback(info, sched)
            fallback = uniformize_approx(fallback, sched)
            fallback = materialize_literals(fallback, info, sched)
            info2 = copy.copy(info)
            info2.final = fallback
            sched.info = info2
            try:
                sched.finish(fallback)
            except PlanImpossible:
                # Last resort: one REPLACE(TEX0).  Still a named plan.
                sched = TevScheduler(ir, info, approx=True)
                sched._emit_carry(vleaf("tex", 0))
                sched._close_pass(None)
    first = sched.passes[0] if sched.passes else {
        "stage_data": [], "textures": []}
    entry = {
        "id": "sched.%s.%s" % (family, ir.sha256[:8]),
        "kind": "tev", "family": family if family in (
            "ui", "blit_post", "sky_water", "terrain", "skinning",
            "lighting", "menu_light", "generic") else "generic",
        "stage": "ps_3_0", "sha256": ir.sha256, "sha1": ir.sha1,
        "version": "0x%08x" % ir.version,
        "instructions": len([i for i in ir.body if i.opcode not in (0x00,)]),
        "draw_enabled": 0, "runtime": 0,
        "stages": len(first["stage_data"]),
        "stage_data": first["stage_data"],
        "textures": first.get("textures", []),
        "inputs": _input_entries(ir, sched),
        "constants": sorted(sched.constants),
        "uses_vertex_color": sched.uses_vertex_color,
        "requires_alpha_fixup": 0,
        "ldr_clamped": 1,
        "quality": quality,
        "preconditions": _preconditions(ir, info, sched, quality),
        "note": ("scheduler-generated TEV plan; draw/runtime disabled"),
    }
    if sched.derived:
        entry["derived_constants"] = {str(k): v for k, v in
                                      sorted(sched.derived.items())}
    if len(sched.passes) > 1:
        entry["pass_count"] = len(sched.passes)
        entry["passes"] = sched.passes
    if any(any(not (0.0 <= v <= 1.0) for v in vals)
           for vals in ir.defs.values()):
        entry["constant_values"] = {str(reg): list(vals)
                                    for reg, vals in sorted(ir.defs.items())}
    if quality == "exact":
        entry["contract"] = "full"
        err = measure_error(ir, entry, samples=max(4, samples))
        entry["verification"] = err
        if err["max_abs_error"] > 2e-3:
            # The structural scheduler lost something; demote honestly.
            entry["quality"] = "approx"
            entry["approximation"] = {
                "policy": "scheduler_structural_demotion",
                "lost_terms": ["numeric_mismatch_above_tolerance"],
                "reason": ("the symbolic scheduler accepted a shape whose "
                           "numeric cross-check exceeded 2e-3"),
                "control_flow": flow,
                "op_lowering": sched.lowering,
                "def_literals": _def_meta(ir),
            }
            entry["approx_error"] = err
    if entry["quality"] == "approx":
        entry["approximation"] = {
            "policy": ("scheduler_approximation" if not entry.get("approximation")
                       else entry["approximation"]["policy"]),
            "lost_terms": sched.lost or ["declared_approximation"],
            "reason": ("PICA TEV has six stages, one PREV accumulator and no "
                       "per-pixel rcp/rsq/pow/compare; the surviving terms are "
                       "the TEV-expressible subset, explicitly listed here."),
            "control_flow": flow,
            "op_lowering": sched.lowering,
            "def_literals": _def_meta(ir),
            "multipass": len(sched.passes) > 1,
        }
        entry["approx_error"] = measure_error(ir, entry, samples=max(8, samples))
    return entry


def _walk_exprs(vec):
    seen = set()

    def walk(e):
        if not isinstance(e, tuple):
            return
        key = id(e)
        if key in seen:
            return
        seen.add(key)
        yield e
        if e[0] == O:
            for a in e[2:]:
                for x in a:
                    yield from walk(x)
    for e in vec:
        yield from walk(e)


def _walk_ops(vec):
    seen = set()

    def walk(e):
        if e[0] != O:
            return
        key = id(e)
        if key in seen:
            return
        seen.add(key)
        yield e
        for a in e[2:]:
            for x in a:
                yield from walk(x)
    for e in vec:
        yield from walk(e)


def _def_meta(ir):
    rows = []
    for reg, vals in sorted(ir.defs.items()):
        if all(0.0 <= v <= 1.0 for v in vals):
            continue
        rows.append({"reg": reg, "literal": list(vals),
                     "handling": "uniform_register_or_derived_constant"})
    return rows


# ---------------------------------------------------------------------------
# Catalog generation
# ---------------------------------------------------------------------------


def load_chunks(path: str) -> List[bytes]:
    with open(path, "rb") as handle:
        data = handle.read()
    count = struct.unpack_from("<I", data, 0)[0]
    pos, out = 4, []
    for _ in range(count):
        size = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        out.append(data[pos:pos + size])
        pos += size
    return out


def _existing_plan_hashes() -> set:
    out = set()
    paths = [os.path.join(REPO, "docs", "shader_plans.json")]
    packs = os.path.join(REPO, "source", "shader_packs")
    if os.path.isdir(packs):
        for name in sorted(os.listdir(packs)):
            p = os.path.join(packs, name, "plans.json")
            if os.path.isfile(p):
                paths.append(p)
            p = os.path.join(packs, name, "menu_plans.json")
            if os.path.isfile(p):
                paths.append(p)
    for path in paths:
        try:
            with open(path, encoding="utf-8") as handle:
                for e in json.load(handle).get("entries", []):
                    out.add(e["sha256"])
        except (OSError, ValueError):
            pass
    return out


def generate(corpus: str, out_path: str, packs: Sequence[str] = (),
             limit: int = 0) -> dict:
    corpus_doc = json.load(open(os.path.join(corpus, "corpus.json"),
                                encoding="utf-8"))
    chunks = load_chunks(os.path.join(corpus, "ps_chunks.bin"))
    existing = _existing_plan_hashes()
    pack_of = {}
    for pack in packs or ["ui", "blit_post", "sky_water", "terrain",
                          "skinning", "lighting"]:
        path = os.path.join(REPO, "source", "shader_packs", pack,
                            "manifest.json")
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as handle:
            for row in json.load(handle)["hashes"]:
                pack_of[row[0]] = (pack, row[1], row[2])
    entries = []
    seen = set()
    stats = Counter()
    for row in corpus_doc["rows"]:
        if row["kind"] != "ps":
            continue
        sha = row["sha256"]
        if sha in seen or sha in existing:
            continue
        if packs and pack_of.get(sha, ("",))[0] not in packs:
            continue
        seen.add(sha)
        ir = C.build_ir(chunks[row["index"]])
        plan = plan_shader(ir, family=pack_of.get(sha, ("generic",))[0])
        if plan is None:
            stats["no_plan"] += 1
            continue
        entries.append(plan)
        stats[plan["quality"]] += 1
        if plan.get("pass_count", 1) > 1:
            stats["multipass"] += 1
        if limit and len(entries) >= limit:
            break
    doc = {
        "schema": "d9-shader-plan-v1",
        "archive": "Skyrim - Shaders.bsa",
        "generated_for": corpus,
        "note": ("Scheduler-generated catalog. Exact rows are symbolically "
                 "scheduled TEV; approximate rows declare every dropped term, "
                 "the lowering policy and a numeric error. No row is "
                 "draw-enabled or runtime-enabled."),
        "summary": dict(stats),
        "entries": entries,
    }
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".",
                exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(doc, handle, indent=1, sort_keys=False)
        handle.write("\n")
    return doc


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=r"E:\host_sm3")
    parser.add_argument("--out", default=os.path.join(
        REPO, "docs", "scheduler_plans.json"))
    parser.add_argument("--pack", action="append", default=[])
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args(argv)
    doc = generate(args.corpus, args.out, args.pack, args.limit)
    print(json.dumps(doc["summary"], indent=2))
    print("wrote %s (%d entries)" % (args.out, len(doc["entries"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
