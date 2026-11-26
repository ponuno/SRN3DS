"""Field-level differential: MojoShader ``d3d`` IR vs ``tools/shaders/asm.py``.

The naive comparison of ``instruction_count`` is useless: MojoShader and the
project parser count declarations and literals differently, which floods the
census with thousands of phantom mismatches.  This module compares the decoded
fields instead, following ``docs/PICA_RESEARCH.md`` section 3.3:

* opcode,
* destination register type/index, write mask, destination modifier
  (``_sat`` / ``_pp`` / ``_centroid``),
* every source's register type/index, 4-component swizzle, source modifier and
  relative-addressing tuple,
* ``def``/``defb``/``defi`` literals,
* ``dcl`` declarations (usage, usage index, register, sampler texture type).

Declarations and literals are compared as separate multisets; the executable
body is compared positionally.  A chunk that parses on both sides and agrees on
every field is reported ``equal``; anything else carries a named divergence.

Host only.  Reads the framed ``sm3_dump`` IR plus the raw chunk dumps that
``tools/sm3_corpus.py`` writes; no retail bytes are embedded in the repository.
"""

from __future__ import annotations

import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import asm  # noqa: E402

# --------------------------------------------------------------------------
# Register names (MojoShader D3D profile) -> asm REG_TYPE_* numbers.
REG_NAME_TO_TYPE = {
    "r": asm.REG_TEMP,
    "v": asm.REG_INPUT,
    "c": asm.REG_CONST,
    "a": asm.REG_ADDR,
    "rast": asm.REG_RASTOUT,
    "attr": asm.REG_ATTROUT,
    "o": asm.REG_OUTPUT,
    "oPos": asm.REG_RASTOUT,
    "oFog": asm.REG_RASTOUT,
    "i": asm.REG_CONSTINT,
    "oC": asm.REG_COLOROUT,
    "oDepth": asm.REG_DEPTHOUT,
    "s": asm.REG_SAMPLER,
    "b": asm.REG_CONSTBOOL,
    "l": asm.REG_LABEL,
    "p": asm.REG_PREDICATE,
    "vPos": 17,
    "vFace": 17,
}
_REG_RE = re.compile(
    r"^(?P<type>oDepth|oPos|oFog|oC|rast|attr|[rvcaosib]l?|i|p)"
    r"(?P<index>\d*)"
)
_TYPE_ORDER = sorted(REG_NAME_TO_TYPE, key=len, reverse=True)

# MojoShader opcode spelling -> asm opcode number.
OPCODE_ALIASES = {
    "if_lt": 0x29, "if_ge": 0x29, "if_gt": 0x29, "if_le": 0x29,
    "if_eq": 0x29, "if_ne": 0x29,
    "breakc_lt": 0x2D, "breakc_ge": 0x2D, "breakc_gt": 0x2D,
    "breakc_le": 0x2D, "breakc_eq": 0x2D, "breakc_ne": 0x2D,
    "texldd": 0x5D, "texldl": 0x5F,
    # ps_2_0 projected/biased sampling: the same opcode with an instruction
    # control bit in the opcode token.  The control word itself is compared.
    "texldp": 0x42, "texldb": 0x42,
}
CONTROL_FROM_ALIAS = {"texldp": 0x01, "texldb": 0x02}
# D3DSHADER_COMPARISON: GT=1, EQ=2, GE=3, LT=4, NE=5, LE=6 (not D3DCMP).
COMP_FROM_ALIAS = {"if_lt": 4, "if_ge": 3, "if_gt": 1, "if_le": 6,
                   "if_eq": 2, "if_ne": 5,
                   "breakc_lt": 4, "breakc_ge": 3, "breakc_gt": 1,
                   "breakc_le": 6, "breakc_eq": 2, "breakc_ne": 5}
TEXLD_OPS = {0x42, 0x5D, 0x5F}
COMPARE_OPS = {0x29, 0x2D}

