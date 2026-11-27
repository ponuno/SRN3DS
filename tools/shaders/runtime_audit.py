"""Runtime-path audit for every committed shader plan.

A plan row in ``plans.json`` is not a runtime path.  This tool joins the three
independent runtime artifacts the 3DS build actually has:

  * ``source/d3d9_pica_vs_*_table.inc``  -- executable PICA VS programs,
    keyed by the exact vs_3_0 bytecode SHA-256; the embedded ``.shbin`` bytes
    are checked (DVLB/DVLE/DVLP, main word count, SHA-256 pin).
  * ``source/d3d9_shader_plan_table.inc`` -- TEV/pica_vs descriptors generated
    from ``docs/shader_plans.json``; a draw-enabled TEV plan must have a row
    here, otherwise ``d9_shader_plan_lookup`` misses it at create time.
  * ``docs/shader_plans.json`` -- the canonical merged manifest.

For every plan with ``draw_enabled == 1`` the audit names one of:

  ``pica_vs:<program>``  exact-hash PICA VS program (validated),
  ``tev:plan_table``     descriptor present in the generated C table,
  *unmatched*            no runtime path, with the reason.

It then statically validates every PICA VS program and every TEV descriptor
(stage/register/pass/texture/constant limits) without a device.

Usage::

    python tools\\shaders\\runtime_audit.py
    python tools\\shaders\\runtime_audit.py --json docs\\runtime_audit.json
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__).replace("\\", "/"))))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "shaders"))

PLANS_PATH = os.path.join(REPO, "docs", "shader_plans.json")
PACKS_DIR = os.path.join(REPO, "source", "shader_packs")
VS_TABLE_GLOB = os.path.join(REPO, "source", "d3d9_pica_vs_table.inc")
VS_TABLE_GLOB_EXTRA = os.path.join(REPO, "source", "d3d9_pica_vs_*_table.inc")
VS_SHIN_GLOB = os.path.join(REPO, "source", "d3d9_pica_vs_shbin.inc")
VS_SHIN_GLOB_EXTRA = os.path.join(REPO, "source", "d3d9_pica_vs_*_shbin.inc")
PLAN_TABLE_PATH = os.path.join(REPO, "source", "d3d9_shader_plan_table.inc")

MAX_VARYINGS = 7
MAX_UNIFORM_ROWS = 32          # PICA VS bank c0..c31
MAX_VS_ATTRS = 5
MAX_TEV_STAGES = 6
MAX_TEV_TEXTURES = 3
MAX_TEV_CONSTANTS = 8
MAX_PASSES = 4
TEV_FUNCS = set(range(0, 10))
TEV_SOURCES = set(range(0, 16))
TEV_RGB_OPS = {0, 1, 2, 3, 4, 5, 8, 9, 12, 13}
TEV_A_OPS = set(range(0, 8))
CONST_REG_MAX = 31
ATTR_NAMES = {0: "position", 1: "blendweight", 2: "blendindices",
              3: "texcoord", 4: "color"}


def _load(path: str):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def pack_documents():
    out = []
    for path in sorted(glob.glob(os.path.join(PACKS_DIR, "*", "plans.json"))):
        out.append((path, _load(path)))
    return out


def census_hashes() -> set:
    hashes = set()
    for path in glob.glob(os.path.join(PACKS_DIR, "*", "manifest.json")):
        manifest = _load(path)
        for row in manifest.get("hashes", []):
            if isinstance(row, list) and row:
                hashes.add(row[0])
    return hashes


def _int_array(text: str, symbol: str):
    match = re.search(r"static const uint8_t %s\[(\d+)\]\s*=\s*\{(.*?)\};"
                      % re.escape(symbol), text, re.S)
    if not match:
        return None, 0
    declared = int(match.group(1))
    values = bytes(int(v, 16) for v in
                   re.findall(r"0x([0-9a-fA-F]{2})", match.group(2)))
    return values, declared


def parse_vs_catalog() -> dict:
    shbin_bytes = {}
    for path in sorted(glob.glob(VS_SHIN_GLOB) +
                       glob.glob(VS_SHIN_GLOB_EXTRA)):
        text = open(path, encoding="utf-8").read()
        for match in re.finditer(
                r"static const uint8_t (\w+)\[(\d+)\]\s*=\s*\{(.*?)\};",
                text, re.S):
            shbin_bytes[match.group(1)] = (
                bytes(int(v, 16) for v in
                      re.findall(r"0x([0-9a-fA-F]{2})", match.group(3))),
                int(match.group(2)))

    programs = {}
    head = re.compile(
        r'"([^"]+)",\s*"([0-9a-f]{64})",\s*"([^"]+)",\s*"([^"]+)",\s*'
        r'"([0-9a-f]{64})",\s*(\w+),\s*(\d+),\s*(\d+)u,\s*(\d+)u,\s*'
        r'(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,', re.S)
    attr = re.compile(
        r"\{\s*(D9_PICA_VS_ATTR_\w+),\s*(D9_DECL_\w+),\s*(\d+),\s*"
        r"(D9_PICA_VS_FMT_\w+),\s*(\d+)u\s*\}")
    for path in sorted(glob.glob(VS_TABLE_GLOB) +
                       glob.glob(VS_TABLE_GLOB_EXTRA)):
        text = open(path, encoding="utf-8").read()
        matches = list(head.finditer(text))
        for position, match in enumerate(matches):
            chunk = text[match.end():
                         matches[position + 1].start()
                         if position + 1 < len(matches) else len(text)]
            (pid, sha, asm_name, shbin_name, shbin_sha, symbol, size,
             instructions, uniform_rows, bone_rows, wvp_first_row,
             wvp_rows, attr_count) = match.groups()
            attrs = []
            for row in attr.finditer(chunk):
                attrs.append({
                    "attr": row.group(1),
                    "decl_type": row.group(2),
                    "components": int(row.group(3)),
                    "format": row.group(4),
                    "reg": int(row.group(5)),
                })
            programs[sha] = {
                "id": pid, "sha256": sha, "asm_name": asm_name,
                "shbin_name": shbin_name, "shbin_sha256": shbin_sha,
                "symbol": symbol, "shbin_size": int(size),
                "instructions": int(instructions),
                "uniform_rows": int(uniform_rows),
                "bone_rows": int(bone_rows),
                "wvp_first_row": int(wvp_first_row),
                "wvp_rows": int(wvp_rows),
                "attr_count": int(attr_count), "attrs": attrs,
                "source": os.path.basename(path),
                "shbin": shbin_bytes.get(symbol, (b"", 0))[0],
                "shbin_declared": shbin_bytes.get(symbol, (b"", 0))[1],
            }
    return programs


def parse_plan_table() -> dict:
    text = open(PLAN_TABLE_PATH, encoding="utf-8").read()
    rows = {}
    for match in re.finditer(
            r'\{\s*\n\s*"([^"]+)",\s*\n\s*"([0-9a-f]{64})",\s*\n\s*'
            r'(\d+),\s*(\d+),\s*(\d+),', text):
        rows[match.group(2)] = {"id": match.group(1),
                                "stage": int(match.group(3)),
                                "kind": int(match.group(4)),
                                "draw_enabled": int(match.group(5))}
    return rows


def merged_plans(documents):
    """One entry per SHA-256; a conflicting duplicate is a hard error."""
    plans = {}
    conflicts = []
    for origin, doc in documents:
        for entry in doc.get("entries", []):
            sha = entry.get("sha256")
            seen = plans.get(sha)
            if seen is None:
                plans[sha] = (origin, entry)
            elif seen[1] != entry:
                conflicts.append("%s vs %s for %s"
                                 % (origin, seen[0], str(sha)[:16]))
    return plans, conflicts


def validate_vs_program(program: dict, plan_entry) -> list:
    errors = []
    pid = program["id"]

    def fail(text):
        errors.append("%s: %s" % (pid, text))

    if not re.fullmatch(r"[a-z0-9]+(?:\.[a-z0-9]+)*\.vs\.[0-9a-f]{8}", pid):
        fail("id is not <family>.vs.<8 hex>")
    if not re.fullmatch(r"[0-9a-f]{64}", program["shbin_sha256"]):
        fail("pinned shbin SHA-256 is not 64 lower-case hex")
    if program["instructions"] < 1:
        fail("main word count is zero")
    if not 1 <= program["uniform_rows"] <= MAX_UNIFORM_ROWS:
        fail("uniform bank %u rows exceeds c0..c31"
             % program["uniform_rows"])
    if program["bone_rows"] == 0:
        if program["wvp_first_row"] != 0 or \
                program["wvp_rows"] != program["uniform_rows"]:
            fail("menu identity bank is inconsistent")
    elif program["bone_rows"] == 12:
        if program["uniform_rows"] < 16 or program["wvp_first_row"] != 12 \
                or program["wvp_rows"] != 4:
            fail("4-bone palette/WVP bank is inconsistent")
    else:
        fail("bone_rows must be 0 or 12")
    if not 1 <= program["attr_count"] <= MAX_VS_ATTRS:
        fail("attr_count %u outside 1..%u"
             % (program["attr_count"], MAX_VS_ATTRS))
    if len(program["attrs"]) != program["attr_count"]:
        fail("attr_count %u but %u attribute rows"
             % (program["attr_count"], len(program["attrs"])))
    regs = set()
    for row in program["attrs"]:
        if row["reg"] > 15:
            fail("attribute uses PICA input v%u (max v15)" % row["reg"])
        if row["reg"] in regs:
            fail("attribute register v%u is duplicated" % row["reg"])
        regs.add(row["reg"])
        if not 1 <= row["components"] <= 4:
            fail("attribute v%u has %u components"
                 % (row["reg"], row["components"]))
        if row["format"] not in ("D9_PICA_VS_FMT_FLOAT",
                                 "D9_PICA_VS_FMT_UBYTE_NORM"):
            fail("attribute v%u has an unknown loader format" % row["reg"])

    data = program["shbin"]
    declared = program["shbin_declared"]
    if not data:
        fail("embedded .shbin bytes are missing")
        return errors
    if declared != len(data):
        fail("declared shbin length %u != %u bytes"
             % (declared, len(data)))
    if program["shbin_size"] != len(data):
        fail("table shbin_size %u != %u bytes"
             % (program["shbin_size"], len(data)))
    if len(data) < 0x30 or data[:4] != b"DVLB":
        fail(".shbin is not a DVLB image")
        return errors
    if int.from_bytes(data[4:8], "little") != 1:
        fail(".shbin does not contain exactly one DVLE")
    dvle = int.from_bytes(data[8:12], "little")
    if dvle < 0x10 or dvle > len(data) - 0x30 or \
            data[dvle:dvle + 4] != b"DVLE":
        fail("DVLE offset 0x%x is invalid" % dvle)
        return errors
    if data[0x0c:0x10] != b"DVLP":
        fail("DVLP block is missing")
    words = int.from_bytes(data[0x18:0x1c], "little")
    if words != program["instructions"]:
        fail(".shbin main is %u words, catalog says %u"
             % (words, program["instructions"]))
    digest = hashlib.sha256(data).hexdigest()
    if digest != program["shbin_sha256"]:
        fail(".shbin SHA-256 %s... != pinned %s..."
             % (digest[:12], program["shbin_sha256"][:12]))

    if plan_entry is None:
        return errors
    plan, origin = plan_entry[1], plan_entry[0]
    if plan.get("kind") != "pica_vs":
        fail("plan for this hash is %r, not pica_vs" % plan.get("kind"))
        return errors
    if plan.get("draw_enabled") != 1:
        # A translated plan may keep a stale proof block; it is not drawn.
        return errors
    emitter = plan.get("emitter") or {}
    if emitter.get("shbin_sha256") != program["shbin_sha256"]:
        fail("plan emitter shbin SHA-256 differs from the catalog")
    if emitter.get("shbin_bytes") != len(data):
        fail("plan emitter shbin_bytes %r != embedded %u"
             % (emitter.get("shbin_bytes"), len(data)))
    if emitter.get("uniform_rows") != program["uniform_rows"]:
        fail("plan emitter uniform_rows %r != catalog %u"
             % (emitter.get("uniform_rows"), program["uniform_rows"]))
    outputs = plan.get("outputs") or []
    if not 1 <= len(outputs) <= MAX_VARYINGS:
        fail("plan has %u outputs (max %u varying)" % (len(outputs),
                                                       MAX_VARYINGS))
    out_regs = [o.get("reg_index") for o in outputs]
    if len(set(out_regs)) != len(out_regs):
        fail("plan output registers are duplicated")
    sem = [o.get("usage_name") for o in outputs]
    if sem.count("position") > 1:
        fail("plan writes more than one position output")
    if sem.count("color") > 1:
        fail("plan writes colour0 and colour1; PICA has one primary colour")
    if sem.count("texcoord") > 3:
        fail("plan writes more than texcoord0..2")
    if any(name not in ("position", "color", "texcoord") for name in sem):
        fail("plan uses an output semantic PICA cannot bind: %r" % sem)
    texcoord_out = any(o.get("usage_name") == "texcoord" for o in outputs)
    has_texcoord_attr = any(row["attr"] == "D9_PICA_VS_ATTR_TEXCOORD"
                            for row in program["attrs"])
    if has_texcoord_attr and not texcoord_out:
        fail("program reads TEXCOORD but the plan exposes no texcoord varying")
    plan_inputs = emitter.get("inputs") or plan.get("inputs") or []
    if plan_inputs and len(plan_inputs) != program["attr_count"]:
        fail("plan declares %u inputs, catalog binds %u attributes"
             % (len(plan_inputs), program["attr_count"]))
    if plan.get("draw_enabled") == 1 and not emitter:
        fail("draw-enabled plan has no emitter proof block")
    return errors

def _stage_errors(label: str, row: dict, constants: set, derived: set) -> list:
    errors = []

    def fail(text):
        errors.append("%s: %s" % (label, text))

    for key in ("func_rgb", "func_a"):
        if row.get(key) not in TEV_FUNCS:
            fail("%s %r outside 0..9" % (key, row.get(key)))
    for key in ("src_rgb", "src_a"):
        values = row.get(key) or []
        if len(values) != 3:
            fail("%s needs 3 sources" % key)
        for value in values:
            if value not in TEV_SOURCES:
                fail("%s source %r outside 0..15" % (key, value))
    for key, allowed in (("op_rgb", TEV_RGB_OPS), ("op_a", TEV_A_OPS)):
        values = row.get(key) or []
        if len(values) != 3:
            fail("%s needs 3 selectors" % key)
        for value in values:
            if value not in allowed:
                fail("%s selector %r is not a PICA encoding" % (key, value))
    for key in ("scale_rgb", "scale_a"):
        if row.get(key, 0) not in (0, 1, 2):
            fail("%s %r outside 0..2" % (key, row.get(key)))
    reg = row.get("const_reg", 255)
    if reg != 255:
        if not isinstance(reg, int) or not 0 <= reg <= CONST_REG_MAX:
            fail("const c%r outside the c0..c31 bank" % reg)
        elif constants and reg not in constants:
            fail("const c%d is not in the constants list" % reg)
        if reg not in constants and reg not in derived:
            fail("const c%d is neither uploaded nor derived" % reg)
    return errors


def validate_tev(entry: dict) -> list:
    errors = []
    label = entry.get("id") or "(unnamed)"

    def fail(text):
        errors.append("%s: %s" % (label, text))

    stages = entry.get("stages")
    if not isinstance(stages, int) or not 1 <= stages <= MAX_TEV_STAGES:
        fail("stages %r outside 1..%u" % (stages, MAX_TEV_STAGES))
        return errors
    stage_data = entry.get("stage_data")
    if not isinstance(stage_data, list) or len(stage_data) != stages:
        fail("stage_data has %r rows for %d stages"
             % (len(stage_data) if isinstance(stage_data, list) else "?",
                stages))
        return errors
    constants = set()
    for reg in entry.get("constants") or []:
        if not isinstance(reg, int) or not 0 <= reg <= CONST_REG_MAX:
            fail("constant c%r outside the c0..c31 bank" % reg)
        else:
            constants.add(reg)
    if len(constants) > MAX_TEV_CONSTANTS:
        fail("%u constants exceed the %u-slot PICA bank"
             % (len(constants), MAX_TEV_CONSTANTS))
    derived = set()
    for index, op in enumerate(entry.get("constant_ops") or []):
        dst = op.get("dst_reg", op.get("dst"))
        if not isinstance(dst, int) or not 0 <= dst <= CONST_REG_MAX:
            fail("constant op %d dst c%r outside c0..c31" % (index, dst))
        else:
            derived.add(dst)
        need = {"pack": 1, "add": 2, "mul": 2, "lerp": 3}.get(op.get("op"), 0)
        if not need:
            fail("constant op %d has unknown op %r" % (index, op.get("op")))
        src = op.get("src") or []
        if len(src) != need:
            fail("constant op %d needs %d sources" % (index, need))
        for item in src:
            reg = item.get("reg")
            if not isinstance(reg, int) or not 0 <= reg <= CONST_REG_MAX:
                fail("constant op %d reads c%r outside c0..c31"
                     % (index, reg))
            swizzle = item.get("swizzle")
            if not (isinstance(swizzle, str) and len(swizzle) == 4
                    and set(swizzle) <= set("xyzw")):
                fail("constant op %d has a bad swizzle" % index)
    for index, row in enumerate(stage_data):
        errors.extend(_stage_errors("%s stage %d" % (label, index), row,
                                    constants, derived))
    textures = entry.get("textures") or []
    if len(textures) > MAX_TEV_TEXTURES:
        fail("%u textures exceed the %u-slot PICA unit"
             % (len(textures), MAX_TEV_TEXTURES))
    for index, texture in enumerate(textures):
        if texture.get("sampler_type") not in (2, 3, 4):
            fail("texture %d has sampler_type %r"
                 % (index, texture.get("sampler_type")))
        for key in ("sampler_reg", "texcoord_reg"):
            value = texture.get(key)
            if not isinstance(value, int) or not 0 <= value <= 15:
                fail("texture %d %s %r outside 0..15"
                     % (index, key, value))

    passes = entry.get("passes")
    if passes is not None:
        if not isinstance(passes, list) or not 2 <= len(passes) <= MAX_PASSES:
            fail("passes must hold 2..%u passes" % MAX_PASSES)
        else:
            if passes[0].get("stage_data") != stage_data:
                fail("passes[0].stage_data does not mirror the main plan")
            for index, item in enumerate(passes):
                if not isinstance(item.get("stage_data"), list) or \
                        not 1 <= len(item["stage_data"]) <= MAX_TEV_STAGES:
                    fail("pass %d stage_data is not 1..%u stages"
                         % (index, MAX_TEV_STAGES))
                if len(item.get("textures") or []) > MAX_TEV_TEXTURES:
                    fail("pass %d has more than %u textures"
                         % (index, MAX_TEV_TEXTURES))
                if not isinstance(item.get("constants"), list):
                    fail("pass %d has no constants list" % index)
                for reg in item.get("constants") or []:
                    if not isinstance(reg, int) or not 0 <= reg <= CONST_REG_MAX:
                        fail("pass %d constant c%r outside c0..c31"
                             % (index, reg))
                blend = item.get("blend")
                if not isinstance(blend, dict):
                    fail("pass %d blend is not an object" % index)
                else:
                    if blend.get("enable") not in (0, 1):
                        fail("pass %d blend.enable %r" % (index,
                                                           blend.get("enable")))
                    for key in ("src", "dst"):
                        if not isinstance(blend.get(key), int) or \
                                not 1 <= blend[key] <= 11:
                            fail("pass %d blend.%s %r outside 1..11"
                                 % (index, key, blend.get(key)))
                    if not isinstance(blend.get("op"), int) or \
                            not 1 <= blend["op"] <= 5:
                        fail("pass %d blend.op %r outside 1..5"
                             % (index, blend.get("op")))
                if item.get("depth_write") not in (0, 1):
                    fail("pass %d depth_write %r" % (index,
                                                      item.get("depth_write")))
    return errors


def audit() -> dict:
    documents = [(PLANS_PATH, _load(PLANS_PATH))]
    documents.extend(pack_documents())
    plans, conflicts = merged_plans(documents)
    census = census_hashes()
    vs_programs = parse_vs_catalog()
    table = parse_plan_table()
    docs_entries = {e["sha256"]: e for e in _load(PLANS_PATH)["entries"]}

    # The generated C table must match docs/shader_plans.json exactly.
    stale = []
    for sha, entry in docs_entries.items():
        if sha not in table:
            stale.append("%s: committed plan has no C descriptor" % entry["id"])
    for sha in table:
        if sha not in docs_entries:
            stale.append("%s: C descriptor has no committed plan" % sha[:16])

    matched, unmatched, invalid = [], [], []
    for sha, (origin, entry) in sorted(plans.items(),
                                       key=lambda kv: kv[1][1]["id"]):
        if entry.get("draw_enabled") != 1:
            continue
        kind = entry.get("kind")
        if kind == "pica_vs":
            program = vs_programs.get(sha)
            if program is None:
                unmatched.append({
                    "id": entry["id"], "sha256": sha, "kind": kind,
                    "family": entry.get("family"),
                    "in_census": sha in census,
                    "runtime": entry.get("runtime"),
                    "reason": "no PICA VS program for this exact hash",
                })
            else:
                matched.append({"id": entry["id"], "sha256": sha,
                                "path": "pica_vs:%s" % program["id"],
                                "in_census": sha in census})
        elif kind == "tev":
            runtime = table.get(sha)
            if runtime is None:
                reason = ("runtime=1 but the generated C table never emitted it"
                          if entry.get("runtime") == 1
                          else "no runtime slot claimed (runtime=%r)"
                          % entry.get("runtime"))
                unmatched.append({
                    "id": entry["id"], "sha256": sha, "kind": kind,
                    "family": entry.get("family"),
                    "in_census": sha in census,
                    "runtime": entry.get("runtime"),
                    "reason": reason,
                })
            else:
                if runtime.get("draw_enabled") != 1:
                    invalid.append("%s: C descriptor is draw-disabled"
                                   % entry["id"])
                matched.append({"id": entry["id"], "sha256": sha,
                                "path": "tev:plan_table",
                                "in_census": sha in census})
                committed = docs_entries.get(sha)
                if committed is None:
                    invalid.append("%s: TEV row is in the C table but not in "
                                   "docs/shader_plans.json" % entry["id"])
                elif committed != entry:
                    invalid.append("%s: pack row differs from the committed "
                                   "runtime descriptor" % entry["id"])
        else:
            unmatched.append({"id": entry["id"], "sha256": sha,
                              "kind": kind, "in_census": sha in census,
                              "runtime": entry.get("runtime"),
                              "reason": "unknown plan kind"})
        if kind == "tev":
            invalid.extend(validate_tev(entry))
        errors = []
        if kind == "pica_vs":
            errors = validate_vs_program(vs_programs[sha], plans[sha]) \
                if sha in vs_programs else []
        invalid.extend(errors)

    # Every catalog program is validated, even if no plan draws it.
    vs_invalid = []
    for sha, program in sorted(vs_programs.items(), key=lambda kv: kv[1]["id"]):
        errors = validate_vs_program(program, plans.get(sha))
        if errors:
            vs_invalid.extend(errors)
    invalid.extend(vs_invalid)

    # A draw-enabled plan must not hide behind a stale runtime row either.
    census_draw = [row for row in matched + unmatched if row["in_census"]]
    return {
        "schema": "pica-runtime-audit/1",
        "documents": [os.path.relpath(path, REPO) for path, _ in documents],
        "census_hashes": len(census),
        "plans": len(plans),
        "draw_enabled": len(matched) + len(unmatched),
        "draw_enabled_census": len(census_draw),
        "runtime_matched": len(matched),
        "unmatched": unmatched,
        "invalid": invalid,
        "conflicts": conflicts,
        "stale_table": stale,
        "vs_programs": len(vs_programs),
        "vs_valid": len(vs_programs) - len({e.split(":")[0]
                                            for e in vs_invalid}),
        "ok": not (unmatched or invalid or conflicts or stale),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", default="", help="write the report here")
    args = parser.parse_args(argv)
    report = audit()
    if args.json:
        os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2)
            handle.write("\n")
    print("draw-enabled: %d (%d in census), runtime paths: %d, unmatched: %d"
          % (report["draw_enabled"], report["draw_enabled_census"],
             report["runtime_matched"], len(report["unmatched"])))
    print("PICA VS programs: %d (%d valid)" %
          (report["vs_programs"], report["vs_valid"]))
    for row in report["unmatched"]:
        print("UNMATCHED %-32s %s%s: %s" % (
            row["id"], row["sha256"][:16],
            " [census]" if row["in_census"] else "", row["reason"]))
    for text in report["invalid"]:
        print("INVALID %s" % text)
    for text in report["conflicts"]:
        print("CONFLICT %s" % text)
    for text in report["stale_table"]:
        print("STALE %s" % text)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
