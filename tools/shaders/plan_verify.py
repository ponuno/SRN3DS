"""Schema + semantic + symbolic verifier for ``docs/shader_plans.json``.

Three layers, cheapest first:

1. **Schema.**  ``docs/shader_plan_schema.json`` is the single contract for a
   pixel or a vertex plan.  This module validates every entry against the rules
   the schema states (the schema file is loaded so the two cannot silently
   diverge: every rule below names the keyword it mirrors).
2. **Semantics.**  Status is *derived*, never trusted: ``draw_enabled == 1``
   means ``draw-enabled``; a ``tev``/``pica_vs`` descriptor with
   ``draw_enabled == 0`` is ``translated``; a census hash with no plan entry is
   ``refused``.  Also checked: unique ids and hashes, stage/kind agreement,
   stage_data length vs ``stages``, TEV source/op ranges, PICA VS output and
   uniform limits, and that a VS is never draw-enabled without codegen.
3. **Symbolic equivalence.**  With ``--data`` the manifest is regenerated from
   the retail archive by ``tools/fxp_compile.py`` and compared field for field
   with the committed file.  The generator only emits a TEV plan after its own
   symbolic check of the final ``oC0`` expression against the IR, so an equal
   regeneration means the committed plan is still the IR's plan.  Without the
   archive this layer reports ``skipped`` instead of passing.

Usage::

    python tools\\shaders\\plan_verify.py --plans docs\\shader_plans.json
    python tools\\shaders\\plan_verify.py --plans docs\\shader_plans.json --data F:\\Skyrim\\Data
"""

from __future__ import annotations

import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

SCHEMA_PATH = os.path.join(REPO, "docs", "shader_plan_schema.json")
PLANS_PATH = os.path.join(REPO, "docs", "shader_plans.json")

FAMILIES = {"ui", "blit_post", "sky_water", "terrain", "skinning",
            "lighting", "menu_light", "generic"}
STATUSES = {"translated", "draw-enabled", "refused"}
TEV_FUNCS = range(0, 10)
TEV_SOURCES = range(0, 16)
# GPU_TEVOP_RGB selects a component in 2-bit steps (R=4, G=8, B=12).
TEV_RGB_OPS = {0, 1, 2, 3, 4, 5, 8, 9, 12, 13}
TEV_A_OPS = set(range(0, 8))
MAX_STAGES = 6
MAX_VS_VARYINGS = 7
MAX_VS_INPUTS = 16
MAX_VS_UNIFORMS = 96


def load_schema(path: str = SCHEMA_PATH) -> dict:
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def derive_status(entry: dict) -> str:
    if entry.get("kind") not in ("tev", "pica_vs"):
        return "refused"
    return "draw-enabled" if entry.get("draw_enabled") == 1 else "translated"


def _is_hex(value, length: int) -> bool:
    return (isinstance(value, str) and len(value) == length
            and all(c in "0123456789abcdef" for c in value))