# Opcodes whose operands are all sources (no destination argument).
NO_DST_OPS = {
    0x1B, 0x1D, 0x1C, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D,
    0x60, 0x19, 0x1A, 0x1E, 0x00,
}
# Declaration/literal opcodes: compared out of band, not in the body.
DECL_OPS = {0x00, 0x1F, 0x51, 0x2F, 0x30}

DEST_MODS = {"_sat": 1, "_pp": 2, "_centroid": 4}
SRC_MODS = {
    "": 0, "_bias": 2, "_bx2": 4, "_x2": 7, "_dz": 9, "_dw": 10,
    "_abs": 11,
}

# The project parser's own (misleading) names, kept for report labels only.
ASM_SRC_MOD_NAMES = {0: "-", 2: "bias", 4: "sign", 6: "1-", 7: "x2",
                     9: "dz", 10: "dw", 11: "abs", 13: "not"}


class ParseProblem(ValueError):
    pass


def _split_operands(text: str) -> List[str]:
    out: List[str] = []
    depth = 0
    current = []
    for ch in text:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    tail = "".join(current).strip()
    if tail:
        out.append(tail)
    return out


def _expand_swizzle(text: str) -> str:
    """MojoShader trims trailing duplicate channels; pad back to 4."""
    if not text:
        return "xyzw"
    text = text.lstrip(".")
    if not text:
        return "xyzw"
    if len(text) >= 4:
        return text[:4]
    return text + text[-1] * (4 - len(text))


def parse_src(text: str) -> dict:
    text = text.strip()
    mod = 0
    if text.startswith("-"):
        text = text[1:]
        mod = 1
    elif text.startswith("1-"):
        text = text[2:]
        mod = 6
    elif text.startswith("!"):
        text = text[1:]
        mod = 13
    suffix = ""
    # MojoShader appends the postmodifier after the register number and the
    # swizzle after everything, so peel the swizzle first, then the relative
    # bracket, then the modifier.
    swz_match = re.search(r"(\.\w+)$", text)
    swizzle = "xyzw"
    if swz_match:
        swizzle = _expand_swizzle(swz_match.group(1))
        text = text[: swz_match.start()]
    relative = None
    rel_match = re.search(r"\[([^\]]+)\]$", text)
    if rel_match:
        relative = rel_match.group(1)
        text = text[: rel_match.start()]
    match = re.search(r"(_abs|_bias|_bx2|_x2|_dz|_dw)$", text)
    if match:
        suffix = match.group(1)
        text = text[: match.start()]
        base = SRC_MODS[suffix]
        # negate + bias/x2/abs share the "negated" variant one past the base
        if mod == 1:
            mod = {2: 3, 4: 5, 7: 8, 11: 12}.get(base, base)
        else:
            mod = base
    for name in _TYPE_ORDER:
        if text.startswith(name):
            rest = text[len(name):]
            if rest.isdigit() or rest == "":
                index = int(rest) if rest else 0
                if name == "vFace":
                    index = 1
                return {
                    "type": REG_NAME_TO_TYPE[name],
                    "index": index,
                    "swizzle": swizzle,
                    "modifier": mod,
                    "relative": _parse_relative(relative),
                }
    raise ParseProblem("source: %r" % text)


def parse_dst(text: str) -> dict:
    text = text.strip()
    mod = 0
    for name, value in DEST_MODS.items():
        if text.endswith(name):
            text = text[: -len(name)]
            mod = value
            break
    swz = re.search(r"\.(\w+)$", text)
    mask = 0xF
    if swz:
        mask = 0
        for ch in swz.group(1):
            mask |= 1 << "xyzw".index(ch)
        text = text[: swz.start()]
    for name in _TYPE_ORDER:
        if text.startswith(name):
            rest = text[len(name):]
            if rest.isdigit() or rest == "":
                return {
                    "type": REG_NAME_TO_TYPE[name],
                    "index": int(rest) if rest else 0,
                    "mask": mask,
                    "modifier": mod,
                }
    raise ParseProblem("dest: %r" % text)


