#!/usr/bin/env python3
"""BSA `ui/interface` plan catalogue: generate, emit and verify (host only).

The pack catalog (``plans.json``) already carries the ten StartMenu/GFx
prefix groups measured from ``TESV.exe``.  This tool adds the **113 BSA UI
hashes** from ``manifest.json``:

* 40 VS -> ``pica_vs`` contract from the canonical vs_3_0 IR.  Programs that
  fit the physical output envelope (position, one colour, texcoord0..2) and
  the straight-line PICA VS opcode set are emitted to picasso and verified
  numerically; those become ``emitted-verified`` with ``runtime: 1``.
* 73 PS -> TEV descriptor from ``bsa_ps.plan_ps`` (exact, derived-constant,
  or a declared approximation with preconditions).  Every PS entry carries a
  complete material contract (textures, constant values, varyings), so it is
  draw-enabled for its declared spec; approximations are named, never silent.

Commands::

    python source\\shader_packs\\ui\\bsa_pack_plans.py generate --data F:\\Skyrim\\Data
    python source\\shader_packs\\ui\\bsa_pack_plans.py verify   --data F:\\Skyrim\\Data
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for _p in (HERE, REPO, os.path.join(REPO, "tools"),
           os.path.join(REPO, "tools", "shaders")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from shaders import asm  # noqa: E402
from shaders import compile as C  # noqa: E402
from shaders import plan_verify  # noqa: E402
import bsa_ps  # noqa: E402
import bsa_vs_emit as V  # noqa: E402

PACK = "ui"
ARCHIVE = "Skyrim - Shaders.bsa"
MANIFEST = os.path.join(HERE, "manifest.json")
PLANS = os.path.join(HERE, "plans.json")
REPORT = os.path.join(HERE, "bsa_vs_report.json")
GENERATED = os.path.join(HERE, "generated")
DEFAULT_DATA = [r"F:\Skyrim\Data",
                r"E:\SteamLibrary\steamapps\common\Skyrim\Data"]


def _load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def _dump(path, doc):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(doc, handle, indent=1, sort_keys=True)
        handle.write("\n")


def find_data(explicit=""):
    if explicit:
        return explicit
    for candidate in DEFAULT_DATA:
        if os.path.exists(os.path.join(candidate, ARCHIVE)):
            return candidate
    raise SystemExit("Shaders.bsa not found; pass --data")


def _pack_hashes():
    manifest = _load(MANIFEST)
    return {row[0]: {"kind": row[1], "class": row[2], "chunks": row[3]}
            for row in manifest["hashes"]}


def _load_irs(data_dir):
    from fxp_compile import _load_packages
    irs, sources = {}, {}
    for package in _load_packages(find_data(data_dir)):
        for chunk in package.chunks:
            ir = C.build_ir(chunk.code)
            if ir.sha256 not in irs:
                irs[ir.sha256] = ir
                sources[ir.sha256] = (package.path, "0x%08x" % chunk.meta)
    return irs, sources


# ---------------------------------------------------------------------------
# Vertex shaders


def _uniform_rows(rows):
    return [{"row": row, "d3d": row} for row in range(rows)]


def _vs_entry(ir, plan, package, key, emit_result):
    entry = dict(plan)
    entry["id"] = "ui.vs.%s" % ir.sha256[:8]
    entry["family"] = PACK
    entry["source_package"] = package
    entry["source_key"] = key
    entry["quality"] = "exact"
    entry["draw_enabled"] = 0
    entry["runtime"] = 0
    entry["codegen"] = "pending"
    entry["note"] = (
        "PICA VS contract from the canonical vs_3_0 IR: %d output(s), %d "
        "uniform register(s); native codegen pending"
        % (len(plan["outputs"]), plan["uniform_registers"]))
    entry["preconditions"] = ["exact_hash_descriptor_match",
                              "pica_vs_codegen"]
    if any(item["usage"] == 10 and item["usage_index"] != 0
           for item in plan["outputs"]):
        entry["preconditions"].append("second_colour_as_primary")
    if emit_result is None:
        return entry
    report, shbin = emit_result
    entry["codegen"] = "emitted-verified"
    entry["draw_enabled"] = 1
    entry["runtime"] = 1
    entry["emitted_asm"] = "generated/%s.v.pica" % entry["id"]
    rows = report["uniform_rows"]
    entry["emitter"] = {
        "samples": report["samples"],
        "tolerance": report["tolerance"],
        "max_abs_position": report["max_abs_position"],
        "max_abs_varying": report["max_abs_varying"],
        "picasso": report["picasso"],
        "bones": 0,
        "uniform_rows": rows,
        "instructions": report["instructions"],
        "shbin": "generated/%s.shbin" % entry["id"],
        "shbin_sha256": report["shbin_sha256"],
        "shbin_bytes": report["shbin_bytes"],
        "inputs": [{"usage": item["usage"],
                    "usage_name": item["usage_name"],
                    "reg_index": item["reg_index"]}
                   for item in plan["inputs"]],
        "uniforms": _uniform_rows(rows),
    }
    entry["preconditions"] = ["exact_hash_descriptor_match",
                              "emitted_code_upload_path"]
    if any(item["usage"] == 10 and item["usage_index"] != 0
           for item in plan["outputs"]):
        entry["preconditions"].append("second_colour_as_primary")
    entry["note"] = (
        "PICA VS emitted and numerically verified against the vs_3_0 IR "
        "(%d samples, max |dpos| %.1e, max |dvary| %.1e, picasso pass, "
        ".shbin SHA-256 pinned); draw-enabled through the upload/bind "
        "contract only"
        % (report["samples"], report["max_abs_position"],
           report["max_abs_varying"]))
    return entry


def _emit_vs(plan, ir, entry_id):
    plan = dict(plan, id=entry_id)
    try:
        text = V.emit(plan, ir, bones=0)
    except V.Refusal as exc:
        return None, str(exc)
    try:
        report, shbin = V.verify(plan, ir, V.SAMPLES, bones=0)
    except V.Refusal as exc:
        return None, str(exc)
    if report["status"] != "pass" or not shbin:
        return None, "verify_failed:picasso=%s" % report["picasso"]
    os.makedirs(GENERATED, exist_ok=True)
    with open(os.path.join(GENERATED, "%s.v.pica" % entry_id), "w",
              encoding="ascii", newline="\n") as handle:
        handle.write(text)
    with open(os.path.join(GENERATED, "%s.shbin" % entry_id), "wb") as handle:
        handle.write(shbin)
    return (report, shbin), None


# ---------------------------------------------------------------------------
# Pixel shaders


def _ps_entry(ir, plan, package, key):
    entry = dict(plan)
    entry["id"] = "ui.ps.%s" % ir.sha256[:8]
    entry["family"] = PACK
    entry["source_package"] = package
    entry["source_key"] = key
    entry["sha1"] = ir.sha1
    entry["version"] = "0x%08x" % ir.version
    entry["stage"] = "ps_3_0"
    entry["kind"] = "tev"
    entry["draw_enabled"] = 1
    entry["runtime"] = 1
    contract = entry.get("material_contract") or {}
    if not contract.get("complete"):
        entry["draw_enabled"] = 0
        entry["runtime"] = 0
    if entry.get("quality") != "exact":
        entry["approximation"] = entry.get("approximation") or "declared"
        entry["note"] = (
            "Declared TEV approximation for the simplified spec (see "
            "'approximation'); complete material contract, so draw-enabled "
            "for that spec only. Not a 1:1 claim for the retail program.")
    else:
        entry["note"] = (
            "TEV descriptor reproduced from the canonical ps_3_0 IR with "
            "derived constants; complete material contract, draw-enabled "
            "for the declared spec.")
    return entry


# ---------------------------------------------------------------------------
# Generate / verify


def generate(data_dir=""):
    hashes = _pack_hashes()
    irs, sources = _load_irs(data_dir)

    entries, refusals, vs_report = [], [], []
    for sha in sorted(hashes):
        row, ir = hashes[sha], irs.get(sha)
        if ir is None:
            raise SystemExit("hash %s is in the pack but not the archive" % sha)
        package, key = sources[sha]
        if ir.kind == "vs":
            plan, _cls, why = C.plan_vs(ir)
            if plan is None:
                refusals.append({"sha256": sha, "kind": "vs",
                                 "class": row["class"], "reason": why,
                                 "needs": "pica_vs contract"})
                continue
            entry_id = "ui.vs.%s" % sha[:8]
            emitted, refusal = _emit_vs(plan, ir, entry_id)
            if emitted is not None:
                report, _shbin = emitted
                vs_report.append(dict(report, id=entry_id, sha256=sha,
                                      status="pass",
                                      refusal="", class_=row["class"]))
            else:
                vs_report.append({"id": entry_id, "sha256": sha,
                                  "status": "pending", "refusal": refusal,
                                  "class_": row["class"]})
            entries.append(_vs_entry(ir, plan, package, key, emitted))
        else:
            plan, why = bsa_ps.plan_ps(ir)
            if plan is None:
                refusals.append({"sha256": sha, "kind": "ps",
                                 "class": row["class"], "reason": why,
                                 "needs": "tev scheduler"})
                continue
            entries.append(_ps_entry(ir, plan, package, key))

    old = _load(PLANS) if os.path.exists(PLANS) else {}
    old_entries = [e for e in old.get("entries", [])
                   if e.get("sha256") not in hashes]
    merged = sorted(old_entries + entries, key=lambda e: e["id"])
    coverage = {
        "translated": len(entries),
        "draw_enabled": sum(1 for e in entries if e.get("draw_enabled") == 1),
        "refused": len(refusals),
        "vs": {
            "contracts": sum(1 for e in entries if e["kind"] == "pica_vs"),
            "emitted": sum(1 for e in entries
                           if e.get("codegen") == "emitted-verified"),
            "draw": sum(1 for e in entries
                        if e["kind"] == "pica_vs"
                        and e.get("draw_enabled") == 1),
        },
        "ps": {
            "translated": sum(1 for e in entries if e["kind"] == "tev"),
            "exact": sum(1 for e in entries if e["kind"] == "tev"
                         and e.get("quality") == "exact"),
            "approx": sum(1 for e in entries if e["kind"] == "tev"
                          and e.get("quality") == "approx"),
            "draw": sum(1 for e in entries if e["kind"] == "tev"
                        and e.get("draw_enabled") == 1),
        },
        "classes": dict(sorted(Counter(
            row["class"] for row in hashes.values()).items())),
    }
    doc = dict(old)
    doc["schema"] = "d9-shader-plan-v1"
    doc["archive"] = ARCHIVE
    doc["note"] = old.get("note", "") or (
        "Pack-owned UI plans: GFx menu blobs plus the BSA ui/interface "
        "catalogue.  Approximations are named and preconditions are explicit.")
    doc["bsa"] = {
        "chunks": sum(row["chunks"] for row in hashes.values()),
        "hashes": len(hashes),
        "coverage": coverage,
        "refusals": refusals,
    }
    doc["entries"] = merged
    _dump(PLANS, doc)
    _dump(REPORT, {"schema": "ui-vs-emitter-report/1", "pack": PACK,
                   "archive": ARCHIVE, "samples": V.SAMPLES,
                   "tolerance": V.TOLERANCE, "bones": 0,
                   "coverage": coverage["vs"], "entries": vs_report})
    return {"doc": doc, "coverage": coverage, "refusals": refusals,
            "vs_report": vs_report}


def verify(data_dir="", symbolic=False):
    errors = []
    pack_hashes = _pack_hashes()
    doc = _load(PLANS)
    entries = doc.get("entries", [])
    bsa = [e for e in entries if e.get("id", "").startswith("ui.")]
    ids = [e.get("id") for e in entries]
    if len(set(ids)) != len(ids):
        errors.append("duplicate plan id")
    shas = [e.get("sha256") for e in bsa]
    missing = set(pack_hashes) - set(shas)
    if missing:
        errors.append("%d BSA hashes have no plan" % len(missing))
    foreign = set(shas) - set(pack_hashes)
    if foreign:
        errors.append("%d BSA plans are not in the manifest" % len(foreign))
    report = _load(REPORT) if os.path.exists(REPORT) else {"entries": []}
    verified = {row["sha256"] for row in report.get("entries", [])
                if row.get("status") == "pass"
                and row.get("samples", 0) > 0 and row.get("picasso") == "pass"
                and row.get("shbin_bytes", 0) > 0}
    for entry in entries:
        if not entry.get("id", "").startswith("ui."):
            # The GFx StartMenu rows are owned by the earlier menu pass; this
            # tool neither validates nor rewrites them.
            continue
        where = "plan %s" % entry.get("id", "?")
        plan_verify.validate_entry(entry, errors, where)
        if not entry.get("quality") and entry.get("codegen") != "pending":
            errors.append("%s: missing quality" % where)
        if entry.get("quality") == "approx" and not entry.get("approximation"):
            errors.append("%s: approx without an approximation string" % where)
        if entry.get("kind") == "tev":
            sha = entry.get("sha256")
            if entry.get("draw_enabled") == 1:
                contract = entry.get("material_contract") or {}
                if not contract.get("complete"):
                    errors.append("%s: draw-enabled without a complete "
                                  "material contract" % where)
            if entry.get("id", "").startswith("ui.") and sha not in pack_hashes:
                errors.append("%s: not a BSA ui hash" % where)
        if entry.get("kind") == "pica_vs":
            if entry.get("draw_enabled") == 1 and entry.get("sha256") not in verified:
                errors.append("%s: draw-enabled without an emitter pass" % where)
            if entry.get("codegen") == "emitted-verified":
                emitter = entry.get("emitter") or {}
                path = os.path.join(HERE, emitter.get("shbin", ""))
                if not os.path.exists(path):
                    errors.append("%s: .shbin missing" % where)
                else:
                    with open(path, "rb") as handle:
                        digest = hashlib.sha256(handle.read()).hexdigest()
                    if digest != emitter.get("shbin_sha256"):
                        errors.append("%s: .shbin SHA-256 mismatch" % where)
                asm_path = os.path.join(HERE, entry.get("emitted_asm", ""))
                if not os.path.exists(asm_path):
                    errors.append("%s: emitted asm missing" % where)
    if symbolic:
        irs, _sources = _load_irs(data_dir)
        for entry in bsa:
            ir = irs.get(entry["sha256"])
            if ir is None:
                errors.append("%s: hash not in archive" % entry["id"])
                continue
            if ir.kind == "ps":
                plan, _why = bsa_ps.plan_ps(ir)
            else:
                plan = C.plan_vs(ir)[0]
            if plan is None:
                errors.append("%s: regenerated as refused" % entry["id"])
                continue
            fresh = _ps_entry(ir, plan, "", "") if ir.kind == "ps" else None
            if fresh is not None:
                for key in ("kind", "quality", "stages", "stage_data",
                            "textures", "constants", "constant_values",
                            "preconditions"):
                    if fresh.get(key) != entry.get(key):
                        errors.append("%s: regenerated field %r differs"
                                      % (entry["id"], key))
    coverage = {
        "translated": len(bsa),
        "draw_enabled": sum(1 for e in bsa if e.get("draw_enabled") == 1),
        "refused": len(pack_hashes) - len(bsa),
    }
    return {"ok": not errors, "errors": errors, "coverage": coverage,
            "entries": len(bsa),
            "emitted_verified": sum(1 for e in bsa
                                    if e.get("codegen") == "emitted-verified")}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("generate", "verify"):
        cmd = sub.add_parser(name)
        cmd.add_argument("--data", default="")
        if name == "verify":
            cmd.add_argument("--symbolic", action="store_true")
    args = parser.parse_args(argv)
    if args.cmd == "generate":
        built = generate(args.data)
        print(json.dumps(built["coverage"], indent=1, sort_keys=True))
        return 0
    report = verify(args.data, args.symbolic)
    print(json.dumps(report, indent=1, sort_keys=True))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