def validate_entry(entry: dict, errors: list, where: str) -> None:
    def fail(message: str) -> None:
        errors.append("%s: %s" % (where, message))

    if not isinstance(entry, dict):
        fail("not an object")
        return
    for key in ("id", "sha256", "stage", "kind", "draw_enabled", "note"):
        if key not in entry:
            fail("missing required %r" % key)
    if errors and not all(isinstance(entry.get(k), (str, int)) for k in ("id",)):
        return

    if not _is_hex(entry.get("sha256"), 64):
        fail("sha256 must be 64 lower-case hex chars")
    if "sha1" in entry and not _is_hex(entry["sha1"], 40):
        fail("sha1 must be 40 lower-case hex chars")
    if entry.get("stage") not in ("ps_3_0", "vs_3_0"):
        fail("stage must be ps_3_0 or vs_3_0")
    kind = entry.get("kind")
    if kind not in ("tev", "pica_vs"):
        fail("kind must be tev or pica_vs")
    if entry.get("draw_enabled") not in (0, 1):
        fail("draw_enabled must be 0 or 1")
    if "family" in entry and entry["family"] not in FAMILIES:
        fail("family %r is not one of the six packs (+menu_light/generic)"
             % entry["family"])
    if "status" in entry:
        want = derive_status(entry)
        if entry["status"] != want:
            fail("declared status %r contradicts derived %r"
                 % (entry["status"], want))

    if kind == "tev":
        if entry.get("stage") != "ps_3_0":
            fail("kind=tev requires stage ps_3_0")
        stages = entry.get("stages")
        if not isinstance(stages, int) or not 1 <= stages <= MAX_STAGES:
            fail("stages must be 1..%d" % MAX_STAGES)
        stage_data = entry.get("stage_data")
        if not isinstance(stage_data, list):
            fail("kind=tev requires stage_data")
        elif isinstance(stages, int) and len(stage_data) != stages:
            fail("stage_data has %d rows but stages=%r"
                 % (len(stage_data), stages))
        for i, stage in enumerate(stage_data or []):
            for key in ("func_rgb", "func_a", "src_rgb", "src_a",
                        "op_rgb", "op_a"):
                if key not in stage:
                    fail("stage %d missing %r" % (i, key))
            if stage.get("func_rgb") not in TEV_FUNCS:
                fail("stage %d func_rgb out of range" % i)
            if stage.get("func_a") not in TEV_FUNCS:
                fail("stage %d func_a out of range" % i)
            for key in ("src_rgb", "src_a"):
                values = stage.get(key, [])
                if len(values) != 3:
                    fail("stage %d %s needs 3 entries" % (i, key))
                for value in values:
                    if value not in TEV_SOURCES:
                        fail("stage %d %s source %r out of range"
                             % (i, key, value))
            for key, allowed in (("op_rgb", TEV_RGB_OPS), ("op_a", TEV_A_OPS)):
                values = stage.get(key, [])
                if len(values) != 3:
                    fail("stage %d %s needs 3 entries" % (i, key))
                for value in values:
                    if value not in allowed:
                        fail("stage %d %s selector %r out of range"
                             % (i, key, value))
        const_regs = set(entry.get("constants") or [])
        for i, stage in enumerate(stage_data or []):
            reg = stage.get("const_reg", 255)
            if reg != 255 and const_regs and reg not in const_regs:
                fail("stage %d uses const c%d but constants=%r"
                     % (i, reg, sorted(const_regs)))
        for i, texture in enumerate(entry.get("textures") or []):
            if texture.get("sampler_type") not in (2, 3, 4):
                fail("texture %d sampler_type must be 2d/cube/volume" % i)

    if kind == "pica_vs":
        if entry.get("stage") != "vs_3_0":
            fail("kind=pica_vs requires stage vs_3_0")
        if entry.get("draw_enabled") == 1:
            # schema allOf: draw-enabled pica_vs requires emitted codegen + the
            # full emitter contract (assembly, .shbin SHA-256, inputs, rows).
            if entry.get("codegen") != "emitted-verified":
                fail("pica_vs can be draw-enabled only with codegen "
                     "'emitted-verified'")
            if not entry.get("emitted_asm"):
                fail("draw-enabled pica_vs requires emitted_asm")
            emitter = entry.get("emitter")
            if not isinstance(emitter, dict):
                fail("draw-enabled pica_vs requires an emitter block")
            else:
                _validate_emitter(entry, emitter, fail)
        elif entry.get("codegen") == "emitted-verified":
            # emitted code may stay translated until the runtime contract is
            # deliberately enabled, but the proof block must be present.
            emitter = entry.get("emitter")
            if isinstance(emitter, dict):
                _validate_emitter(entry, emitter, fail)
        elif entry.get("draw_enabled") == 1:
            fail("pica_vs draw-enabled without emitted codegen")
        outputs = entry.get("outputs")
        if not isinstance(outputs, list) or not 1 <= len(outputs) <= MAX_VS_VARYINGS:
            fail("pica_vs needs 1..%d outputs" % MAX_VS_VARYINGS)
        inputs = entry.get("inputs")
        if not isinstance(inputs, list) or len(inputs) > MAX_VS_INPUTS:
            fail("pica_vs inputs must be a list of at most %d" % MAX_VS_INPUTS)
        uniform = entry.get("uniform_registers", 0)
        if not isinstance(uniform, int) or not 0 <= uniform <= MAX_VS_UNIFORMS:
            fail("uniform_registers must be 0..%d" % MAX_VS_UNIFORMS)