DECL_RE = re.compile(r"^dcl_(?:(?P<sampler>2d|cube|volume)"
                     r"|(?P<usage>[a-z]+?)(?P<index>\d*))"
                     r"(?:_pp|_centroid)?$")


def parse_mojoshader(text: str) -> dict:
    """Turn one d3d-profile disassembly into a normalised instruction list."""
    body: List[dict] = []
    defs: List[Tuple[int, Tuple[float, ...]]] = []
    decls: List[Tuple[int, int, int, int, int]] = []
    version = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line in ("end", "vs_3_0", "ps_3_0", "vs_2_0", "ps_2_0"):
            if line != "end" and line != "vs_3_0" and line != "ps_3_0":
                version = line
            continue
        head, _, rest = line.partition(" ")
        head = head.strip()
        operands = _split_operands(rest) if rest else []

        decl = DECL_RE.match(head)
        if head == "dcl":
            # dcl vPos / dcl vFace: a misc-type declaration with no usage.
            dst = parse_dst(operands[0])
            decls.append((dst["type"], dst["index"], 0, 0, 0))
            continue
        if decl:
            dst = parse_dst(operands[0])
            if decl.group("sampler"):
                decls.append((dst["type"], dst["index"], 0, 0,
                              {"2d": 2, "cube": 3, "volume": 4}[
                                  decl.group("sampler")]))
            else:
                usage = {v: k for k, v in asm.USAGE_NAMES.items()}.get(
                    decl.group("usage"), -1)
                index = int(decl.group("index") or 0)
                decls.append((dst["type"], dst["index"], usage, index, 0))
            continue

        name = head
        dst_mod = 0
        changed = True
        while changed:
            changed = False
            for suffix, value in DEST_MODS.items():
                if name.endswith(suffix):
                    name = name[: -len(suffix)]
                    dst_mod |= value
                    changed = True
                    break
        if name in ("def", "defb", "defi"):
            opcode = asm.NAME_TO_OPCODE[name]
            dst = parse_dst(operands[0])
            values = tuple(float(v) for v in operands[1:5])
            defs.append((dst["index"], values))
            continue
        if name in ("nop", "end"):
            continue

        opcode = OPCODE_ALIASES.get(name)
        if opcode is None:
            opcode = asm.NAME_TO_OPCODE.get(name)
        if opcode is None:
            raise ParseProblem("opcode: %r" % name)

        pos = 0
        dst = None
        if opcode not in NO_DST_OPS:
            dst = parse_dst(operands[pos])
            dst["modifier"] = dst_mod
            pos += 1
        srcs = [parse_src(op) for op in operands[pos:]]
        body.append({"opcode": opcode, "dst": dst, "srcs": srcs,
                     "comp": COMP_FROM_ALIAS.get(name),
                     "controls": CONTROL_FROM_ALIAS.get(name, 0)})
    return {"version": version, "body": body, "defs": defs, "decls": decls}


REL_NAME_TO_TYPE = {"a": asm.REG_ADDR, "aL": 15, "i": asm.REG_CONSTINT,
                    "b": asm.REG_CONSTBOOL, "c": asm.REG_CONST}


def _parse_relative(text: Optional[str]):
    if not text:
        return None
    match = re.match(r"^(aL|[aicb])(\d*)(?:\.([xyzw]))?$", text)
    if not match:
        return ("?", text, None)
    comp = match.group(3)
    return (REL_NAME_TO_TYPE.get(match.group(1), -1),
            int(match.group(2) or 0), comp)


def _proj_src(src) -> dict:
    return {"type": src.reg_type, "index": src.index,
            "swizzle": src.swizzle, "modifier": src.modifier,
            "relative": None}


