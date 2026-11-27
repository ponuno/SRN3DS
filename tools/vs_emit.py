#!/usr/bin/env python3
r"""Minimal PICA200 vertex-shader emitter for the SM3 shader plan layer.

The input is a ``pica_vs`` descriptor from ``docs/shader_plans.json`` plus the
decoded D3D9 vs_3_0 IR.  The output is picasso assembly (``.v.pica``) that uses
only straight-line arithmetic: no loops, branches, texture fetches or
address-register reads.  Anything outside the small contract is refused by a
named reason; there is no structural fallback.

Contract
--------
* attributes: D3D input reg vN is PICA attribute vN; position/texcoord/colour/
  normal/tangent/binormal are all plain attributes.  Skinning (blendweight /
  blendindices) is refused.
* outputs: position -> ``position``, texcoord index 0..2 -> ``texcoordN``,
  colour index 0 -> ``color``, colour index 1 -> ``texcoord2`` (a carry
  varying; the PS declaration must map color1 to texcoord2, and it conflicts
  with a real texcoord2 output).  Other output semantics are refused.
* uniforms: D3D ``def`` constants become ``.constf``; every other cN becomes
  row N of one ``.fvec u[N+1]`` block.  The runtime must upload the D3D float
  bank to that block; this emitter does not allocate or upload it.
* instructions: mov, add, sub, mul, mad, dp3, dp4, min, max, frc.  Source
  modifier is identity or negate; destination modifier must be identity.

Commands::

    python tools\vs_emit.py emit --manifest docs\shader_plans.json ^
        --id game.vs.0ccc7542 --out source\generated\game.vs.0ccc7542.v.pica
    python tools\vs_emit.py emit --data E:\SteamLibrary\steamapps\common\Skyrim\Data ^
        --package shadersfx\imagespace002.fxp --offset 4808 ^
        --id game.vs.0ccc7542 --out ... 
    python tools\vs_emit.py emit-c --manifest docs\generated_vs.json ^
        --out source\d3d9_generated_vs_table.inc
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Dict, List, Optional, Sequence, Tuple

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import asm  # noqa: E402
from shaders import compile as C  # noqa: E402

SUPPORTED_OPS = {
    0x01: "mov", 0x02: "add", 0x03: "sub", 0x04: "mad", 0x05: "mul",
    0x08: "dp3", 0x09: "dp4", 0x0A: "min", 0x0B: "max", 0x13: "frc",
}
INPUT_USAGE_NAMES = {0: "position", 3: "normal", 5: "texcoord", 6: "tangent",
                     7: "binormal", 10: "color"}
OUTPUT_TEXCOORD = {0: "texcoord0", 1: "texcoord1", 2: "texcoord2"}
SWZ = "xyzw"


class Refusal(Exception):
    """Named refusal; the message is the diagnostic."""


def _fmt(value: float) -> str:
    text = "%.9g" % float(value)
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"
    return text


def _mask_text(mask: int) -> str:
    if mask == 0xF:
        return ""
    return "." + "".join(SWZ[i] for i in range(4) if mask & (1 << i))


def _src_name(src: asm.Src, const_names: Dict[int, str],
              const_map: Dict[int, str]) -> str:
    if src.relative:
        raise Refusal("relative_addressing_not_supported")
    if src.reg_type == asm.REG_CONST:
        name = const_names.get(src.index) or const_map.get(src.index)
        if name is None:
            raise Refusal("constant_c%d_not_declared" % src.index)
    elif src.reg_type == asm.REG_INPUT:
        name = "v%d" % src.index
    elif src.reg_type == asm.REG_TEMP:
        name = "r%d" % src.index
    else:
        raise Refusal("source_register_type_%d_not_supported" % src.reg_type)
    if src.swizzle != "xyzw":
        name += "." + src.swizzle
    if src.modifier == 0:
        return name
    if src.modifier == 1:
        return "-" + name
    raise Refusal("source_modifier_%s_not_supported" %
                  asm.SRC_MODIFIER_NAMES.get(src.modifier, str(src.modifier)))


def _src_operand(src: asm.Src, const_names: Dict[int, str],
                 const_map: Dict[int, str]) -> Tuple[str, str]:
    """(text, kind) where kind is reg / uniform / const."""
    text = _src_name(src, const_names, const_map)
    if src.reg_type == asm.REG_CONST:
        return text, ("const" if src.index in const_names else "uniform")
    return text, "reg"


def _negate(text: str) -> str:
    return text[1:] if text.startswith("-") else "-" + text


def _binary(lines: List[str], op: str, dst: str,
            a: Tuple[str, str], b: Tuple[str, str], scratch: str) -> None:
    """Emit a commutative two-source op.

    picasso rejects a uniform/constant in the second ALU slot (``add`` is the
    strict one).  Put such an operand first, or stage it through a scratch reg.
    """
    a_text, a_kind = a
    b_text, b_kind = b
    if b_kind != "reg":
        if a_kind == "reg":
            a_text, b_text = b_text, a_text
            a_kind, b_kind = b_kind, a_kind
        else:
            lines.append("    mov %s, %s" % (scratch, b_text))
            b_text, b_kind = scratch, "reg"
    lines.append("    %s %s, %s, %s" % (op, dst, a_text, b_text))


def _lower_alu(ins: asm.Instruction, dst: str, operands: List[Tuple[str, str]],
               lines: List[str], scratch: str) -> None:
    op = ins.opcode
    if op == 0x01:                                  # mov
        lines.append("    mov %s, %s" % (dst, operands[0][0]))
    elif op == 0x02:                                # add
        _binary(lines, "add", dst, operands[0], operands[1], scratch)
    elif op == 0x03:                                # sub
        _binary(lines, "add", dst, operands[0],
                (_negate(operands[1][0]), operands[1][1]), scratch)
    elif op == 0x05:                                # mul
        _binary(lines, "mul", dst, operands[0], operands[1], scratch)
    elif op == 0x04:                                # mad
        a, b, c = operands
        if c[1] != "reg":
            # PICA MAD's third source is a register only; use mul + add.
            _binary(lines, "mul", scratch, a, b, scratch)
            _binary(lines, "add", dst, (scratch, "reg"), c, scratch)
        else:
            lines.append("    mad %s, %s, %s, %s" % (dst, a[0], b[0], c[0]))
    elif op in (0x08, 0x09):                        # dp3 / dp4
        name = "dp3" if op == 0x08 else "dp4"
        a, b = operands
        if b[1] != "reg":
            if a[1] == "reg":
                a, b = b, a
            else:
                lines.append("    mov %s, %s" % (scratch, b[0]))
                b = (scratch, "reg")
        lines.append("    %s %s, %s, %s" % (name, dst, a[0], b[0]))
    elif op in (0x0A, 0x0B):                        # min / max
        _binary(lines, "min" if op == 0x0A else "max", dst,
                operands[0], operands[1], scratch)
    elif op == 0x13:                                # frc
        lines.append("    frc %s, %s" % (dst, operands[0][0]))
    else:
        raise Refusal("unsupported_opcode_%s" % ins.name)


def _dst_name(dst: asm.Dst, out_map: Dict[int, str]) -> str:
    if dst.modifier:
        raise Refusal("destination_modifier_not_supported")
    if dst.reg_type == asm.REG_TEMP:
        name = "r%d" % dst.index
    elif dst.reg_type in (asm.REG_OUTPUT, asm.REG_RASTOUT, asm.REG_ATTROUT,
                          asm.REG_COLOROUT):
        name = out_map.get((dst.reg_type, dst.index))
        if name is None:
            raise Refusal("output_register_%s%d_not_mapped" %
                          (dst.type_name, dst.index))
    else:
        raise Refusal("destination_register_type_%d_not_supported" % dst.reg_type)
    return name + _mask_text(dst.mask)


def check_plan(plan: dict) -> None:
    """Static contract check before bytecode/IR is consulted."""
    if plan.get("kind") != "pica_vs":
        raise Refusal("plan_kind_%s_not_pica_vs" % plan.get("kind"))
    if plan.get("skinning"):
        raise Refusal("skinning_unsupported")
    for item in plan.get("inputs", []):
        usage = item["usage"]
        if usage in (1, 2):
            raise Refusal("skinning_unsupported")
        if usage not in INPUT_USAGE_NAMES:
            raise Refusal("input_usage_%s_unsupported" % item["usage_name"])
    seen_position = 0
    seen_texcoord = set()
    seen_color1 = False
    for item in plan.get("outputs", []):
        usage = item["usage"]
        if usage == 0:
            seen_position += 1
        elif usage == 5:
            if item["usage_index"] > 2:
                raise Refusal("output_texcoord%d_unsupported" % item["usage_index"])
            if item["usage_index"] in seen_texcoord:
                raise Refusal("duplicate_texcoord%d_output" % item["usage_index"])
            seen_texcoord.add(item["usage_index"])
        elif usage == 10:
            if item["usage_index"] == 1:
                seen_color1 = True
            elif item["usage_index"] != 0:
                raise Refusal("output_color%d_unsupported" % item["usage_index"])
        else:
            raise Refusal("output_usage_%s_unsupported" % item["usage_name"])
    if seen_position != 1:
        raise Refusal("position_output_count_%d (need exactly 1)" % seen_position)
    if seen_color1 and 2 in seen_texcoord:
        raise Refusal("color1_conflicts_texcoord2")
    if len(plan.get("outputs", [])) > 7:
        raise Refusal("too_many_vs_varyings (%d > 7)" % len(plan["outputs"]))
    for name in plan.get("expandable", []):
        raise Refusal("expandable_opcode_%s_not_implemented" % name)
    if plan.get("instructions", 0) > C.VS_MAX_INSTRUCTIONS:
        raise Refusal("too_many_vs_instructions (%d > %d)" %
                      (plan["instructions"], C.VS_MAX_INSTRUCTIONS))


def _output_map(plan: dict) -> Dict[Tuple[int, int], str]:
    out: Dict[Tuple[int, int], str] = {}
    for item in plan["outputs"]:
        key = (item["type"], item["reg_index"])
        if item["usage"] == 0:
            out[key] = "outpos"
        elif item["usage"] == 5:
            out[key] = "outtc%d" % item["usage_index"]
        elif item["usage"] == 10:
            out[key] = "outclr" if item["usage_index"] == 0 else "outclr1"
    return out


def emit_plan(plan: dict, ir: C.IR) -> str:
    """Return picasso assembly for one plan.  Raises Refusal by name."""
    check_plan(plan)
    if ir.error:
        raise Refusal("parse_error_%s" % ir.error)
    if ir.sha256 != plan["sha256"]:
        raise Refusal("ir_hash_mismatch")
    out_map = _output_map(plan)

    defs = {ins.dst.index: tuple(ins.literal) for ins in ir.body
            if ins.opcode == 0x51 and ins.dst is not None}
    constants = sorted({s.index for ins in ir.body for s in ins.srcs
                        if s.reg_type == asm.REG_CONST})
    const_names = {index: "d%d" % index for index in defs}
    nondef = [i for i in constants if i not in defs]
    max_nondef = max(nondef) if nondef else -1
    if max_nondef >= C.VS_MAX_CONSTANTS:
        raise Refusal("too_many_vs_constants (%d > %d)" %
                      (max_nondef + 1, C.VS_MAX_CONSTANTS))
    const_map = {i: "cu[%d]" % i for i in nondef}

    lines = [
        "; Generated by tools/vs_emit.py from plan %s" % plan["id"],
        "; D3D9 vs_3_0 SHA-256 %s" % plan["sha256"],
        "; Straight-line PICA VS: no loops, branches, texld or address reads.",
        "; Inputs: " + ", ".join(
            "v%d=%s%d" % (i["reg_index"], i["usage_name"], i["usage_index"])
            for i in plan["inputs"]),
    ]
    if max_nondef >= 0:
        lines.append(".fvec cu[%d]" % max(4, max_nondef + 1))
        lines.append("; c0..c%d upload to the cu block in order" % max_nondef)
    for index in sorted(defs):
        values = defs[index]
        lines.append(".constf d%d(%s)" % (
            index, ", ".join(_fmt(v) for v in values)))
    for item in plan["outputs"]:
        name = out_map[(item["type"], item["reg_index"])]
        if item["usage"] == 0:
            semantic = "position"
        elif item["usage"] == 5:
            semantic = OUTPUT_TEXCOORD[item["usage_index"]]
        elif item["usage"] == 10 and item["usage_index"] == 1:
            semantic = "texcoord2"
        else:
            semantic = "color"
        lines.append(".out %s %s" % (name, semantic))
    lines.append("")
    lines.append(".proc main")
    used_temps = set()
    for ins in ir.body:
        if ins.dst is not None and ins.dst.reg_type == asm.REG_TEMP:
            used_temps.add(ins.dst.index)
        for src in ins.srcs:
            if src.reg_type == asm.REG_TEMP:
                used_temps.add(src.index)
    scratch = None
    for candidate in range(15, -1, -1):
        if candidate not in used_temps:
            scratch = "r%d" % candidate
            break
    if scratch is None:
        raise Refusal("no_free_temporary_register")
    for ins in ir.body:
        if ins.opcode == 0x51:
            continue
        if ins.opcode == 0x00:
            continue
        if ins.opcode not in SUPPORTED_OPS:
            raise Refusal("unsupported_opcode_%s" % ins.name)
        if ins.dst is None:
            raise Refusal("opcode_%s_without_destination" % ins.name)
        operands = [_src_operand(src, const_names, const_map)
                    for src in ins.srcs]
        _lower_alu(ins, _dst_name(ins.dst, out_map), operands, lines, scratch)
    lines.append("    end")
    lines.append(".end")
    lines.append("")
    return "\n".join(lines)


def _find_chunk(plan: dict, data_dir: str = "") -> Tuple[bytes, str, int]:
    import fxp_compile as F
    for package in F._load_packages(F.find_data(data_dir)):
        for chunk in package.chunks:
            if chunk.kind != "vs":
                continue
            ir = C.build_ir(chunk.code)
            if ir.sha256 == plan["sha256"]:
                return chunk.code, package.path, chunk.offset
    raise Refusal("bytecode_for_%s_not_found" % plan["sha256"][:12])


def _load_plan(manifest_path: str, plan_id: str) -> dict:
    with open(manifest_path, "r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    for entry in manifest.get("entries", []):
        if entry.get("id") == plan_id:
            return entry
    raise Refusal("plan_%s_not_found" % plan_id)


def _entry_from_plan(plan: dict, asm_name: str, source_package: str,
                     source_offset: int) -> dict:
    return {
        "id": plan["id"],
        "sha256": plan["sha256"],
        "asm": asm_name,
        "source_package": source_package,
        "source_offset": source_offset,
        "instructions": plan["instructions"],
        "inputs": plan["inputs"],
        "outputs": plan["outputs"],
        "constants": plan["constants"],
    }


def _merge_generated(path: str, entry: dict) -> None:
    """Append-only: never replace an existing hash with different bytes."""
    manifest = {"schema": "d9-generated-vs-v1", "entries": []}
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
    entries = manifest.setdefault("entries", [])
    for old in entries:
        if old["sha256"] == entry["sha256"]:
            if old.get("asm") != entry.get("asm"):
                raise Refusal("append_only_conflict_for_%s" % entry["sha256"][:12])
            return
    entries.append(entry)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")


def emit_c_table(manifest_path: str) -> str:
    with open(manifest_path, "r", encoding="utf-8") as handle:
        manifest = json.load(handle)
    lines = [
        "/* Generated by tools/vs_emit.py emit-c.  Do not edit. */",
        "/* Exact SHA-256 -> emitted PICA VS id.  No bytecode is stored. */",
        "typedef struct {",
        "    const char *id;",
        "    const char *sha256;",
        "    const char *asm_name;",
        "} D9GeneratedVsEntry;",
        "static const D9GeneratedVsEntry d9_generated_vs_entries[] = {",
    ]
    for entry in sorted(manifest.get("entries", []),
                        key=lambda e: e["sha256"]):
        lines.append('    {"%s", "%s", "%s"},' % (
            entry["id"], entry["sha256"], entry.get("asm", "")))
    lines.append("};")
    lines.append("#define D9_GENERATED_VS_ENTRY_COUNT "
                 "(sizeof(d9_generated_vs_entries)/sizeof(d9_generated_vs_entries[0]))")
    lines.append("")
    return "\n".join(lines)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_emit = sub.add_parser("emit")
    p_emit.add_argument("--manifest", default=os.path.join(REPO, "docs",
                                                           "shader_plans.json"))
    p_emit.add_argument("--id", default="")
    p_emit.add_argument("--data", default="")
    p_emit.add_argument("--package", default="")
    p_emit.add_argument("--offset", type=int, default=-1)
    p_emit.add_argument("--out", required=True)
    p_emit.add_argument("--table", default="",
                        help="append the emitted id to this generated_vs table")

    p_c = sub.add_parser("emit-c")
    p_c.add_argument("--manifest", default=os.path.join(REPO, "docs",
                                                         "generated_vs.json"))
    p_c.add_argument("--out", default=os.path.join(
        REPO, "source", "d3d9_generated_vs_table.inc"))

    args = parser.parse_args(argv)
    if args.cmd == "emit-c":
        text = emit_c_table(args.manifest)
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("wrote %s" % args.out)
        return 0

    try:
        if args.package and args.offset >= 0:
            import fxp_compile as F
            code = None
            package = args.package
            offset = args.offset
            for pkg in F._load_packages(F.find_data(args.data)):
                if pkg.path.lower() != args.package.lower():
                    continue
                package = pkg.path
                for chunk in pkg.chunks:
                    if chunk.offset == args.offset:
                        code = chunk.code
                        break
            if code is None:
                raise Refusal("bytecode_not_found")
            plan, cls, why = C.plan_vs(C.build_ir(code))
            if plan is None:
                raise Refusal(why or "plan_not_pica_vs")
            plan = dict(plan)
            plan["id"] = args.id or ("game.vs.%s" % plan["sha256"][:8])
        elif args.id:
            plan = _load_plan(args.manifest, args.id)
            code, package, offset = _find_chunk(plan, args.data)
        else:
            raise Refusal("need --id or --package/--offset")
        ir = C.build_ir(code)
        text = emit_plan(plan, ir)
    except Refusal as exc:
        print("REFUSED: %s" % exc, file=sys.stderr)
        return 2

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("wrote %s (%s, %d instructions)" %
          (args.out, plan["id"], plan["instructions"]))
    if args.table:
        asm_name = os.path.basename(args.out)
        try:
            _merge_generated(args.table, _entry_from_plan(plan, asm_name,
                                                          package, offset))
        except Refusal as exc:
            print("REFUSED: %s" % exc, file=sys.stderr)
            return 2
        print("updated %s" % args.table)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