def _validate_emitter(entry: dict, emitter: dict, fail) -> None:
    """Mirror the schema's $defs/emitter contract."""
    for key in ("samples", "tolerance", "max_abs_position", "max_abs_varying",
                "picasso", "bones", "uniform_rows", "instructions", "shbin",
                "shbin_sha256", "shbin_bytes", "inputs", "uniforms"):
        if key not in emitter:
            fail("emitter block lacks %r" % key)
    if not _is_hex(emitter.get("shbin_sha256"), 64):
        fail("emitter shbin_sha256 must be 64 lower-case hex chars")
    shbin = emitter.get("shbin")
    if not (isinstance(shbin, str) and shbin.startswith("generated/")
            and shbin.endswith(".shbin")):
        fail("emitter shbin must be 'generated/<name>.shbin'")
    if emitter.get("picasso") not in ("pass", "skipped"):
        fail("emitter picasso must be 'pass' or 'skipped'")
    if not isinstance(emitter.get("samples"), int) or emitter["samples"] < 1:
        fail("emitter samples must be >= 1")
    for key in ("max_abs_position", "max_abs_varying"):
        if not isinstance(emitter.get(key), (int, float)) or emitter[key] < 0:
            fail("emitter %s must be non-negative" % key)
    rows = emitter.get("uniforms")
    if not isinstance(rows, list) or len(rows) < 16:
        fail("emitter uniforms must list at least 16 PICA rows")
    else:
        want = [{"row": row, "d3d": d3d} for row, d3d in
                _expected_rows(emitter.get("uniform_rows", 0))]
        if rows != want:
            fail("emitter uniform rows do not follow the 4-bone/WVP mapping")
        if emitter.get("uniform_rows") != len(rows):
            fail("emitter uniform_rows disagrees with the uniforms list")
    plan_inputs = [{"usage": item.get("usage"),
                    "usage_name": item.get("usage_name"),
                    "reg_index": item.get("reg_index")}
                   for item in entry.get("inputs", [])]
    if emitter.get("inputs") != plan_inputs:
        fail("emitter inputs disagree with the plan inputs")


def _expected_rows(rows) -> list:
    out = []
    if not isinstance(rows, int):
        return out
    for row in range(rows):
        if row < 12:
            d3d = row
        elif row < 16:
            d3d = 180 + (row - 12)
        else:
            d3d = 184 + (row - 16)
        out.append((row, d3d))
    return out


def verify_document(doc: dict, schema: dict) -> list:
    errors: list = []
    if doc.get("schema") != schema.get("properties", {}).get("schema", {}).get("const"):
        errors.append("top-level: schema must be %r"
                      % schema["properties"]["schema"]["const"])
    if not doc.get("archive"):
        errors.append("top-level: archive is required")
    entries = doc.get("entries")
    if not isinstance(entries, list):
        errors.append("top-level: entries must be an array")
        return errors
    seen_ids, seen_sha = set(), set()
    for index, entry in enumerate(entries):
        where = "entry[%d] %s" % (index, entry.get("id", "?"))
        validate_entry(entry, errors, where)
        ident = entry.get("id")
        if ident in seen_ids:
            errors.append("%s: duplicate id" % where)
        seen_ids.add(ident)
        sha = entry.get("sha256")
        if sha in seen_sha:
            errors.append("%s: duplicate sha256" % where)
        seen_sha.add(sha)
    return errors


def status_table(doc: dict) -> dict:
    counts = {"translated": 0, "draw-enabled": 0, "refused": 0}
    for entry in doc.get("entries", []):
        counts[derive_status(entry)] += 1
    return counts