def parse_project(code: bytes) -> dict:
    program = asm.parse_program(code)
    body = []
    defs = []
    decls = []
    usage_to_int = {name: value for value, name in asm.USAGE_NAMES.items()}
    texture_to_int = {name: value
                      for value, name in asm.TEXTURE_TYPE_NAMES.items()}
    for ins in program.instructions:
        if ins.opcode in DECL_OPS:
            if ins.opcode in (0x51, 0x30) and ins.literal:
                defs.append((ins.dst.index if ins.dst else 0, tuple(ins.literal)))
            elif ins.opcode == 0x1F and ins.decl is not None:
                decl = ins.decl
                decls.append((decl["reg_type"], decl["reg_index"],
                              usage_to_int.get(decl["usage"], -1),
                              decl["usage_index"],
                              texture_to_int.get(decl["texture_type"], 0)))
            continue
        dst = None
        srcs = []
        comp = None
        controls = 0
        opcode_token = struct.unpack_from("<I", code, ins.offset)[0]
        if ins.opcode in COMPARE_OPS:
            # The comparison function lives in the instruction token bits
            # 16-23; MojoShader folds it into the opcode spelling (if_lt, ...).
            comp = (opcode_token >> 16) & 0x7
        if ins.opcode in TEXLD_OPS:
            controls = (opcode_token >> 16) & 0xFF
        if ins.dst is not None:
            if ins.opcode in NO_DST_OPS:
                # asm.parse_program wraps the first operand as a destination
                # for every opcode; for branch/loop/label opcodes that word is a
                # source.  Re-decode the raw token as a source instead of
                # calling this a divergence.
                srcs.append(_proj_src(asm._src(ins.dst.token)))
            else:
                dst = {"type": ins.dst.reg_type, "index": ins.dst.index,
                       "mask": ins.dst.mask, "modifier": ins.dst.modifier}
        # Walk the source words directly: a relative-addressed source is
        # followed by one extra relative token that is not a source operand.
        # Word 0 is the opcode token and word 1 is the destination (or, for a
        # branch/loop opcode, the first source already handled above).
        start = 2
        words = [struct.unpack_from("<I", code, ins.offset + 4 * k)[0]
                 for k in range(start, ins.words)]
        k = 0
        while k < len(words):
            token = words[k]
            entry = _proj_src(asm._src(token))
            if asm.is_relative(token) and k + 1 < len(words):
                rel = words[k + 1]
                entry["relative"] = (asm.reg_type(rel), asm.reg_number(rel),
                                     "xyzw"[(rel >> 16) & 3])
                k += 2
            else:
                k += 1
            srcs.append(entry)
        body.append({"opcode": ins.opcode, "dst": dst, "srcs": srcs,
                     "comp": comp, "controls": controls})
    return {"version": program.version, "body": body, "defs": defs,
            "decls": decls, "error": program.error}


def _decl_key(row) -> tuple:
    reg_type, reg_index, usage, usage_index, texture_type = row
    # MojoShader reports the sampler texture type where the project parser
    # reports the dcl target; both are 0 for non-sampler declarations.
    return (reg_type, reg_index, usage, usage_index,
            texture_type if reg_type == asm.REG_SAMPLER else 0)


