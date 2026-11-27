"""Residual audit of dumped ``menu.generic.vs`` bytecode.

Reads the ``TESV_MENU_SHADER_DUMP`` directory (``vs_<sha>.txt`` + ``.bin``),
replays the structural runtime contract from
``source/d3d9_menu_shader_runtime.c`` and reports, per unique hash:

  ``accepted``     structural family match whose visible position0/texcoord0/
                   color0 path is exactly the COM fixed transform;
  ``not_fixed``    family match, but the visible path is not the fixed
                   transform (no CPU draw; named refusal at draw time);
  ``inputs_only``  rejected by an input semantic that is outside the measured
                   family (skinning blendweight/blendindices);
  ``needs_math``   rejected by a real arithmetic opcode without a PICA VS
                   equivalent (``lrp``, ``nrm``);
  ``shape``        rejected for a structural reason (version, length, missing
                   outputs).

It also runs the same dataflow check as ``vs_cpu_safe()`` and reports whether
``tangent0``/``binormal0`` reach position0/texcoord0/color0.

Usage::

    python tools\\shaders\\menu_vs_residual.py E:\\menu_draw_dump
    python tools\\shaders\\menu_vs_residual.py E:\\menu_draw_dump --json out.json
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re

END = 0xFFFF
COMMENT = 0xFFFE
PHASE = 0xFFFD
DCL = 0x1F
TEXLD = 0x42

OPCODE_NAMES = {
    0x01: "mov", 0x02: "add", 0x03: "sub", 0x04: "mad", 0x05: "mul",
    0x06: "rcp", 0x07: "rsq", 0x08: "dp3", 0x09: "dp4", 0x0A: "min",
    0x0B: "max", 0x0C: "slt", 0x0D: "sge", 0x0E: "exp", 0x0F: "log",
    0x10: "lit", 0x11: "dst", 0x12: "lrp", 0x13: "frc", 0x14: "m4x4",
    0x15: "m4x3", 0x16: "m3x4", 0x17: "m3x3", 0x18: "m3x2", 0x1F: "dcl",
    0x20: "pow", 0x21: "crs", 0x22: "sgn", 0x23: "abs", 0x24: "nrm",
    0x25: "sincos", 0x2E: "mova", 0x51: "def", 0x52: "defi", 0x53: "defb",
}

ALLOWED_OPCODES = {
    0x01, 0x02, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
    0x0D, 0x13, 0x1F, 0x20, 0x23, 0x25, 0x2E, 0x51,
}

USAGE_NAMES = {0: "position", 1: "blendweight", 2: "blendindices",
               3: "normal", 4: "psize", 5: "texcoord", 6: "tangent",
               7: "binormal", 8: "tessfactor", 9: "positiont",
               10: "color", 11: "fog", 12: "depth", 13: "sample"}


def words_from_bin(path):
    import struct
    data = open(path, "rb").read()
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


def words_from_txt(path):
    text = open(path, encoding="utf-8", errors="replace").read().splitlines()
    return [int(x, 16) for x in
            re.findall(r"\b[0-9a-fA-F]{8}\b", " ".join(text[1:]))]


def reg(token):
    return ((token >> 28) & 7, token & 0x7FF, (token >> 16) & 0xFF,
            (token >> 24) & 0xF, (token >> 16) & 0xF,
            (token & (1 << 13)) != 0)


def parse(words):
    info = {"version": words[0] if words else 0, "instructions": 0,
            "ops": collections.Counter(), "inputs": [], "outputs": [],
            "bad": None, "texld": False}
    if not words:
        info["bad"] = "empty"
        return info
    i = 1
    end = False
    while i < len(words):
        token = words[i]
        op = token & 0xFFFF
        if op == END:
            end = True
            break
        if op == COMMENT:
            i += ((token >> 16) & 0x7FFF) + 1
            continue
        if op == PHASE:
            info["bad"] = "phase token"
            break
        total = 1 + ((token >> 24) & 0xF)
        if total > len(words) - i:
            info["bad"] = "instruction past the stream"
            return info
        if op == TEXLD:
            info["texld"] = True
        info["instructions"] += 1
        info["ops"][OPCODE_NAMES.get(op, "op_0x%02x" % op)] += 1
        if op == DCL and total == 3:
            decl, dst = words[i + 1], words[i + 2]
            usage, index = decl & 0x1F, (decl >> 16) & 0xF
            rtype, rnum = (dst >> 28) & 7, dst & 0x7FF
            if rtype == 1:
                info["inputs"].append((usage, index, rnum))
            elif rtype == 6:
                info["outputs"].append((usage, index, rnum))
        i += total
    if not end:
        info["bad"] = info["bad"] or "no END token"
    return info


def usage_allowed(usage, index):
    if usage == 0 and index == 0:
        return True
    if usage == 3 and index == 0:
        return True
    if usage == 5 and index <= 4:
        return True
    if usage in (6, 7) and index == 0:
        return True
    if usage == 10 and index <= 1:
        return True
    return False


def cpu_safe(words):
    """Python twin of vs_cpu_safe(): visible path is the COM fixed transform."""
    posin = 0xFFFF
    tcins, colin = [], 0xFFFF
    posout = tcout = colout = 0xFFFF
    i = 1
    while i < len(words):
        token = words[i]
        op = token & 0xFFFF
        if op == END:
            break
        if op == COMMENT:
            i += ((token >> 16) & 0x7FFF) + 1
            continue
        total = 1 + ((token >> 24) & 0xF)
        if total > len(words) - i:
            return False, "past the stream"
        if op == DCL and total == 3:
            decl, dst = words[i + 1], words[i + 2]
            usage, index = decl & 0x1F, (decl >> 16) & 0xF
            rtype, rnum = (dst >> 28) & 7, dst & 0x7FF
            if rtype == 1:
                if usage == 0 and index == 0:
                    posin = rnum
                elif usage == 5:
                    tcins.append(rnum)
                elif usage == 10 and index == 0:
                    colin = rnum
            elif rtype == 6:
                if usage == 0 and index == 0:
                    posout = rnum
                elif usage == 5 and index == 0:
                    tcout = rnum
                elif usage == 10 and index == 0:
                    colout = rnum
        i += total
    if posin == 0xFFFF or posout == 0xFFFF or tcout == 0xFFFF:
        return False, "missing position/texcoord0 declaration"
    pos_rows = tc_writes = tcgen_writes = tcgen_rows = color_writes = 0
    uvgen = color_swap = 0
    i = 1
    while i < len(words):
        token = words[i]
        op = token & 0xFFFF
        if op == END:
            break
        if op == COMMENT:
            i += ((token >> 16) & 0x7FFF) + 1
            continue
        total = 1 + ((token >> 24) & 0xF)
        if total > len(words) - i:
            return False, "past the stream"
        if op == DCL or 0x51 <= op <= 0x53:
            i += total
            continue
        if total < 2:
            i += total
            continue
        dtype, dnum, _dswz, dmod, dmask, drel = reg(words[i + 1])
        if dtype == 6 and dnum == posout:
            if op != 0x09 or total != 4:
                return False, "position is not a dp4 row"
            mask = (words[i + 1] >> 16) & 0xF
            if not mask or mask & (mask - 1):
                return False, "position write mask is not one component"
            component = 0
            while not (mask & (1 << component)):
                component += 1
            src_pos = src_row = 0xFFFF
            for s in range(2):
                rtype, rnum, swz, mod, _m, rel = reg(words[i + 2 + s])
                if rel or mod != 0 or swz != 0xE4:
                    return False, "position source modifier/swizzle"
                if rtype == 1 and rnum == posin:
                    src_pos = rnum
                elif rtype == 2 and rnum < 4:
                    src_row = rnum
                else:
                    return False, "position row reads a non-constant/temp"
            if src_pos == 0xFFFF or src_row != component:
                return False, "position row is not c0..c3 * POSITION0"
            pos_rows |= 1 << component
        elif dtype == 6 and dnum == tcout:
            mask = (words[i + 1] >> 16) & 0xF
            if not mask or (mask & ~3):
                return False, "texcoord write mask"
            if op == 0x01 and total == 3:
                tc_writes += 1
                rtype, rnum, swz, mod, _m, rel = reg(words[i + 2])
                if rel or mod != 0 or swz != 0xE4:
                    return False, "texcoord mov source modifier"
                if not (rtype == 1 and rnum in tcins):
                    return False, "texcoord mov is not an input passthrough"
            elif op == 0x09 and total == 4:
                for s in range(2):
                    rtype, rnum, swz, mod, _m, rel = reg(words[i + 2 + s])
                    if rel or mod != 0 or swz != 0xE4:
                        return False, "texgen source modifier"
                    if rtype == 1 and rnum == posin:
                        continue
                    if rtype == 2 and rnum in (4, 5):
                        tcgen_rows |= 1 << (rnum - 4)
                        continue
                    return False, "texgen reads a non-constant/temp"
                uvgen = 1
                tcgen_writes += 1
            else:
                return False, "texcoord written by a non-fixed form"
        elif dtype == 6 and dnum == colout:
            if op != 0x01 or total != 3 or colin == 0xFFFF:
                return False, "color0 is not a COLOR0 passthrough"
            rtype, rnum, swz, mod, _m, rel = reg(words[i + 2])
            if rel or mod != 0 or rtype != 1 or rnum != colin:
                return False, "color0 source is not the declared input"
            if swz == 0xE4:
                color_swap = 0
            elif swz == 0xC6:
                color_swap = 1
            else:
                return False, "color0 swizzle"
            color_writes += 1
        i += total
    if pos_rows != 0xF:
        return False, "position is not all four c0..c3 rows"
    if not ((tc_writes == 1 and tcgen_writes == 0) or
            (tc_writes == 0 and tcgen_writes == 2 and uvgen and
             tcgen_rows == 3)):
        return False, "texcoord is not a passthrough/complete texgen"
    if colout != 0xFFFF and color_writes != 1:
        return False, "color0 declared but not written once"
    if colout == 0xFFFF and color_writes != 0:
        return False, "color0 written without a declaration"
    return True, "uvgen=%d swap=%d" % (uvgen, color_swap)


def tangent_on_visible_path(words):
    """Conservative taint: do tangent0/binormal0 reach position0/tc0/color0?"""
    inp, out = {}, {}
    i = 1
    while i < len(words):
        token = words[i]
        op = token & 0xFFFF
        if op == END:
            break
        if op == COMMENT:
            i += ((token >> 16) & 0x7FFF) + 1
            continue
        total = 1 + ((token >> 24) & 0xF)
        if op == DCL and total == 3:
            decl, dst = words[i + 1], words[i + 2]
            rtype, rnum = (dst >> 28) & 7, dst & 0x7FF
            key = (decl & 0x1F, (decl >> 16) & 0xF)
            if rtype == 1:
                inp[rnum] = key
            elif rtype == 6:
                out[rnum] = key
        i += total
    taint = set()
    visible = {}
    i = 1
    while i < len(words):
        token = words[i]
        op = token & 0xFFFF
        if op == END:
            break
        if op == COMMENT:
            i += ((token >> 16) & 0x7FFF) + 1
            continue
        total = 1 + ((token >> 24) & 0xF)
        if op == DCL:
            i += total
            continue
        if total >= 2:
            dst = reg(words[i + 1])[:2]
            srcs = [reg(words[i + 2 + s])[:2] for s in range(total - 2)]
            for src in srcs:
                if src[0] == 1 and inp.get(src[1], (99,))[0] in (6, 7):
                    taint.add(src)
            tainted = any(src in taint for src in srcs)
            if tainted:
                taint.add(dst)
            if dst[0] == 6 and out.get(dst[1], (99,))[0] in (0, 5, 10):
                visible[out[dst[1]]] = tainted
        i += total
    return visible


def classify(info, words):
    if info["version"] != 0xFFFE0300:
        return "shape", "version 0x%08x" % info["version"]
    if info["bad"]:
        return "shape", info["bad"]
    if not 1 <= info["instructions"] <= 96:
        return "shape", "instructions %d" % info["instructions"]
    if info["texld"]:
        return "shape", "texture sample"
    allowed_names = {OPCODE_NAMES[o] for o in ALLOWED_OPCODES}
    bad_ops = sorted(name for name in info["ops"] if name not in allowed_names)
    if bad_ops:
        if "lrp" in bad_ops:
            return "needs_math", "lrp (0x12) has no PICA VS equivalent"
        return "needs_math", "opcode " + ", ".join(bad_ops)
    outs = {(u, i) for u, i, _ in info["outputs"]}
    if (0, 0) not in outs:
        return "shape", "missing position0 output"
    if (5, 0) not in outs:
        return "shape", "missing texcoord0 output"
    bad_inputs = [(u, i) for u, i, _ in info["inputs"]
                  if not usage_allowed(u, i)]
    if bad_inputs:
        names = ", ".join("%s%d" % (USAGE_NAMES.get(u, "usage"), i)
                          for u, i in bad_inputs)
        return "inputs_only", "input " + names
    ok, why = cpu_safe(words)
    if ok:
        return "accepted", why
    visible = tangent_on_visible_path(words)
    tangent_hits = [("%s%d" % (USAGE_NAMES.get(u, "usage"), i))
                    for (u, i), dirty in visible.items() if dirty]
    detail = "not fixed: " + why
    if any(u in (6, 7) for u, _ in visible):
        detail += "; tangent/binormal on visible path: " + ",".join(tangent_hits)
    return "not_fixed", detail


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", help="TESV_MENU_SHADER_DUMP directory")
    parser.add_argument("--json", default="", help="write a JSON summary")
    args = parser.parse_args(argv)
    files = sorted(f for f in os.listdir(args.dump)
                   if f.startswith("vs_") and f.endswith(".txt"))
    rows = []
    for name in files:
        sha = name[3:-4]
        bin_path = os.path.join(args.dump, "vs_%s.bin" % sha)
        if os.path.exists(bin_path):
            data = open(bin_path, "rb").read()
            words = words_from_bin(bin_path)
            digest = hashlib.sha256(data).hexdigest()
        else:
            words = words_from_txt(os.path.join(args.dump, name))
            digest = None
        info = parse(words)
        verdict, detail = classify(info, words)
        rows.append({
            "sha256": sha,
            "bin_sha256": digest,
            "verdict": verdict,
            "detail": detail,
            "instructions": info["instructions"],
            "ops": dict(info["ops"]),
            "inputs": ["%s%d/v%d" % (USAGE_NAMES.get(u, "usage"), i, r)
                       for u, i, r in info["inputs"]],
            "outputs": ["%s%d/v%d" % (USAGE_NAMES.get(u, "usage"), i, r)
                        for u, i, r in info["outputs"]],
        })
    counts = collections.Counter(row["verdict"] for row in rows)
    print("unique VS dumps: %d" % len(rows))
    for verdict in ("accepted", "not_fixed", "inputs_only", "needs_math",
                    "shape"):
        print("  %-12s %d" % (verdict, counts.get(verdict, 0)))
    for verdict in ("inputs_only", "needs_math", "shape"):
        group = [r for r in rows if r["verdict"] == verdict]
        if not group:
            continue
        print("\n[%s]" % verdict)
        by_reason = collections.Counter(r["detail"] for r in group)
        for reason, count in sorted(by_reason.items(),
                                    key=lambda kv: -kv[1]):
            print("  %3d  %s" % (count, reason))
        for row in group:
            print("    %s  %s" % (row["sha256"][:16], row["detail"]))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump({"dump": args.dump, "counts": dict(counts), "rows": rows},
                      handle, indent=2)
            handle.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