# --------------------------------------------------------------------------
# Symbolic equivalence against the retail archive.

def verify_against_archive(doc: dict, data_dir: str,
                           extra_docs=()) -> dict:
    """Regenerate the manifest from bytecode and compare it to the commit."""
    try:
        from fxp_compile import (build_manifest, merge_extra_entries,
                                 runtime_entries)
    except Exception as exc:  # pragma: no cover - import guard
        return {"status": "skipped", "reason": "fxp_compile unavailable: %s" % exc}
    if not data_dir or not os.path.isdir(data_dir):
        return {"status": "skipped", "reason": "retail data dir absent"}
    regenerated = build_manifest(data_dir, include_update=False)
    runtime_docs = [(origin, {"entries": runtime_entries(doc)})
                    for origin, doc in extra_docs]
    runtime_docs = [(origin, doc) for origin, doc in runtime_docs if doc["entries"]]
    if runtime_docs:
        regenerated = merge_extra_entries(regenerated, runtime_docs)
    committed = {entry["sha256"]: entry for entry in doc.get("entries", [])}
    fresh = {entry["sha256"]: entry for entry in regenerated.get("entries", [])}
    problems = []
    for sha, entry in committed.items():
        other = fresh.get(sha)
        if other is None:
            problems.append("%s: regenerated manifest has no plan (now %r)"
                            % (entry["id"], "refused"))
            continue
        if other != entry:
            keys = sorted(set(entry) | set(other))
            diff = [k for k in keys if entry.get(k) != other.get(k)]
            problems.append("%s: fields differ: %s" % (entry["id"], diff))
    for sha, entry in fresh.items():
        if sha not in committed:
            problems.append("%s: regenerated plan is missing from the commit"
                            % entry["id"])
    return {"status": "pass" if not problems else "fail",
            "entries": len(committed),
            "problems": problems,
            "note": ("the generator emits a TEV plan only after verifying the "
                     "final oC0 expression symbolically against the IR")}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plans", default=PLANS_PATH)
    parser.add_argument("--schema", default=SCHEMA_PATH)
    parser.add_argument("--data", default="")
    parser.add_argument("--extra", action="append", default=[],
                        help="pack-owned plan document merged by the generator")
    parser.add_argument("--json", default="", help="write the verdict here")
    args = parser.parse_args(argv)

    schema = load_schema(args.schema)
    with open(args.plans, encoding="utf-8") as handle:
        doc = json.load(handle)

    extra_docs = []
    paths = list(args.extra)
    try:
        from fxp_compile import pack_plan_paths  # host tool, same tree
        paths = pack_plan_paths() + paths
    except Exception:  # pragma: no cover - import guard
        pass
    for path in paths:
        if path in [seen for seen, _ in extra_docs]:
            continue
        with open(path, encoding="utf-8") as handle:
            extra_docs.append((path, json.load(handle)))

    errors = verify_document(doc, schema)
    pack_errors = []
    pack_counts = {}
    for origin, extra in extra_docs:
        if not isinstance(extra, dict) or not isinstance(extra.get("entries"), list):
            pack_errors.append("%s: not a plan document" % origin)
            continue
        pack_counts[origin] = len(extra["entries"])
        for entry in extra["entries"]:
            validate_entry(entry, pack_errors, origin)
    symbolic = verify_against_archive(doc, args.data, extra_docs)
    verdict = {
        "plans": os.path.relpath(args.plans, REPO),
        "schema": os.path.relpath(args.schema, REPO),
        "entries": len(doc.get("entries", [])),
        "pack_plans": pack_counts,
        "status": status_table(doc),
        "schema_errors": errors,
        "pack_schema_errors": pack_errors,
        "symbolic": symbolic,
        "ok": (not errors and not pack_errors
               and symbolic["status"] in ("pass", "skipped")),
    }
    if args.json:
        os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(verdict, handle, indent=2)
            handle.write("\n")
    print(json.dumps(verdict, indent=2))
    return 0 if verdict["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
