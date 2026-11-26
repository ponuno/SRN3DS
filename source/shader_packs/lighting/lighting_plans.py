"""Lighting pack: base-lit TEV recipes, material contracts and verification.

Scope: the two no-loop base-lit permutations in ``lighting/059/p5.fxp`` that
the phase-1 planner refuses only because of the normal-decode literal and the
normal/tangent-basis ``dp4`` prebake:

* ``base_lit``        -- p5 MAD normal, no shadow sample (5 chunks)
* ``base_lit_shadow`` -- p5 MAD normal + projected shadow mask (5 chunks)

The planner here is pack-local on purpose.  It does not change
``tools/shaders/compile.py`` or the C translator.  A plan is emitted only after
a numeric symbolic check of the contracted IR against the TEV stage expression;
``draw_enabled`` stays 0 because the runtime has no enforcement for the
material contract yet.

Contracts (also written into every plan):

* ``normal_prebaked``            NormalSampler already stores the decoded
                                 tangent normal.  The retail ``2*n-1`` (p5) or
                                 ``(n-0.5)*2`` (p1200005) decode is folded into
                                 the asset, so the TEV plan samples it directly.
* ``tangent_basis_prebaked``     the c0..c2 basis rows are prebaked into the
                                 normal map or the vertex stream.
* ``ambient_emissive_varying``   the secondary colour v2 carries
                                 ``basis * normal + c6`` (the C menu-light
                                 evaluator's ``secondary_color``).
* ``shadow_uv_projected``        the shadow sampler reads a pre-projected UV
                                 varying (retail derives it from vPos + c7).
* ``fog_off_secondary_zero``     only the shadow recipe: v2.w = 0 and the
                                 final c8.w = 1, i.e. no fog blend.  The exact
                                 shadow+fog form needs seven stages and is a
                                 named refusal, not a silent approximation.

No game bytes are stored here.  Every entry is hash + derived metadata.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import struct
import sys
from typing import Dict, List, Optional, Sequence

REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))
if REPO not in sys.path:
    sys.path.insert(0, REPO)

from tools.shaders import asm, compile as C  # noqa: E402

# TEV numeric constants (mirror tools/shaders/compile.py and 3ds/gpu/enums.h)
SRC_PRIMARY, SRC_FRAG_PRIMARY = C.SRC_PRIMARY, C.SRC_FRAG_SECONDARY
SRC_TEX0, SRC_TEX1, SRC_TEX2 = C.SRC_TEX0, C.SRC_TEX1, C.SRC_TEX2
SRC_CONST, SRC_PREV = C.SRC_CONST, C.SRC_PREV
RGB_COLOR, RGB_ALPHA = C.RGB_COLOR, C.RGB_ALPHA
RGB_R = C.RGB_R
A_ALPHA, A_B = C.A_ALPHA, C.A_B
F_REPLACE, F_MODULATE, F_DOT3_RGBA = (
    C.FUNC_REPLACE, C.FUNC_MODULATE, C.FUNC_DOT3_RGBA)
F_MULTIPLY_ADD, F_INTERPOLATE = C.FUNC_MULTIPLY_ADD, C.FUNC_INTERPOLATE

BASE_LIT_SHA = "8699c0204453a1dd2189a7252766f2a9591b296370467869b106e049576ea9f1"
BASE_LIT_SHADOW_SHA = "a9942b49966b60249d8a9aeb4e049d1976086fe7b9293168f70c25aeb8477642"

#: Exact opcode body of the two accepted permutations (def excluded: index 0).
BASE_LIT_BODY = [0x42, 0x05, 0x05, 0x42, 0x04, 0x01, 0x09, 0x09, 0x09, 0x08,
                 0x02, 0x04, 0x05, 0x05, 0x01, 0x04, 0x04, 0x05]
BASE_LIT_SHADOW_BODY = [0x42, 0x05, 0x05, 0x04, 0x42, 0x05, 0x42, 0x04, 0x01,
                        0x09, 0x09, 0x09, 0x08, 0x02, 0x04, 0x05, 0x05, 0x01,
                        0x04, 0x04, 0x05]
BASE_LIT_DEF = (2.0, -1.0, 1.0, 0.0)


def _stage(func_rgb, func_a, src_rgb, src_a, op_rgb, op_a,
           const_reg=255, scale_rgb=0, scale_a=0) -> dict:
    return {
        "func_rgb": func_rgb, "func_a": func_a,
        "src_rgb": list(src_rgb), "src_a": list(src_a),
        "op_rgb": list(op_rgb), "op_a": list(op_a),
        "scale_rgb": scale_rgb, "scale_a": scale_a,
        "const_reg": const_reg,
        "const_is_literal": 0, "const_rgba8": 0,
    }


def _color3(op=RGB_COLOR):
    return [op, op, op]


def _alpha3(op=A_ALPHA):
    return [op, op, op]


def base_lit_plan(ir: C.IR) -> Optional[dict]:
    """Six-stage exact-under-contract plan for the p5 base-lit hash."""
    body = [i.opcode for i in ir.body]
    if body[:1] != [0x51] or body[1:] != BASE_LIT_BODY:
        return None
    if ir.sha256 != BASE_LIT_SHA:
        return None
    if tuple(ir.defs.get(8, ())) != BASE_LIT_DEF:
        return None
    if ir.samplers != {0: 2, 1: 2}:
        return None
    if {r: (d["usage"], d["usage_index"]) for r, d in ir.inputs.items()} != {
            0: (5, 0), 1: (10, 0), 2: (10, 1)}:
        return None
    stages = [
        # 0: NdotL = dot3(prebaked tangent normal, c3), clamped by the LDR model.
        _stage(F_DOT3_RGBA, F_DOT3_RGBA,
               [SRC_TEX1, SRC_CONST, SRC_CONST],
               [SRC_TEX1, SRC_CONST, SRC_CONST],
               _color3(), _alpha3(), const_reg=3),
        # 1: light = c4 * NdotL + (basis*normal + c6) -> v2 by contract.
        _stage(F_MULTIPLY_ADD, F_MULTIPLY_ADD,
               [SRC_PREV, SRC_CONST, SRC_FRAG_PRIMARY],
               [SRC_PREV, SRC_CONST, SRC_FRAG_PRIMARY],
               _color3(), _alpha3(), const_reg=4),
        # 2: rgb = diffuse * light; alpha = diffuse.a * vertex.a.
        _stage(F_MODULATE, F_MODULATE,
               [SRC_TEX0, SRC_PREV, SRC_PREV],
               [SRC_TEX0, SRC_PRIMARY, SRC_PREV],
               _color3(), _alpha3()),
        # 3: rgb *= vertex.rgb; alpha *= material alpha c5.z.
        _stage(F_MODULATE, F_MODULATE,
               [SRC_PREV, SRC_PRIMARY, SRC_PREV],
               [SRC_PREV, SRC_CONST, SRC_PREV],
               _color3(), [A_ALPHA, A_B, A_ALPHA], const_reg=5),
        # 4: rgb = lerp(rgb, v2, v2.a); alpha passes through.
        _stage(F_INTERPOLATE, F_REPLACE,
               [SRC_PREV, SRC_FRAG_PRIMARY, SRC_FRAG_PRIMARY],
               [SRC_PREV, SRC_PREV, SRC_PREV],
               [RGB_COLOR, RGB_COLOR, RGB_ALPHA], _alpha3()),
        # 5: rgb *= c7.a; alpha passes through.
        _stage(F_MODULATE, F_REPLACE,
               [SRC_PREV, SRC_CONST, SRC_PREV],
               [SRC_PREV, SRC_PREV, SRC_PREV],
               [RGB_COLOR, RGB_ALPHA, RGB_COLOR], _alpha3(), const_reg=7),
    ]
    return _plan_doc(ir, "lighting.base_lit.ps", "base_lit", stages,
                     textures=[(0, 0), (0, 1)],
                     constants=[3, 4, 5, 7], inputs=[(1, 0), (2, 1)],
                     preconditions=[
                         "normal_prebaked", "tangent_basis_prebaked",
                         "ambient_emissive_varying", "material_constants_c0_c7",
                         "ldr_quality", "texture0_2d", "texture1_2d",
                         "vertex_color",
                     ],
                     note=("six-stage base-lit TEV recipe; draw needs prebaked "
                           "normal/basis and ambient+emissive in v2"))


def base_lit_shadow_plan(ir: C.IR) -> Optional[dict]:
    """Five-stage shadow recipe; the exact fog form needs seven stages."""
    body = [i.opcode for i in ir.body]
    if body[:1] != [0x51] or body[1:] != BASE_LIT_SHADOW_BODY:
        return None
    if ir.sha256 != BASE_LIT_SHADOW_SHA:
        return None
    if tuple(ir.defs.get(9, ())) != BASE_LIT_DEF:
        return None
    if ir.samplers != {0: 2, 1: 2, 2: 2}:
        return None
    if {r: (d["usage"], d["usage_index"]) for r, d in ir.inputs.items()} != {
            0: (5, 0), 1: (10, 0), 2: (10, 1)}:
        return None
    stages = [
        # 0: NdotL = dot3(prebaked tangent normal, c3); alpha carries it too.
        _stage(F_DOT3_RGBA, F_DOT3_RGBA,
               [SRC_TEX1, SRC_CONST, SRC_CONST],
               [SRC_TEX1, SRC_CONST, SRC_CONST],
               _color3(), _alpha3(), const_reg=3),
        # 1: rgb = shadow.x * c4; alpha keeps NdotL.
        _stage(F_MODULATE, F_REPLACE,
               [SRC_TEX2, SRC_CONST, SRC_PREV],
               [SRC_PREV, SRC_PREV, SRC_PREV],
               [RGB_R, RGB_COLOR, RGB_COLOR], _alpha3(), const_reg=4),
        # 2: rgb = (shadow*c4) * NdotL + v2; alpha passes.
        _stage(F_MULTIPLY_ADD, F_REPLACE,
               [SRC_PREV, SRC_PREV, SRC_FRAG_PRIMARY],
               [SRC_PREV, SRC_PREV, SRC_PREV],
               [RGB_COLOR, RGB_ALPHA, RGB_COLOR], _alpha3()),
        # 3: rgb = diffuse * light; alpha = diffuse.a * vertex.a.
        _stage(F_MODULATE, F_MODULATE,
               [SRC_TEX0, SRC_PREV, SRC_PREV],
               [SRC_TEX0, SRC_PRIMARY, SRC_PREV],
               _color3(), _alpha3()),
        # 4: rgb *= vertex.rgb; alpha *= c5.z.
        _stage(F_MODULATE, F_MODULATE,
               [SRC_PREV, SRC_PRIMARY, SRC_PREV],
               [SRC_PREV, SRC_CONST, SRC_PREV],
               _color3(), [A_ALPHA, A_B, A_ALPHA], const_reg=5),
    ]
    return _plan_doc(ir, "lighting.base_lit_shadow.ps", "base_lit_shadow", stages,
                     textures=[(0, 0), (0, 1), (4, 2)],
                     constants=[3, 4, 5], inputs=[(1, 0), (2, 1)],
                     preconditions=[
                         "normal_prebaked", "tangent_basis_prebaked",
                         "ambient_emissive_varying", "shadow_uv_projected",
                         "shadow_mask_2d", "fog_off_secondary_zero",
                         "material_constants_c0_c7", "ldr_quality",
                         "texture0_2d", "texture1_2d", "vertex_color",
                     ],
                     note=("five-stage base-lit+shadow recipe under a fog-off "
                           "contract; exact shadow+fog needs 7 TEV stages"))


def _plan_doc(ir, ident, family, stages, textures, constants, inputs,
              preconditions, note) -> dict:
    return {
        "id": ident,
        "kind": "tev",
        "family": "lighting",
        "recipe": family,
        "stage": "ps_3_0",
        "sha256": ir.sha256,
        "sha1": ir.sha1,
        "version": "0x%08x" % ir.version,
        "instructions": len(ir.body) + len(ir.program.declarations),
        "draw_enabled": 0,
        "stages": len(stages),
        "stage_data": stages,
        "textures": [
            {"texcoord_reg": tc, "sampler_reg": s, "sampler_type": 2}
            for tc, s in textures
        ],
        "inputs": [
            {"usage": 10, "usage_index": i, "reg_index": r,
             "tev_source": SRC_PRIMARY if i == 0 else SRC_FRAG_PRIMARY}
            for r, i in inputs
        ],
        "constants": constants,
        "uses_vertex_color": 1,
        "requires_alpha_fixup": 0,
        "ldr_clamped": 1,
        "preconditions": preconditions,
        "note": note,
    }


# ---------------------------------------------------------------------------
# Numeric symbolic check of the contracted IR against the TEV stage list.
# ---------------------------------------------------------------------------

def _clamp01(v: float) -> float:
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


def _vec(values: Sequence[float]) -> List[float]:
    return [float(v) for v in values]


def _sel(vec: Sequence[float], op: int, out_ch: int, alpha: bool) -> float:
    if alpha:
        return {A_ALPHA: vec[3], 2: vec[0], 4: vec[1], 6: vec[2],
                3: 1.0 - vec[0], 5: 1.0 - vec[1], 7: 1.0 - vec[2],
                1: 1.0 - vec[3]}.get(op, vec[3])
    if op == RGB_COLOR:
        return vec[out_ch]
    if op == 1:
        return 1.0 - vec[out_ch]
    return {RGB_ALPHA: vec[3], RGB_R: vec[0], 5: 1.0 - vec[0],
            8: vec[1], 9: 1.0 - vec[1], 12: vec[2],
            13: 1.0 - vec[2]}.get(op, vec[out_ch])


def eval_plan(plan: dict, tex: Sequence[Sequence[float]],
              consts: Dict[int, Sequence[float]],
              primary: Sequence[float],
              frag: Sequence[float]) -> List[float]:
    prev = [0.0, 0.0, 0.0, 0.0]
    for stage in plan["stage_data"]:
        def src(src_id, op, ch, alpha):
            if src_id == SRC_PREV:
                return _sel(prev, op, ch, alpha)
            if src_id == SRC_PRIMARY:
                return _sel(primary, op, ch, alpha)
            if src_id == SRC_FRAG_PRIMARY:
                return _sel(frag, op, ch, alpha)
            if SRC_TEX0 <= src_id <= SRC_TEX2:
                return _sel(tex[src_id - SRC_TEX0], op, ch, alpha)
            if src_id == SRC_CONST:
                reg = stage["const_reg"]
                if reg == 255:
                    reg = 0
                return _sel(consts[reg], op, ch, alpha)
            raise AssertionError("source %r" % (src_id,))

        out = [0.0] * 4
        if stage["func_rgb"] in (F_DOT3_RGBA, C.FUNC_DOT3_RGB):
            a = [src(stage["src_rgb"][0], stage["op_rgb"][0], c, False)
                 for c in range(3)]
            b = [src(stage["src_rgb"][1], stage["op_rgb"][1], c, False)
                 for c in range(3)]
            value = _clamp01(sum(x * y for x, y in zip(a, b)))
            out[0] = out[1] = out[2] = value
        else:
            a = [src(stage["src_rgb"][0], stage["op_rgb"][0], c, False)
                 for c in range(3)]
            b = [src(stage["src_rgb"][1], stage["op_rgb"][1], c, False)
                 for c in range(3)]
            c3 = [src(stage["src_rgb"][2], stage["op_rgb"][2], c, False)
                  for c in range(3)]
            for ch in range(3):
                out[ch] = _func(stage["func_rgb"], a[ch], b[ch], c3[ch])
        if stage["func_a"] == F_DOT3_RGBA:
            out[3] = out[0]
        elif stage["func_a"] == F_REPLACE:
            out[3] = src(stage["src_a"][0], stage["op_a"][0], 3, True)
        else:
            a = src(stage["src_a"][0], stage["op_a"][0], 3, True)
            b = src(stage["src_a"][1], stage["op_a"][1], 3, True)
            c3 = src(stage["src_a"][2], stage["op_a"][2], 3, True)
            out[3] = _func(stage["func_a"], a, b, c3)
        scale = (1.0, 2.0, 4.0)[stage["scale_rgb"]]
        out[:3] = [_clamp01(v * scale) for v in out[:3]]
        out[3] = _clamp01(out[3] * (1.0, 2.0, 4.0)[stage["scale_a"]])
        prev = out
    return prev


def _func(func, a, b, c):
    if func == F_REPLACE:
        return a
    if func == F_MODULATE:
        return a * b
    if func == C.FUNC_ADD:
        return a + b
    if func == C.FUNC_SUBTRACT:
        return a - b
    if func == F_MULTIPLY_ADD:
        return a * b + c
    if func == F_INTERPOLATE:
        return a * (1.0 - c) + b * c
    raise AssertionError("combine function %r" % (func,))


BASIS = object()


def eval_ir_contract(ir: C.IR, tex, consts, primary, frag,
                     shadow: bool) -> List[float]:
    """Interpret the recognised p5 IR under the material contract.

    Contract substitutions, one per TEV gap:

    * normal decode (``mad/add`` with the c8/c9 literal) is skipped because
      ``NormalSampler`` already stores the decoded tangent normal;
    * the c0..c2 ``dp4`` basis rows are replaced by a ``BASIS`` marker and the
      following ``basis + c6`` add becomes v2 (``ambient_emissive_varying``);
    * the shadow sampler's projected UV is supplied by the connector, so the
      sampler value is used directly (``shadow_uv_projected``).
    """
    temps: Dict[int, List] = {}
    out = [0.0, 0.0, 0.0, 1.0]
    defs = {k: _vec(v) for k, v in ir.defs.items()}

    def value(src):
        if src.reg_type == asm.REG_TEMP:
            vec = list(temps.get(src.index, [BASIS] * 4))
        elif src.reg_type == asm.REG_INPUT:
            vec = list(primary if src.index == 1 else
                       (frag if src.index == 2 else [0.0] * 4))
        elif src.reg_type == asm.REG_CONST:
            vec = list(defs.get(src.index, _vec(consts[src.index])))
        else:
            raise AssertionError("source register type %r" % src.reg_type)
        comps = [("xyzw".index(src.swizzle[c])) for c in range(4)]
        vec = [vec[c] for c in comps]
        if src.modifier == 1:
            vec = [-v if v is not BASIS else BASIS for v in vec]
        elif src.modifier == 8:
            vec = [abs(v) if v is not BASIS else BASIS for v in vec]
        elif src.modifier != 0:
            raise AssertionError("source modifier %r" % src.modifier)
        return vec

    def is_normal_decode(ins):
        # p5: mad temp.xyz, temp.xzyw, c8.x, c8.y  /  mov temp.w, c8.z
        # p1200005: add temp.xyz, temp.xzyw, c8.x  (twice)  / mov temp.w, c8.y
        if ins.opcode == 0x04 and ins.srcs and ins.srcs[0].reg_type not in (
                asm.REG_TEMP, asm.REG_INPUT, asm.REG_CONST):
            # Projected shadow UV: vPos * c7 + c7.zw under shadow_uv_projected.
            return True
        if ins.opcode not in (0x04, 0x02, 0x01) or ins.dst is None:
            return False
        if ins.dst.reg_type != asm.REG_TEMP or ins.dst.index not in (1, 2):
            return False
        consts_used = [s for s in ins.srcs if s.reg_type == asm.REG_CONST]
        return bool(consts_used) and all(
            s.index in (8, 9) for s in consts_used)

    for ins in ir.body:
        if ins.opcode in (0x51, 0x00):
            continue
        if is_normal_decode(ins):
            if os.environ.get("LIGHTING_TRACE"):
                print("skip", ins.text())
            continue
        if ins.opcode == 0x42:
            vec = _vec(tex[ins.srcs[1].index])
        elif ins.opcode == 0x01:
            vec = value(ins.srcs[0])
        elif ins.opcode in (0x02, 0x05):
            a, b = value(ins.srcs[0]), value(ins.srcs[1])
            vec = [BASIS if x is BASIS or y is BASIS else
                   (x + y if ins.opcode == 0x02 else x * y)
                   for x, y in zip(a, b)]
        elif ins.opcode == 0x04:
            a = value(ins.srcs[0])
            b = value(ins.srcs[1])
            c = value(ins.srcs[2])
            vec = [BASIS if x is BASIS or y is BASIS or z is BASIS else
                   x * y + z for x, y, z in zip(a, b, c)]
        elif ins.opcode == 0x08:
            a, b = value(ins.srcs[0]), value(ins.srcs[1])
            d = _clamp01(a[0] * b[0] + a[1] * b[1] + a[2] * b[2])
            if ins.dst.modifier == 1:
                d = _clamp01(d)
            vec = [d, d, d, d]
        elif ins.opcode == 0x09:
            vec = [BASIS] * 4
        else:
            raise AssertionError("opcode %s" % ins.name)

        # ambient_emissive_varying: basis-product + c6 is supplied as v2.
        if any(v is BASIS for v in vec) and ins.opcode != 0x09:
            sources = [s for s in ins.srcs if s.reg_type == asm.REG_CONST]
            if sources and all(s.index == 6 for s in sources):
                vec = [frag[c] if v is BASIS else v for c, v in enumerate(vec)]
        if any(v is BASIS for v in vec) and ins.opcode != 0x09:
            raise AssertionError("uncontracted basis product on %s" % ins.name)

        if ins.dst is None:
            continue
        if os.environ.get("LIGHTING_TRACE"):
            mask = "".join("xyzw"[c] for c in range(4) if ins.dst.mask & (1 << c))
            print("%-28s %s -> %s" % (ins.text(), mask,
                                      ["B" if v is BASIS else round(v, 4)
                                       for v in vec]))
        if ins.dst.reg_type == asm.REG_COLOROUT:
            for ch in range(4):
                if ins.dst.mask & (1 << ch):
                    out[ch] = _clamp01(vec[ch])
        elif ins.dst.reg_type == asm.REG_TEMP:
            previous = temps.get(ins.dst.index, [0.0] * 4)
            merged = list(previous)
            for ch in range(4):
                if ins.dst.mask & (1 << ch):
                    merged[ch] = vec[ch]
            temps[ins.dst.index] = merged
    return out


def verify(ir: C.IR, plan: dict, shadow: bool, samples: int = 256) -> dict:
    """Numeric check under the ``ldr_quality`` contract.

    Textures, vertex colours and all non-directional material constants are in
    [0,1]; the light colour is in [0,0.5] so ``c4*NdotL + ambient`` stays in
    [0,1].  That is the same LDR premise the phase-1 verifier's ``_norm`` uses:
    no intermediate TEV clamp can change the final value.
    """
    rng = random.Random(0x1F5 if shadow else 0x1F4)
    worst = 0.0
    for n in range(samples):
        def unit():
            return rng.random()

        tex = [[unit() for _ in range(4)] for _ in range(3)]
        consts = {r: [rng.uniform(-1.0, 1.0) for _ in range(4)]
                  for r in range(9)}
        for reg in (4, 5, 6, 7, 8):
            consts[reg] = [unit() for _ in range(4)]
        consts[4] = [rng.uniform(0.0, 0.5) for _ in range(4)]
        consts[3] = [rng.uniform(-1.0, 1.0) for _ in range(3)] + [0.0]
        primary = [unit() for _ in range(3)] + [unit()]
        frag = [rng.uniform(0.0, 0.5) for _ in range(3)] + [
            (0.0 if shadow else unit())]
        if shadow:
            consts[8][3] = 1.0    # fog_off_secondary_zero: final c8.w = 1
        want = eval_ir_contract(ir, tex, consts, primary, frag, shadow)
        got = eval_plan(plan, tex, consts, primary,
                        frag if not shadow else
                        [frag[0], frag[1], frag[2], 0.0])
        for a, b in zip(want, got):
            worst = max(worst, abs(a - b))
    return {"samples": samples, "max_abs_error": worst,
            "ok": worst < 1e-6,
            "model": "ldr_quality; no intermediate clamp divergence"}


def recognise(ir: C.IR):
    plan = base_lit_plan(ir)
    if plan is not None:
        return plan, False
    plan = base_lit_shadow_plan(ir)
    if plan is not None:
        return plan, True
    return None, False


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


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=r"E:\host_sm3")
    parser.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "plans.json"))
    args = parser.parse_args(argv)
    with open(os.path.join(args.corpus, "corpus.json"), encoding="utf-8") as handle:
        corpus = json.load(handle)
    chunks = load_chunks(os.path.join(args.corpus, "ps_chunks.bin"))
    by_hash = {}
    counts = {}
    for row in corpus["rows"]:
        if row["kind"] != "ps":
            continue
        if row["family"] not in ("lighting/deferred/shadows",
                                 "particles/effects"):
            continue
        by_hash.setdefault(row["sha256"], row)
        counts[row["sha256"]] = counts.get(row["sha256"], 0) + 1
    entries, report = [], []
    for sha, row in sorted(by_hash.items()):
        ir = C.build_ir(chunks[row["index"]])
        plan, shadow = recognise(ir)
        if plan is None:
            continue
        check = verify(ir, plan, shadow)
        if not check["ok"]:
            raise SystemExit("%s: plan mismatch %.3g" % (sha, check["max_abs_error"]))
        entries.append(plan)
        report.append({"sha256": sha, "chunks": counts[sha],
                       "recipe": plan["recipe"], "stages": plan["stages"],
                       "verify": check})
    doc = {
        "schema": "d9-shader-plan-v1",
        "archive": "Skyrim - Shaders.bsa",
        "pack": "lighting",
        "note": ("Pack-owned TEV recipes, merged by tools/fxp_compile.py.  "
                 "None is draw-certified: draw_enabled is 0, no runtime slot "
                 "is claimed, and every row lists its material contract."),
        "entries": entries,
        "summary": {
            "recipes": len(entries),
            "chunks": sum(row["chunks"] for row in report),
            "hashes": len(entries),
            "runtime": 0,
        },
        "report": report,
    }
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(doc, handle, indent=1)
        handle.write("\n")
    print(json.dumps(doc["summary"], indent=2))
    for row in report:
        print("%-22s %2d chunks %d stages %s" % (
            row["recipe"], row["chunks"], row["stages"], row["sha256"][:16]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
