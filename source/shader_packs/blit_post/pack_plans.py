"""Generate the blit_post pack catalog from the host SM3 corpus.

This is a *pack-local* tool: it reads the host-only census dump
(``E:\\host_sm3`` by default), never the retail archive, and writes
``plans.json`` / ``refusals.json`` next to this file.  No game bytes are
stored; only the SHA-256 identity and derived descriptors are emitted.

Plan policy (phase 1, port-lite):

* ``quality: "exact"`` - the declared TEV descriptor reproduces the declared
  simplified SM3 spec.  Only these carry ``draw_enabled: 1``.
* ``quality: "approx"`` - the descriptor is a deliberate simplification.  The
  ``approximation`` field states what was dropped (tap count, LUT slices,
  eye-adaptation state) and ``draw_enabled`` stays 0.
* ``runtime`` is 0 for every entry.  Promotion is a separate, measured step.

Usage::

    python source/shader_packs/blit_post/pack_plans.py
    python source/shader_packs/blit_post/pack_plans.py --corpus E:\\host_sm3 --check
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from shaders import asm  # noqa: E402

DEFAULT_CORPUS = r"E:\host_sm3"
ARCHIVE = "Skyrim - Shaders.bsa"

# PICA TEV source/selector values (<3ds/gpu/enums.h>, mirrored by
# tools/shaders/compile.py).
SRC_PRIMARY = 0x00
SRC_TEX0, SRC_TEX1 = 0x03, 0x04
SRC_CONST = 0x0E
SRC_PREV = 0x0F
RGB_COLOR, RGB_ALPHA = 0x00, 0x02
A_ALPHA = 0x00
FUNC_REPLACE, FUNC_MODULATE, FUNC_INTERPOLATE = 0, 1, 4
FUNC_DOT3_RGB = 6


def _pack_rgba8(vals) -> int:
    out = 0
    for i, v in enumerate(vals):
        q = int(round(max(0.0, min(1.0, v)) * 255.0))
        out |= q << (8 * i)
    return out


def _lit_stage(func_rgb, src_rgb, op_rgb, func_a, src_a, op_a, rgba):
    return {
        "func_rgb": func_rgb, "func_a": func_a,
        "src_rgb": list(src_rgb), "src_a": list(src_a),
        "op_rgb": list(op_rgb), "op_a": list(op_a),
        "scale_rgb": 0, "scale_a": 0,
        "const_reg": 255, "const_is_literal": 1,
        "const_rgba8": _pack_rgba8(rgba),
    }


def _const_stage(rgba):
    return _lit_stage(FUNC_REPLACE, [SRC_CONST, SRC_PREV, SRC_PREV],
                      [RGB_COLOR] * 3, FUNC_REPLACE,
                      [SRC_CONST, SRC_PREV, SRC_PREV], [A_ALPHA] * 3, rgba)


def _copy_stage():
    return {
        "func_rgb": FUNC_REPLACE, "func_a": FUNC_REPLACE,
        "src_rgb": [SRC_TEX0, SRC_PREV, SRC_PREV],
        "src_a": [SRC_TEX0, SRC_PREV, SRC_PREV],
        "op_rgb": [RGB_COLOR] * 3, "op_a": [A_ALPHA] * 3,
        "scale_rgb": 0, "scale_a": 0,
        "const_reg": 255, "const_is_literal": 0, "const_rgba8": 0,
    }


def _tex(n):
    return [{"sampler_reg": i, "sampler_type": 2, "texcoord_reg": 0}
            for i in range(n)]


def _tev(sha, note, stage_data, textures, quality="exact",
         approximation=None, draw=0, instructions=0):
    entry = {
        "id": "blit.ps.%s" % sha[:8],
        "sha256": sha,
        "stage": "ps_3_0",
        "kind": "tev",
        "draw_enabled": draw,
        "family": "blit_post",
        "version": "0xffff0300",
        "instructions": instructions,
        "note": note,
        "quality": quality,
        "runtime": 0,
        "ldr_clamped": 1,
        "requires_alpha_fixup": 0,
        "uses_vertex_color": 0,
        "stages": len(stage_data),
        "stage_data": stage_data,
        "textures": textures,
        "constants": [],
    }
    if approximation:
        entry["approximation"] = approximation
    if draw:
        entry["preconditions"] = ["exact_hash_descriptor_match",
                                  "one_stage_tev_contract",
                                  "runtime_slot_promotion_pending"]
    return entry


def _pica_vs(sha, program, note):
    inputs, outputs = [], []
    for decl in program.declarations:
        row = {
            "usage": decl.usage,
            "usage_index": decl.usage_index,
            "usage_name": asm.USAGE_NAMES.get(decl.usage, "unknown"),
            "reg_index": decl.reg_index,
        }
        if decl.reg_type == asm.REG_INPUT:
            inputs.append(row)
        elif decl.reg_type == asm.REG_OUTPUT:
            row["type"] = 6
            outputs.append(row)
    consts = {i.srcs[0].index for i in program.instructions
              for _ in [0] if i.srcs}
    uniform = (max(consts) + 1) if consts else 0
    return {
        "id": "blit.vs.%s" % sha[:8],
        "sha256": sha,
        "stage": "vs_3_0",
        "kind": "pica_vs",
        "draw_enabled": 0,
        "family": "blit_post",
        "version": "0xfffe0300",
        "instructions": len(program.instructions),
        "note": note,
        "quality": "contract",
        "runtime": 0,
        "inputs": inputs,
        "outputs": outputs,
        "uniform_registers": uniform,
        "codegen": "pending",
        "preconditions": ["pica_vs_codegen", "exact_hash_descriptor_match",
                          "fullscreen_quad_semantics"],
    }


# --- IR-driven plans for the former refusals ------------------------------
# Every census hash in this pack now has a descriptor.  These plans are
# deliberately approximate: they keep the dominant sample/op and record the
# dropped work in ``approximation``.  ``constant_ops`` folds an out-of-range
# def literal into a derived c0..c7 register; ``passes`` replays accumulators
# that need more than one TEV pass.
def _mod_stage(tex=SRC_TEX0, const_reg=255, literal=True, rgba=(1, 1, 1, 1)):
    return {
        "func_rgb": FUNC_MODULATE, "func_a": FUNC_MODULATE,
        "src_rgb": [tex, SRC_CONST, SRC_PREV],
        "src_a": [tex, SRC_CONST, SRC_PREV],
        "op_rgb": [RGB_COLOR] * 3, "op_a": [A_ALPHA] * 3,
        "scale_rgb": 0, "scale_a": 0,
        "const_reg": const_reg, "const_is_literal": 1 if literal else 0,
        "const_rgba8": _pack_rgba8(rgba) if literal else 0,
    }


def _lerp_stage(const_reg=255, literal=True, rgba=(.5, .5, .5, .5)):
    return _lit_stage(
        FUNC_INTERPOLATE, [SRC_TEX0, SRC_TEX1, SRC_CONST],
        [RGB_COLOR, RGB_COLOR, RGB_ALPHA],
        FUNC_INTERPOLATE, [SRC_TEX0, SRC_TEX1, SRC_CONST],
        [A_ALPHA, A_ALPHA, A_ALPHA],
        rgba if literal else (.5, .5, .5, .5)) | {
            "const_reg": const_reg,
            "const_is_literal": 1 if literal else 0,
            "const_rgba8": _pack_rgba8(rgba) if literal else 0,
        }


def _luma_stage(const_reg=255, literal=True,
                rgba=(.2125, .7154, .0721, 1)):
    return _lit_stage(
        FUNC_DOT3_RGB, [SRC_TEX0, SRC_CONST, SRC_PREV],
        [RGB_COLOR] * 3, FUNC_REPLACE,
        [SRC_TEX0, SRC_PREV, SRC_PREV], [A_ALPHA] * 3,
        rgba if literal else (.2125, .7154, .0721, 1)) | {
            "const_reg": const_reg,
            "const_is_literal": 1 if literal else 0,
            "const_rgba8": _pack_rgba8(rgba) if literal else 0,
        }


def _two_pass(entry):
    """Mirror the main pass and accumulate the same tap with ADD blending."""
    main = entry["stage_data"]
    first = {
        "stage_data": copy.deepcopy(main),
        "textures": copy.deepcopy(entry["textures"]),
        "constants": list(entry["constants"]),
        "inputs": [],
        "blend": {"enable": 0, "src": 2, "dst": 1, "op": 1},
        "depth_write": 0,
    }
    second = {
        "stage_data": copy.deepcopy(main),
        "textures": copy.deepcopy(entry["textures"]),
        "constants": list(entry["constants"]),
        "inputs": [],
        "blend": {"enable": 1, "src": 2, "dst": 2, "op": 1},
        "depth_write": 1,
    }
    entry["passes"] = [first, second]
    return entry


_DEF_RE = re.compile(r"def\w* c(\d+) \{([^}]*)\}")


def _out_of_range_def(program):
    """First ``def`` lane outside RGBA8 [0,1], with its text and sign."""
    for ins in program.instructions:
        if ins.name not in ("def", "defi", "defb"):
            continue
        match = _DEF_RE.search(ins.text())
        if not match:
            continue
        try:
            values = [float(v) for v in match.group(2).split(",")]
        except ValueError:
            continue
        for lane, value in enumerate(values):
            if value < 0.0 or value > 1.0:
                text = ins.text()
                if text.startswith("def "):
                    text = text[4:]
                return "xyzw"[lane], value < 0.0, text
    return "x", False, ""


def _refusal_ps_plan(sha, row, program):
    """Build one approximate descriptor for a census row without a plan."""
    cls = row["class"] or "unclassified"
    samples = program.texture_samples()
    names = {ins.name for ins in program.instructions}
    ntex = max(1, min(3, sum(1 for c in program.constants
                             if c.register_set == 3)))
    loops = program.loops()
    entry = None

    if cls == "def_literal_out_of_range":
        lane, negate, literal = _out_of_range_def(program)
        derived = [{
            "dst_reg": 7, "op": "pack",
            "src": [{"reg": 0, "swizzle": lane * 4, "negate": 1 if negate else 0}],
        }]
        if samples > 6 or loops or "ifc" in names:
            stage = [_mod_stage(const_reg=7, literal=False)]
            entry = _tev(sha, "derived/uniform constant plan (%d samples)"
                         % samples, stage, _tex(1), "approx",
                         "def %s leaves RGBA8 [0,1]; the offending lane is "
                         "folded into derived c7 (constant_ops pack of c0, "
                         "negate=%d) instead of a literal. %d samples/loops/"
                         "branches collapse to one weighted centre tap replayed "
                         "through two accumulating passes."
                         % (literal or "literal", 1 if negate else 0, samples),
                         0, len(program.instructions))
            entry["constant_ops"] = derived
            entry["constants"] = [0, 7]
            entry.setdefault("preconditions", []).extend(
                ["derived_constant_source_uploaded", "def_literal_uniform_fold"])
            _two_pass(entry)
        elif "dp3" in names:
            entry = _tev(sha, "derived constant luma plan",
                         [_luma_stage(const_reg=7, literal=False)], _tex(1),
                         "approx", "def %s leaves RGBA8 [0,1] and is folded "
                         "into derived c7 (constant_ops of c0); DOT3 luma is "
                         "kept, the %d extra taps are dropped."
                         % (literal or "literal", max(0, samples - 1)),
                         0, len(program.instructions))
            entry["constant_ops"] = derived
            entry["constants"] = [0, 7]
            entry.setdefault("preconditions", []).extend(
                ["derived_constant_source_uploaded", "def_literal_uniform_fold"])
        elif ntex >= 2 or "lrp" in names:
            entry = _tev(sha, "derived constant two-texture lerp",
                         [_lerp_stage(const_reg=7, literal=False)], _tex(2),
                         "approx", "def %s leaves RGBA8 [0,1] and is folded "
                         "into derived c7 (constant_ops of c0); cmp/lrp "
                         "selects collapse to a fixed 0.5 two-texture "
                         "INTERPOLATE, %d-tap chain reduced."
                         % (literal or "literal", samples),
                         0, len(program.instructions))
            entry["constant_ops"] = derived
            entry["constants"] = [0, 7]
            entry.setdefault("preconditions", []).extend(
                ["derived_constant_source_uploaded", "def_literal_uniform_fold"])
        else:
            entry = _tev(sha, "derived constant tint/copy plan",
                         [_mod_stage(const_reg=7, literal=False)], _tex(1),
                         "approx", "def %s leaves RGBA8 [0,1] and is folded "
                         "into derived c7 (constant_ops of c0); the remaining "
                         "arithmetic is approximated by a single tinted "
                         "centre sample." % (literal or "literal"),
                         0, len(program.instructions))
            entry["constant_ops"] = derived
            entry["constants"] = [0, 7]
            entry.setdefault("preconditions", []).extend(
                ["derived_constant_source_uploaded", "def_literal_uniform_fold"])

    elif cls == "dataflow":
        entry = _tev(sha, "two-pass accumulator for a %d-tap dataflow blur"
                     % samples, [_mod_stage(literal=True,
                                            rgba=(.5, .5, .5, .5))],
                     _tex(1), "approx",
                     "%d-tap separable blur accumulator replaced by one "
                     "centred tap; the two passes each contribute 0.5 with "
                     "ADD blending, so the sum stays 1.0. The per-tap offset "
                     "constants c1..cN cannot be folded into TEV, so %d taps "
                     "are dropped." % (samples, max(0, samples - 1)),
                     0, len(program.instructions))
        _two_pass(entry)

    elif cls == "unsupported_opcode":
        if "lrp" in names or ntex >= 2:
            entry = _tev(sha, "cmp/lrp select collapsed to a two-texture lerp",
                         [_lerp_stage()], _tex(2), "approx",
                         "cmp/lrp select chain in a %d-texture imagespace "
                         "shader collapsed to a fixed 0.5 INTERPOLATE of s0/"
                         "s1; rcp/abs/derivative weights and %d taps are "
                         "dropped%s." % (ntex, max(0, samples - 2),
                                         "" if ntex <= 2 else ", s2 unbound"),
                         0, len(program.instructions))
        elif "dp3" in names:
            entry = _tev(sha, "luma kept, cmp tone curve approximated",
                         [_luma_stage()], _tex(1), "approx",
                         "DOT3 luma kept; the cmp/lerp tone curve and %d "
                         "extra taps collapse to a fixed exposure, so bloom/"
                         "adaptation weighting is lost." % max(0, samples - 1),
                         0, len(program.instructions))
        else:
            entry = _tev(sha, "single centred tap after opcode lowering",
                         [_mod_stage()], _tex(1), "approx",
                         "rcp/abs/cmp/texldd lowering approximated by one "
                         "centred sample; %d taps and the select chain are "
                         "dropped." % max(0, samples - 1),
                         0, len(program.instructions))

    elif cls == "tev_prebake":
        entry = _tev(sha, "prebaked TEV state plus DOT3 luma",
                     [_luma_stage()], _tex(1), "approx",
                     "dp4 with the guest ColorSelect constant is collapsed "
                     "to DOT3 luma and tex.w alpha; the tev_prebake selector "
                     "state is a recorded precondition, not read back.",
                     0, len(program.instructions))
        entry["preconditions"] = ["tev_prebake_guest_state",
                                  "colorselect_dp4_alpha_channel_kept"]

    elif cls == "input_not_a_colour_varying":
        entry = _tev(sha, "texcoord1 scalar rebound as TEV primary colour",
                     [_mod_stage(tex=SRC_PRIMARY, literal=True,
                                 rgba=(0, 1, 0, 0))], [],
                     "approx", "the source input is texcoord1, not a colour "
                     "varying; the scalar is bound as the TEV primary colour "
                     "and multiplied by literal (0,1,0,0). This matches only "
                     "when the varying carries 0/1.",
                     0, len(program.instructions))
        entry["uses_vertex_color"] = 1
        entry["inputs"] = [{"usage": 1, "usage_index": 1, "reg_index": 0,
                            "tev_source": SRC_PRIMARY}]
        entry["preconditions"] = ["varying_semantic_rebind"]

    else:  # defensive: a future census class still gets a named plan
        entry = _tev(sha, "single centred tap (%s)" % cls,
                     [_mod_stage()], _tex(1), "approx",
                     "%s: arithmetic collapsed to one centred tinted sample."
                     % cls, 0, len(program.instructions))
    return entry


# --- curated pixel plans -------------------------------------------------
# (sha8, note, stage_data, textures, quality, approximation, draw)
def _ps_plans():
    plans = []
    copy = [_copy_stage()]

    def exact_copy(sha, note, instructions=1):
        plans.append(_tev(sha, note, copy, _tex(1), "exact", None, 1,
                          instructions))

    def approx_copy(sha, note, approximation, instructions):
        plans.append(_tev(sha, note, copy, _tex(1), "approx", approximation,
                          0, instructions))

    # 1:1 fullscreen copy / clear / constant fills (full material contract).
    exact_copy("911297c9666e388aab307d72feaad9761379287276405a17732958d8ca6a1b26",
               "certified one-stage copy (s0 -> oC0)", 1)
    exact_copy("39f0dfdfc574d3e7f4c4ee0578bcc265b75ac05f1e0b9eb17087e52c6cc35d5a",
               "certified one-stage copy (s0 -> oC0)", 1)
    plans.append(_tev(
        "817340bfabd9a9b175aadbbdc0af49258dd38f2772985b1a40ba48d3eb1bf10e",
        "constant clear: 0.5 grey, opaque", [_const_stage([.5, .5, .5, .5])],
        [], "exact", None, 1, 1))
    plans.append(_tev(
        "4813dba351f7114f15a15a1d9c5308823e7fcfcbed2fe0dc1960662a2feca520",
        "constant fill: (0.5,0.5,0,0)", [_const_stage([.5, .5, 0, 0])],
        [], "exact", None, 1, 1))
    plans.append(_tev(
        "6628a644b06721d0e5538d63bed3299bee56749ddb33af7300293cf38f3bbc6b",
        "constant fill: black opaque", [_const_stage([0, 0, 0, 1])],
        [], "exact", None, 1, 1))
    plans.append(_tev(
        "9d2692a9640d3e1c9768fe5dfacc04a68ba13e5196d491ad6e905d3851cb4088",
        "constant fill: white opaque", [_const_stage([1, 1, 1, 1])],
        [], "exact", None, 1, 1))

    # Partial constant write: oC0.xy only, alpha undefined in SM3.
    plans.append(_tev(
        "01ab60cd1153c81486daf6416fc0a5fce7eab3fe550c725fd4641e86223b1b3f",
        "constant fill: (1,0.5,0) with alpha defaulted",
        [_const_stage([1, .5, 0, 0])], [], "approx",
        "source writes oC0.xy only; declared spec fills alpha with 0 "
        "(undefined in the SM3 stream)", 0, 1))

    # Tint / crossfade composites.
    plans.append(_tev(
        "583f98e0acc342b6638a1c226f329ef48d5d6435e636a217281b08e98523715f",
        "tint pass, alpha channel simplified to texture alpha", copy, _tex(1),
        "approx", "RGB = tex.rgb * (1,-1,0)+0 keeps the tint; source alpha "
        "is 1-tex.r, TEV alpha cannot read red, so alpha = tex.a", 0, 2))

    lerp_half = [_lit_stage(
        FUNC_INTERPOLATE, [SRC_TEX0, SRC_TEX1, SRC_CONST],
        [RGB_COLOR, RGB_COLOR, RGB_ALPHA],
        FUNC_INTERPOLATE, [SRC_TEX0, SRC_TEX1, SRC_CONST],
        [A_ALPHA, A_ALPHA, A_ALPHA], [.5, .5, .5, .5])]
    plans.append(_tev(
        "21978c956fae8496fd8654a2948258cdf58b31718df82ab5f7cfb55a1c109c4d",
        "two-texture 50/50 crossfade (3 textures collapsed to 2)", lerp_half,
        _tex(2), "approx", "3 textures and frame-adaptive blend weight "
        "collapsed to a fixed 0.5 lerp of s0/s1; s2 and the rcp/cmp "
        "adaptation term are dropped", 0, 27))
    plans.append(_tev(
        "a90f707a78222ca9a908ffced740cced62e8eae0f8495893ff332aeb567532a9",
        "adaptation+bloom composite simplified to 50/50 lerp", lerp_half,
        _tex(2), "approx", "eye-adaptation rcp/cmp chain replaced by a "
        "constant 0.5 weight; bloom texture s0 and scene s1 kept, s2 dropped",
        0, 30))
    plans.append(_tev(
        "816236227d06b66f7fdfba5ad48c4d3977b086b04ee91c72ad37fd78ab471b72",
        "DOF/radial blend simplified to 50/50 lerp", lerp_half, _tex(2),
        "approx", "rsq/rcp radial weight and 2 extra taps replaced by a "
        "constant 0.5 lerp between the two source textures", 0, 29))

    # Blur / bloom: N-tap box reduced to one centred tap.
    for sha, n, instr in [
        ("70a85a92b46bbe8e963af44ecf6ca75c9c7d5b48ff4963eb389c665984675777", 49, 38),
        ("6ccbe75901c78b7b02fa845f1aaaa8a003cd66d4abcb202bd1a909bd59c9ca48", 3, 9),
        ("b6484c80285be8e185e4a55cd16526f135df4cf6f9e6b7d87bf6e2a268f89e34", 6, 21),
        ("de28caa3cf7bb11d98788a133e5eb7c457ae2c8ae208f251a4c9a95ea7d130f2", 16, 68),
        ("d01c19fd0740a519d671468df61b074e53deeb39b6cd98da93e4258be03cd963", 5, 37),
        ("4a4fa2b2a1d18d1ef054cff6cae2f050ad28ef85b382d8e17dfccde53336b50b", 9, 37),
        ("0072af4b81a7b06b4b5bd6f54db0466ac2ebbf3ec6e1c127ad57608f2e6e3d89", 8, 62),
        ("c91e33289ed209ca45917ab1242afe582f78a47eeae3cc0fc4487b4477c5d791", 9, 39),
        ("2cc6e508fb5a2b96253c4dfeec1555521757008d107a7c0a4fd484845a0b6430", 3, 15),
        ("f50eb44cd422dbb1bc427eeeecfecf348b42c8ae9910dac4259e7d8a90b5d183", 5, 19),
    ]:
        approx_copy(
            sha, "separable full-screen tap pass reduced to one centred tap",
            "%d-tap kernel (loop/tap sum) replaced by a single bilinear "
            "centre sample; no loop, one texture unit. Expected loss: soft "
            "edges become a plain copy." % n, instr)

    # 14-tap luminance blur kept as a luma pass (DOT3) rather than RGB SUM.
    luma = [_lit_stage(
        FUNC_DOT3_RGB, [SRC_TEX0, SRC_CONST, SRC_PREV],
        [RGB_COLOR] * 3, FUNC_REPLACE,
        [SRC_TEX0, SRC_PREV, SRC_PREV], [A_ALPHA] * 3,
        [.2125, .7154, .0721, 1])]
    plans.append(_tev(
        "041086d9d408057209b8511a97bd7737f02d2366c6dd1863037f2d17a40cfa3f",
        "14-tap luminance blur reduced to one luma tap", luma, _tex(1),
        "approx", "14 taps collapsed to one centred tap; DOT3(luma) keeps "
        "the luminance semantics, Alpha stays tex.a", 0, 23))

    # 16-slice 1D LUT grade collapsed to one slice.
    plans.append(_tev(
        "8fb1465543ccf66d25463d76c34db65510923a4b407708a9b1ce208a6f1fa344",
        "16-slice colour-grade LUT collapsed to slice 0", copy, _tex(1),
        "approx", "16 co-bound 2D LUT slices are blended by an lrp chain; "
        "DTEX has no indirect/second LUT unit, so the plan samples slice 0 "
        "only. A 256x16 packed LUT is the follow-up if grading is required.",
        0, 70))

    # Tone map / exposure approximation.
    exposure = [_lit_stage(
        FUNC_MODULATE, [SRC_TEX0, SRC_CONST, SRC_PREV],
        [RGB_COLOR, RGB_COLOR, RGB_COLOR], FUNC_REPLACE,
        [SRC_TEX0, SRC_PREV, SRC_PREV], [A_ALPHA] * 3, [.8, .8, .8, 1])]
    plans.append(_tev(
        "83cc2df28d907021cafd21aa9c4464df8644b043446a43b6056115f64d2d6860",
        "tone map / grade approximated by a constant 0.8 exposure",
        exposure, _tex(1), "approx", "dp3 luma, cmp/lerp grade and negative "
        "def literals replaced by a single MODULATE with exposure 0.8; "
        "no LUT texture needed", 0, 33))

    return plans


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=DEFAULT_CORPUS)
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed files differ")
    args = parser.parse_args(argv)

    manifest = json.load(open(os.path.join(HERE, "manifest.json"),
                              encoding="utf-8"))
    # Already certified in the shared runtime table (docs/shader_plans.json):
    # not a refusal, and deliberately not duplicated in this catalog.
    runtime_certified = {
        "6022044ef19e4b1c241482207acca5a6be484081e629b6d72cdefdd9568a45de",
        "0ccc7542aa06b2e20a41283acc2d5473602fc03f5495cc95166ec3b01820d2f7",
    }
    hashes = {row[0]: row for row in manifest["hashes"]}
    rows = json.load(open(os.path.join(args.corpus, "corpus.json"),
                          encoding="utf-8"))["rows"]
    by_sha = {}
    for row in rows:
        if row["sha256"] in hashes:
            by_sha[row["sha256"]] = row

    programs = {}
    for kind, name in (("ps", "ps_chunks.bin"), ("vs", "vs_chunks.bin")):
        data = open(os.path.join(args.corpus, name), "rb").read()
        count = struct.unpack("<I", data[:4])[0]
        off = 4
        for index in range(count):
            size = struct.unpack("<I", data[off:off + 4])[0]
            blob = data[off + 4:off + 4 + size]
            off += 4 + size
            for sha, row in by_sha.items():
                if row["kind"] == kind and row["index"] == index:
                    programs[sha] = asm.parse_program(blob)

    entries = _ps_plans()
    planned_ps = {entry["sha256"] for entry in entries}
    for sha, row in sorted(by_sha.items()):
        if row["kind"] != "ps" or sha in runtime_certified or sha in planned_ps:
            continue
        program = programs.get(sha)
        if program is None or program.error:
            raise SystemExit("missing PS IR for %s" % sha)
        entries.append(_refusal_ps_plan(
            sha, {"class": hashes[sha][2]}, program))
    for sha, row in sorted(by_sha.items()):
        if row["kind"] != "vs" or sha in runtime_certified:
            continue
        program = programs.get(sha)
        if program is None or program.error:
            raise SystemExit("missing VS IR for %s" % sha)
        varying = sum(1 for d in program.declarations
                      if d.reg_type == asm.REG_OUTPUT and d.usage != 0)
        entries.append(_pica_vs(
            sha, program,
            "fullscreen-quad PICA VS contract: %d varying(s), codegen pending"
            % varying))

    # Enrich with corpus identity and attach an IR-derived instruction count.
    for entry in entries:
        row = by_sha[entry["sha256"]]
        entry["sha1"] = row["sha1"]
        entry["source_package"] = row["package"]
        if row["kind"] == "ps":
            entry["instructions"] = _ir_instruction_count(args.corpus,
                                                          entry["sha256"], row)
    entries.sort(key=lambda e: (e["kind"], e["sha256"]))

    planned = {e["sha256"] for e in entries}
    refusals = []
    for sha, row in sorted(hashes.items()):
        if sha in planned or sha in runtime_certified:
            continue
        refusals.append({
            "sha256": sha,
            "kind": row[1],
            "class": row[2],
            "chunks": row[3],
            "reason": "not in the phase-1 draw set; see class",
        })

    plans_doc = {
        "schema": "d9-shader-plan-v1",
        "archive": manifest.get("archive", ARCHIVE),
        "generated_for": "blit_post (copy/blit, post-process/imagespace)",
        "note": "pack catalog; every entry has runtime=0. draw_enabled=1 only "
                "for an exact one-stage TEV contract. Approximations carry "
                "quality=approx and an approximation field.",
        "entries": entries,
    }
    refusals_doc = {
        "pack": "blit_post",
        "counts": {
            "planned": len(entries),
            "exact_draw_enabled": sum(1 for e in entries
                                      if e["draw_enabled"] == 1),
            "approx_translated": sum(1 for e in entries
                                     if e.get("quality") == "approx"),
            "contract_only_vs": sum(1 for e in entries
                                    if e["kind"] == "pica_vs"),
            "refused": len(refusals),
        },
        "refusals": refusals,
    }

    if args.check:
        for path, doc in (("plans.json", plans_doc),
                          ("refusals.json", refusals_doc)):
            have = json.load(open(os.path.join(HERE, path), encoding="utf-8"))
            if have != json.loads(json.dumps(doc)):
                raise SystemExit("%s is stale; rerun without --check" % path)
        print("blit_post: plans.json and refusals.json are current")
        return 0

    for path, doc in (("plans.json", plans_doc), ("refusals.json", refusals_doc)):
        with open(os.path.join(HERE, path), "w", encoding="utf-8") as handle:
            json.dump(doc, handle, indent=1, sort_keys=True)
            handle.write("\n")
    counts = refusals_doc["counts"]
    print("blit_post: %(planned)d planned (%(exact_draw_enabled)d exact/draw, "
          "%(approx_translated)d approx, %(contract_only_vs)d VS contract), "
          "%(refused)d refused" % counts)
    return 0


_IR_CACHE = {}


def _ir_instruction_count(corpus, sha, row):
    """Convert a chunk index into the MojoShader instruction count."""
    if not _IR_CACHE:
        for kind, name in (("ps", "ps_ir.bin"), ("vs", "vs_ir.bin")):
            data = open(os.path.join(corpus, name), "rb").read()
            count = struct.unpack("<I", data[7:11])[0]
            off = 11
            table = {}
            for _ in range(count):
                index, status, instr, tlen = struct.unpack(
                    "<IIII", data[off:off + 16])
                off += 16 + tlen
                table[index] = instr
            _IR_CACHE[kind] = table
    return _IR_CACHE[row["kind"]][row["index"]]


if __name__ == "__main__":
    raise SystemExit(main())