def compare(mj: dict, proj: dict) -> dict:
    """Return ``{"status", "detail", "index"}`` for one chunk."""
    if proj.get("error"):
        return {"status": "parse_error_project", "detail": proj["error"],
                "index": -1}
    a, b = mj["body"], proj["body"]
    if len(a) != len(b):
        return {"status": "differ_count", "index": -1,
                "detail": "mojoshader=%d project=%d" % (len(a), len(b))}
    for i, (x, y) in enumerate(zip(a, b)):
        if x["opcode"] != y["opcode"]:
            return {"status": "differ_opcode", "index": i,
                    "detail": "%s vs %s" % (asm.OPCODE_NAMES.get(x["opcode"]),
                                            asm.OPCODE_NAMES.get(y["opcode"]))}
        if x.get("comp") is not None and x["comp"] != y.get("comp"):
            return {"status": "differ_compare", "index": i,
                    "detail": "%r vs %r" % (x["comp"], y.get("comp"))}
        if x["opcode"] in TEXLD_OPS and x.get("controls", 0) != y.get("controls", 0):
            return {"status": "differ_texld_control", "index": i,
                    "detail": "0x%02x vs 0x%02x" % (x.get("controls", 0),
                                                    y.get("controls", 0))}
        if (x["dst"] is None) != (y["dst"] is None):
            return {"status": "differ_dst", "index": i, "detail": "presence"}
        if x["dst"] is not None:
            for field in ("type", "index", "mask"):
                if x["dst"][field] != y["dst"][field]:
                    return {"status": "differ_dst", "index": i,
                            "detail": "%s %r vs %r" % (field, x["dst"][field],
                                                       y["dst"][field])}
            if x["dst"]["modifier"] != y["dst"]["modifier"]:
                return {"status": "differ_saturate", "index": i,
                        "detail": "%d vs %d" % (x["dst"]["modifier"],
                                                y["dst"]["modifier"])}
        if len(x["srcs"]) != len(y["srcs"]):
            return {"status": "differ_src_count", "index": i,
                    "detail": "%d vs %d" % (len(x["srcs"]), len(y["srcs"]))}
        for j, (sx, sy) in enumerate(zip(x["srcs"], y["srcs"])):
            for field in ("type", "index", "swizzle"):
                if sx[field] != sy[field]:
                    return {"status": "differ_src_%s" % field, "index": i,
                            "detail": "src%d %r vs %r" % (
                                j, sx[field], sy[field])}
            if sx["modifier"] != sy["modifier"]:
                return {"status": "differ_src_modifier", "index": i,
                        "detail": "src%d %d vs %d" % (j, sx["modifier"],
                                                      sy["modifier"])}
            if sx.get("relative") != sy.get("relative"):
                return {"status": "differ_relative", "index": i,
                        "detail": "src%d %r vs %r" % (j, sx.get("relative"),
                                                      sy.get("relative"))}
    if Counter(reg for reg, _ in mj["defs"]) != Counter(
            reg for reg, _ in proj["defs"]):
        return {"status": "differ_def_count", "index": -1,
                "detail": "mojoshader=%d project=%d" % (
                    len(mj["defs"]), len(proj["defs"]))}
    mj_defs = defaultdict(list)
    for reg, values in mj["defs"]:
        mj_defs[reg].append(values)
    proj_defs = defaultdict(list)
    for reg, values in proj["defs"]:
        proj_defs[reg].append(values)
    for reg in proj_defs:
        left = sorted(mj_defs.get(reg, []))
        right = sorted(proj_defs[reg])
        if len(left) != len(right):
            return {"status": "differ_def", "index": reg, "detail": "count"}
        for lv, rv in zip(left, right):
            for k in range(4):
                if abs(lv[k] - rv[k]) > 1e-6:
                    return {"status": "differ_def", "index": reg,
                            "detail": "c%d.%d %r vs %r" % (reg, k, lv[k], rv[k])}
    mj_decls = Counter(_decl_key(d) for d in mj["decls"])
    proj_decls = Counter(_decl_key(d) for d in proj["decls"])
    if mj_decls != proj_decls:
        return {"status": "differ_decl", "index": -1,
                "detail": "mojoshader=%r project=%r" % (
                    sorted(mj_decls), sorted(proj_decls))}
    return {"status": "equal", "index": -1, "detail": ""}


# --------------------------------------------------------------------------
# Corpus loading

def read_chunk_dump(path: str) -> List[bytes]:
    with open(path, "rb") as handle:
        data = handle.read()
    count = struct.unpack_from("<I", data, 0)[0]
    pos = 4
    out = []
    for _ in range(count):
        size = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        out.append(data[pos:pos + size])
        pos += size
    return out


