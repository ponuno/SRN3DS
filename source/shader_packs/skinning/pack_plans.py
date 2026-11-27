#!/usr/bin/env python3
"""Skinning pack plan catalog: generate and verify (host only).

The shared runtime table (``docs/shader_plans.json`` /
``source/d3d9_shader_plan_table.inc``) is capped at
``D9_SHADER_PLAN_MAX_ENTRIES == 16`` and is reserved for draw-certified
descriptors.  The skinning pack therefore keeps its full translation catalog
here, in its own directory, and never rewrites another pack's rows.

Commands::

    python source\\shader_packs\\skinning\\pack_plans.py generate ^
        --data F:\\Skyrim\\Data
    python source\\shader_packs\\skinning\\pack_plans.py verify
    python source\\shader_packs\\skinning\\pack_plans.py verify ^
        --data F:\\Skyrim\\Data

``generate`` re-derives every plan from the retail bytecode with the shared
planner (``tools/shaders/compile.py``).  ``verify`` is three layers:

1. **Coverage** — plans and refusals partition the pack manifest exactly:
   every hash is in exactly one of them, every plan is a manifest hash, and
   the refusal class agrees with the census class in the manifest.
2. **Schema/semantics** — each entry is validated with the same
   ``plan_verify.validate_entry`` the global manifest uses.  PICA VS limits
   (<=7 outputs, <=16 inputs, <=96 uniforms) and the "no PICA VS draw without
   codegen" rule are enforced; a draw-enabled entry must carry a material
   contract (non-empty ``preconditions``).
3. **Symbolic** — with ``--data`` every plan is regenerated from the archive
   and compared field for field.  No plan is emitted for a hash whose IR does
   not reproduce the committed descriptor.

No game bytes are written.  Nothing here changes ``GetDeviceCaps`` or enables
a draw.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for path in (REPO, os.path.join(REPO, "tools"),
             os.path.join(REPO, "tools", "shaders")):
    if path not in sys.path:
        sys.path.insert(0, path)

from shaders import asm, compile as C, plan_verify  # noqa: E402

PACK = "skinning"
ARCHIVE = "Skyrim - Shaders.bsa"
MANIFEST = os.path.join(HERE, "manifest.json")
PLANS = os.path.join(HERE, "plans.json")
REFUSALS = os.path.join(HERE, "refusals.json")
EMITTER_REPORT = os.path.join(HERE, "emitter_report.json")
DEFAULT_DATA = [r"F:\Skyrim\Data",
                r"E:\SteamLibrary\steamapps\common\Skyrim\Data"]
MAX_ENTRIES = 16


def _load(path: str):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def _dump(path: str, doc) -> None:
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(doc, handle, indent=1, sort_keys=True)
        handle.write("\n")


def find_data(explicit: str = "") -> str:
    if explicit:
        return explicit
    for candidate in DEFAULT_DATA:
        if os.path.exists(os.path.join(candidate, ARCHIVE)):
            return candidate
    raise SystemExit("Shaders.bsa not found; pass --data")


def _pack_hashes() -> dict:
    manifest = _load(MANIFEST)
    return {row[0]: {"kind": row[1], "class": row[2], "chunks": row[3]}
            for row in manifest["hashes"]}


def _def_reads(ir) -> dict:
    read = {}
    for ins in ir.body:
        for src in ins.srcs:
            if src.reg_type == asm.REG_CONST and src.index in ir.defs:
                read.setdefault(src.index, set()).update(
                    "xyzw".index(ch) for ch in src.swizzle)
    return read


def _offending_defs(ir) -> list:
    read = _def_reads(ir)
    out = []
    for reg in sorted(ir.defs):
        vals = ir.defs[reg]
        for comp in sorted(read.get(reg, ())):
            value = vals[comp]
            if not (value == value) or value < 0.0 or value > 1.0:
                out.append("c%d.%s = %g" % (reg, "xyzw"[comp], value))
    return out


def _emitter_verified() -> dict:
    if not os.path.exists(EMITTER_REPORT):
        return {}
    report = _load(EMITTER_REPORT)
    return {entry["sha256"]: entry for entry in report.get("entries", [])
            if entry.get("status") == "pass"}


def _plan_entry(ir, plan: dict, package: str, key: str) -> dict:
    entry = dict(plan)
    entry["id"] = "skin.%s.%s" % ("vs" if plan["stage"] == "vs_3_0" else "ps",
                                  plan["sha256"][:8])
    entry["family"] = PACK
    entry["draw_enabled"] = 0
    entry["source_package"] = package
    entry["source_key"] = key
    if plan["kind"] == "pica_vs":
        entry["codegen"] = "pending"
        entry["note"] = (
            "PICA VS contract: %d varying(s), %d uniform register(s); "
            "native codegen pending, draw disabled"
            % (len(plan["outputs"]), plan["uniform_registers"]))
        entry["preconditions"] = [
            "blend_weight_blend_index_inputs",
            "exact_hash_descriptor_match",
            "pica_vs_codegen",
        ]
        emitted = _emitter_verified().get(plan["sha256"])
        if emitted is not None:
            entry["codegen"] = "emitted-verified"
            entry["emitted_asm"] = "generated/%s.v.pica" % entry["id"]
            entry["emitter"] = {
                "samples": emitted["samples"],
                "tolerance": emitted["tolerance"],
                "max_abs_position": emitted["max_abs_position"],
                "max_abs_varying": emitted["max_abs_varying"],
                "picasso": emitted["picasso"],
                "bones": emitted["bones"],
                "uniform_rows": emitted["uniform_rows"],
            }
            entry["note"] = (
                "PICA VS emitted and numerically verified against the vs_3_0 "
                "IR (%d samples, max |dpos| %.1e, picasso pass); draw still "
                "blocked by the shared pica_vs draw rule and upload path"
                % (emitted["samples"], emitted["max_abs_position"]))
            entry["preconditions"] = [
                "blend_weight_blend_index_inputs",
                "bone_palette_window_%d" % emitted["bones"],
                "exact_hash_descriptor_match",
                "emitted_code_upload_path",
            ]
    else:
        entry["note"] = (
            "TEV plan reproduced offline; draw needs the material contract "
            "(texture and constant registers) at the draw site")
        entry["preconditions"] = ["material_constants", "texture0_2d"]
    return entry


def _refusal(sha: str, row: dict, why: str, ir) -> dict:
    reason, _, detail = why.partition(": ")
    out = {"sha256": sha, "kind": row["kind"], "class": row["class"],
           "chunks": row["chunks"], "reason": reason, "detail": detail}
    if reason == "def_literal_out_of_range":
        out["detail"] = "%s; literal rewrite (subtract formulation) needed" % (
            ", ".join(_offending_defs(ir)) or detail)
        out["needs"] = "shared literal-range rewrite in the planner"
    elif reason == "unsupported_opcode":
        out["needs"] = "shared opcode lowering (cmp/if/loop/rcp/derivatives)"
    elif reason.startswith("too_many_vs_varyings"):
        out["needs"] = "varying packing or a multi-pass VS"
    elif reason == "pica_vs_output_envelope":
        out["needs"] = ("fit the 7-output PICA VS envelope: drop/pack one "
                        "varying or use a multi-pass VS")
    elif reason == "no_output_write":
        out["needs"] = "shader writes no colour output"
    else:
        out["needs"] = "shared planner work"
    return out


def generate(data_dir: str) -> dict:
    from fxp_compile import _load_packages

    hashes = _pack_hashes()
    pkgs = _load_packages(find_data(data_dir))
    irs, sources = {}, {}
    for package in pkgs:
        for chunk in package.chunks:
            ir = C.build_ir(chunk.code)
            if ir.sha256 in hashes and ir.sha256 not in irs:
                irs[ir.sha256] = ir
                sources[ir.sha256] = (package.path, "0x%08x" % chunk.meta)

    entries, refusals = [], []
    for sha in sorted(hashes):
        row = hashes[sha]
        ir = irs.get(sha)
        if ir is None:
            raise SystemExit("hash %s is in the pack but not in the archive" % sha)
        if ir.kind == "ps" and row["kind"] != "ps":
            raise SystemExit("%s: pack says ps, IR says %s" % (sha, ir.kind))
        plan, _cls, why = (C.plan_ps(ir) if ir.kind == "ps"
                           else C.plan_vs(ir))
        # plan_vs counts only the usage != 0 varyings against PICA's 7; the
        # d9-shader-plan-v1 schema counts every output, including position.
        # Keep the catalog schema-clean: refuse the overflow by name.
        if plan is not None and plan["kind"] == "pica_vs" \
                and len(plan["outputs"]) > 7:
            why = ("pica_vs_output_envelope: %d outputs > 7 (position counts; "
                   "drop/pack a varying or use a multi-pass VS)"
                   % len(plan["outputs"]))
        if plan is not None and (plan["kind"] != "pica_vs"
                                 or len(plan["outputs"]) <= 7):
            entries.append(_plan_entry(ir, plan, *sources[sha]))
        else:
            if not why:
                why = "refused"
            refusals.append(_refusal(sha, row, why, ir))

    coverage = {"translated": len(entries),
                "draw_enabled": sum(1 for e in entries
                                    if e["draw_enabled"] == 1),
                "refused": len(refusals)}
    if coverage["translated"] + coverage["refused"] != len(hashes):
        raise SystemExit("catalog does not partition the pack manifest")
    plans = {
        "schema": "shader-pack-plan-catalog/1",
        "pack": PACK,
        "archive": ARCHIVE,
        "generated_for": find_data(data_dir),
        "planner": "tools/shaders/compile.py plan_ps/plan_vs",
        "status_rule": (
            "A pack plan is a d9-shader-plan-v1 entry. draw_enabled stays 0: "
            "a pica_vs descriptor is never draw-enabled without native codegen, "
            "and a TEV entry is draw-enabled only with a material contract."),
        "coverage": coverage,
        "entries": entries,
    }
    return {"plans": plans,
            "refusals": {"schema": "shader-pack-refusals/1", "pack": PACK,
                         "coverage": coverage, "refusals": refusals}}


def load_catalog() -> dict:
    """Merge the committed plan catalog and the refusal table."""
    plans = _load(PLANS)
    refusals = _load(REFUSALS)
    return {"schema": plans["schema"], "pack": plans["pack"],
            "archive": plans["archive"], "generated_for": plans.get("generated_for"),
            "planner": plans.get("planner"), "coverage": plans["coverage"],
            "entries": plans["entries"],
            "refusals": refusals["refusals"]}


def verify_document(doc: dict, pack_hashes: dict) -> dict:
    errors = []
    schema = plan_verify.load_schema()
    entries = doc.get("entries", [])
    refusals = doc.get("refusals", [])
    plan_shas = [e.get("sha256") for e in entries]
    refusal_shas = [r.get("sha256") for r in refusals]
    if len(set(plan_shas)) != len(plan_shas):
        errors.append("duplicate plan sha256")
    plan_ids = [e.get("id") for e in entries]
    if len(set(plan_ids)) != len(plan_ids):
        errors.append("duplicate plan id")
    if len(set(refusal_shas)) != len(refusal_shas):
        errors.append("duplicate refusal sha256")
    overlap = set(plan_shas) & set(refusal_shas)
    if overlap:
        errors.append("hashes both planned and refused: %s" % sorted(overlap)[:3])
    missing = set(pack_hashes) - set(plan_shas) - set(refusal_shas)
    if missing:
        errors.append("%d pack hashes have neither a plan nor a refusal"
                      % len(missing))
    foreign = (set(plan_shas) | set(refusal_shas)) - set(pack_hashes)
    if foreign:
        errors.append("%d catalog hashes are not in the pack manifest"
                      % len(foreign))

    for entry in entries:
        where = "plan %s" % entry.get("id", "?")
        plan_verify.validate_entry(entry, errors, where)
        row = pack_hashes.get(entry.get("sha256"))
        if row is None:
            continue
        if row["kind"] == "ps" and entry.get("stage") != "ps_3_0":
            errors.append("%s: pack kind ps but stage %r" % (where, entry.get("stage")))
        if row["kind"] == "vs" and entry.get("stage") != "vs_3_0":
            errors.append("%s: pack kind vs but stage %r" % (where, entry.get("stage")))
        if entry.get("kind") == "pica_vs":
            if entry.get("draw_enabled") != 0:
                errors.append("%s: PICA VS cannot be draw-enabled" % where)
            if len(entry.get("outputs", [])) > 7:
                errors.append("%s: more than 7 varyings" % where)
            if len(entry.get("inputs", [])) > 16:
                errors.append("%s: more than 16 inputs" % where)
            if entry.get("uniform_registers", 0) > 96:
                errors.append("%s: more than 96 uniform registers" % where)
        if entry.get("draw_enabled") == 1 and not entry.get("preconditions"):
            errors.append("%s: draw-enabled without a material contract" % where)
        if entry.get("codegen") == "emitted-verified":
            asm = entry.get("emitted_asm")
            if not asm or not os.path.exists(os.path.join(HERE, asm)):
                errors.append("%s: emitted-verified without an asm file" % where)
            elif entry["sha256"] not in _emitter_verified():
                errors.append("%s: emitted-verified but the report has no pass"
                              % where)

    for refusal in refusals:
        where = "refusal %s" % str(refusal.get("sha256"))[:12]
        row = pack_hashes.get(refusal.get("sha256"))
        if row is None:
            continue
        if not refusal.get("reason"):
            errors.append("%s: unnamed refusal" % where)
        if refusal.get("class") != row["class"]:
            errors.append("%s: class %r disagrees with manifest %r"
                          % (where, refusal.get("class"), row["class"]))
        if not refusal.get("needs"):
            errors.append("%s: refusal does not name the blocking lever" % where)

    coverage = doc.get("coverage", {})
    derived = {"translated": len(entries),
               "draw_enabled": sum(1 for e in entries
                                   if e.get("draw_enabled") == 1),
               "refused": len(refusals)}
    if coverage != derived:
        errors.append("coverage %r disagrees with the catalog %r"
                      % (coverage, derived))
    return {"errors": errors, "coverage": derived,
            "entries": len(entries), "refusals": len(refusals)}


def verify_symbolic(doc: dict, data_dir: str) -> dict:
    from fxp_compile import _load_packages

    wanted = {e["sha256"]: e for e in doc.get("entries", [])}
    found = {}
    for package in _load_packages(find_data(data_dir)):
        for chunk in package.chunks:
            ir = C.build_ir(chunk.code)
            if ir.sha256 in wanted and ir.sha256 not in found:
                plan, _cls, why = (C.plan_ps(ir) if ir.kind == "ps"
                                   else C.plan_vs(ir))
                found[ir.sha256] = (plan, why, ir)
    problems = []
    for sha, committed in wanted.items():
        plan, why, ir = found.get(sha, (None, "hash not in the archive", None))
        if plan is None:
            problems.append("%s: regenerated as refused (%s)"
                            % (committed.get("id", sha[:12]), why))
            continue
        fresh = _plan_entry(ir, plan, committed["source_package"],
                            committed["source_key"])
        keys = sorted(set(committed) | set(fresh))
        diff = [k for k in keys if committed.get(k) != fresh.get(k)]
        if diff:
            problems.append("%s: fields differ: %s"
                            % (committed.get("id", sha[:12]), diff))
    return {"status": "pass" if not problems else "fail",
            "checked": len(wanted), "problems": problems}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser("generate")
    gen.add_argument("--data", default="")
    ver = sub.add_parser("verify")
    ver.add_argument("--data", default="")
    ver.add_argument("--json", default="")
    args = parser.parse_args(argv)

    if args.cmd == "generate":
        built = generate(args.data)
        _dump(PLANS, built["plans"])
        _dump(REFUSALS, built["refusals"])
        coverage = built["plans"]["coverage"]
        print("wrote %s and %s: %d translated (%d draw-enabled), %d refused"
              % (PLANS, REFUSALS, coverage["translated"],
                 coverage["draw_enabled"], coverage["refused"]))
        return 0

    doc = load_catalog()
    report = verify_document(doc, _pack_hashes())
    report["symbolic"] = (verify_symbolic(doc, args.data) if args.data
                          else {"status": "skipped"})
    report["ok"] = (not report["errors"]
                    and report["symbolic"]["status"] in ("pass", "skipped"))
    if args.json:
        _dump(args.json, report)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