def read_ir_dump(path: str) -> Dict[int, Tuple[int, str]]:
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:7] != b"SM3IR1\x00":
        raise SystemExit("bad IR magic in %s" % path)
    count = struct.unpack_from("<I", data, 7)[0]
    pos = 11
    out = {}
    for _ in range(count):
        index, status, instructions, text_len = struct.unpack_from(
            "<IIII", data, pos)
        pos += 16
        text = data[pos:pos + text_len].decode("utf-8", "replace")
        pos += text_len
        out[index] = (status, text)
    return out


def run(corpus_dir: str, out_path: str) -> dict:
    with open(os.path.join(corpus_dir, "corpus.json"), encoding="utf-8") as handle:
        corpus = json.load(handle)

    buckets = {}
    for kind in ("ps", "vs"):
        chunks = read_chunk_dump(os.path.join(corpus_dir, "%s_chunks.bin" % kind))
        ir = read_ir_dump(os.path.join(corpus_dir, "%s_ir.bin" % kind))
        buckets[kind] = (chunks, ir)

    rows = corpus["rows"]
    # Unique instruction streams: one representative per bytecode SHA-256.
    unique: Dict[str, dict] = {}
    for row in rows:
        unique.setdefault(row["sha256"], row)

    results = []
    for row in unique.values():
        chunks, ir = buckets[row["kind"]]
        status, text = ir[row["index"]]
        if status != 0:
            results.append({**row, "status": "parse_error_mojoshader",
                            "detail": text[:160], "index": -1})
            continue
        try:
            mj = parse_mojoshader(text)
        except ParseProblem as exc:
            results.append({**row, "status": "parse_error_differential",
                            "detail": str(exc), "index": -1})
            continue
        proj = parse_project(chunks[row["index"]])
        verdict = compare(mj, proj)
        results.append({**row, **verdict})

    statuses = Counter(r["status"] for r in results)
    per_family = defaultdict(Counter)
    for row in results:
        per_family[row["family"]][row["status"]] += 1
    detail = Counter()
    examples = defaultdict(list)
    for row in results:
        if row["status"] != "equal":
            key = "%s: %s" % (row["status"], row["detail"])
            detail[key] += 1
            if len(examples[row["status"]]) < 3:
                examples[row["status"]].append({
                    "sha256": row["sha256"][:16],
                    "family": row["family"],
                    "package": row["package"],
                    "index": row["index"],
                    "detail": row["detail"],
                })

    chunk_counts = Counter(row["kind"] for row in rows)
    unique_equal = statuses.get("equal", 0)
    unique_total = len(results)
    equal_chunks = sum(1 for row in rows if row["sha256"] in
                       {r["sha256"] for r in results if r["status"] == "equal"})
    report = {
        "chunks": len(rows),
        "chunk_kinds": dict(chunk_counts),
        "unique_streams": unique_total,
        "unique_program_streams": len({row["program_sha1"] for row in rows}),
        "unique_equal": unique_equal,
        "unique_differ": unique_total - unique_equal,
        "chunks_equal": equal_chunks,
        "chunks_differ": len(rows) - equal_chunks,
        "statuses": dict(statuses),
        "status_by_family": {k: dict(v) for k, v in sorted(per_family.items())},
        "top_divergences": [{"reason": k, "count": v}
                            for k, v in detail.most_common(25)],
        "examples": dict(examples),
        "note": ("instruction_count is never compared; declarations, literals "
                 "and bodies are compared field by field"),
    }
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2)
        handle.write("\n")
    return report


def main(argv=None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=r"E:\host_sm3")
    parser.add_argument("--out", default=r"E:\host_sm3\differential.json")
    args = parser.parse_args(argv)
    report = run(args.corpus, args.out)
    print(json.dumps({k: report[k] for k in (
        "chunks", "unique_streams", "unique_equal", "unique_differ",
        "chunks_equal", "chunks_differ", "statuses")}, indent=2))
    print("top divergences:")
    for row in report["top_divergences"][:12]:
        print("  %6d  %s" % (row["count"], row["reason"][:110]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
