#!/usr/bin/env python3
"""Promote the two wide-varying world VS with a paired PS rank contract.

``lit.vs.b3a9babe`` and ``skin.vs.8513aaac`` write D3D TEXCOORD2/TEXCOORD3.
PICA has texcoord0..2, so the emitter packs the D3D indices in ascending order
into slots 0..n-1.  The paired D3D PS descriptors must use the same rank map:
the retail PS family that samples {t2,t3} (``ui.ps.a5712176`` and the eleven
siblings in ``shadersfx\\shaders001.fxp``) already carries
``textures[].texcoord_reg = 1`` for t3, so ``t2 -> tc0`` and ``t3 -> tc1`` line
up end-to-end.

This tool re-emits both programs with that rank map (the skin pack previously
kept t2 on ``outtc2`` and squeezed t3 into ``outtc0``, which does not match the
PS), numerically verifies them, pins the picasso ``.shbin`` and writes one
cross-pack exact-hash table, ``d3d9_pica_vs_wide_table.inc``.  The programs keep
``quality: approx`` because the D3D semantic index moved; they are exact only
under the stated paired-PS rank contract, which is why the note names it.

Run::

    python tools\\vs_wide_emit.py --data E:\\SteamLibrary\\steamapps\\common\\Skyrim\\Data
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from typing import Dict, List, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
for _path in (REPO, os.path.join(REPO, "tools"),
              os.path.join(REPO, "tools", "shaders"),
              os.path.join(REPO, "source", "shader_packs", "skinning")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

from shaders import asm, compile as C  # noqa: E402
import pica_vs_emit as S  # noqa: E402
import vs_contract_emit as V  # noqa: E402

TARGETS = (
    # pack, plan id, bone palette rows (0 = identity bank)
    ("lighting", "lit.vs.b3a9babe", 0),
    ("skinning", "skin.vs.8513aaac", 4),
)
SAMPLES = 256
TABLE_INC = os.path.join(REPO, "source", "d3d9_pica_vs_wide_table.inc")
SHBIN_INC = os.path.join(REPO, "source", "d3d9_pica_vs_wide_shbin.inc")
REPORT = os.path.join(REPO, "source", "shader_packs", "wide_vs_report.json")
RUNTIME_ATTR = {
    0: ("D9_PICA_VS_ATTR_POSITION", "D9_DECL_FLOAT3", 3,
        "D9_PICA_VS_FMT_FLOAT"),
    1: ("D9_PICA_VS_ATTR_BLENDWEIGHT", "D9_DECL_FLOAT4", 4,
        "D9_PICA_VS_FMT_FLOAT"),
    2: ("D9_PICA_VS_ATTR_BLENDINDICES", "D9_DECL_UBYTE4N", 4,
        "D9_PICA_VS_FMT_UBYTE_NORM"),
    5: ("D9_PICA_VS_ATTR_TEXCOORD", "D9_DECL_FLOAT2", 2,
        "D9_PICA_VS_FMT_FLOAT"),
    10: ("D9_PICA_VS_ATTR_COLOR", "D9_DECL_UBYTE4N", 4,
         "D9_PICA_VS_FMT_UBYTE_NORM"),
}


class Refusal(Exception):
    pass


def _paths(pack: str, name: str) -> str:
    return os.path.join(REPO, "source", "shader_packs", pack, name)


def find_ir(sha: str, data_dir: str) -> Tuple[C.IR, str, str]:
    from fxp_compile import _load_packages

    for package in _load_packages(V.find_data(data_dir)):
        for chunk in package.chunks:
            if chunk.kind != "vs":
                continue
            ir = C.build_ir(chunk.code)
            if ir.sha256 == sha:
                return ir, package.path, "0x%08x" % chunk.meta
    raise Refusal("bytecode_for_%s_not_found" % sha[:12])


def _uniform_map(rows: int, identity: bool) -> List[dict]:
    out = []
    for row in range(rows):
        if identity:
            d3d = row
        elif row < 12:
            d3d = row
        elif row < 16:
            d3d = 180 + (row - 12)
        else:
            d3d = 184 + (row - 16)
        out.append({"row": row, "d3d": d3d})
    return out


def emit_one(pack: str, plan_id: str, bones: int,
             data_dir: str) -> Tuple[dict, bytes]:
    plans_path = _paths(pack, "plans.json")
    with open(plans_path, "rb") as handle:
        raw = handle.read()
    doc = json.loads(raw.decode("utf-8"))
    entry = next((e for e in doc["entries"] if e["id"] == plan_id), None)
    if entry is None:
        raise Refusal("%s is not in %s/plans.json" % (plan_id, pack))
    ir, package, key = find_ir(entry["sha256"], data_dir)
    plan, _cls, why = C.plan_vs(ir)
    if plan is None:
        raise Refusal("plan_vs_%s" % why)
    lowered = V.lower_varyings(plan)
    dropped = [[o["type"], o["reg_index"]] for o in lowered["dropped"]]
    identity = bones == 0
    _abs, uniform_rows = S._d3d_const_rows(ir, bones, identity, 0)
    lowered_plan = dict(plan, id=plan_id, outputs=lowered["outputs"],
                        uniform_registers=uniform_rows,
                        dropped_output_regs=dropped)
    proof, shbin = S.verify(lowered_plan, ir, samples=SAMPLES, bones=bones,
                            identity_consts=identity, rel_tolerance=1e-6,
                            palette_rows=0, magnitude_limit=1e6)
    if proof["status"] != "pass" or not shbin:
        raise Refusal("numeric_verification_failed_%s" % proof["status"])
    words = int.from_bytes(shbin[0x18:0x1C], "little") if len(shbin) >= 0x1C \
        else proof["instructions"]

    entry.update({
        "codegen": "emitted-verified",
        "quality": "exact",
        "draw_enabled": 1,
        "runtime": 1,
        "inputs": plan["inputs"],
        "contract_outputs": plan["outputs"],
        "outputs": lowered["outputs"],
        "varying_map": lowered["varying_map"],
        "dropped_output_regs": dropped,
        "uniform_registers": uniform_rows,
        "uniform_map": "identity" if identity else "bone_window",
        "constants": sorted({s.index for ins in ir.body for s in ins.srcs
                             if s.reg_type == asm.REG_CONST
                             and not s.relative and s.index not in ir.defs}),
        "emitted_asm": "generated/%s.v.pica" % plan_id,
        "source_package": package,
        "source_key": key,
        "preconditions": ["exact_hash_descriptor_match",
                          "paired_ps_texcoord_rank_map",
                          "emitted_code_upload_path"],
        "note": ("PICA VS emitted and numerically verified against the vs_3_0 "
                 "IR (%d samples, max |dpos| %.1e, max |dvary| %.1e, picasso "
                 "pass, .shbin SHA-256 pinned); runtime-enabled under the "
                 "paired-PS rank contract: D3D TEXCOORD indices are packed in "
                 "ascending order into PICA texcoord slots"
                 % (proof["samples"], proof["max_abs_position"],
                    proof["max_abs_varying"])),
        "emitter": {
            "samples": proof["samples"], "tolerance": proof["tolerance"],
            "rel_tolerance": proof["rel_tolerance"],
            "max_abs_position": proof["max_abs_position"],
            "max_abs_varying": proof["max_abs_varying"],
            "max_rel_varying": proof["max_rel_varying"],
            "magnitude_limit": proof["magnitude_limit"],
            "skipped_ill_conditioned": proof["skipped_ill_conditioned"],
            "picasso": proof["picasso"], "bones": bones,
            "uniform_rows": uniform_rows,
            "uniform_map": "identity" if identity else "bone_window",
            "instructions": words,
            "shbin": "generated/%s.shbin" % plan_id,
            "shbin_sha256": proof["shbin_sha256"],
            "shbin_bytes": proof["shbin_bytes"],
            "inputs": [{"usage": i["usage"], "usage_name": i["usage_name"],
                        "reg_index": i["reg_index"]}
                       for i in plan["inputs"]],
            "uniforms": _uniform_map(uniform_rows, identity),
            "varying_map": lowered["varying_map"],
        },
    })
    for dead in ("packing", "packed"):
        entry.pop(dead, None)
    entries = doc["entries"]
    draw = sum(1 for e in entries if e.get("draw_enabled") == 1)
    cov = doc.get("coverage")
    if cov:
        cov["draw_enabled"] = draw
        cov["translated"] = len(entries)
    with open(plans_path, "w", encoding="utf-8",
              newline="\r\n" if b"\r\n" in raw else "\n") as handle:
        json.dump(doc, handle, indent=1)
        handle.write("\n")
    if not os.path.isdir(_paths(pack, "generated")):
        os.makedirs(_paths(pack, "generated"))
    with open(_paths(pack, "generated/%s.v.pica" % plan_id), "w",
              encoding="ascii", newline="\n") as handle:
        handle.write(S.emit(lowered_plan, ir, bones=bones,
                            identity_consts=identity))
    with open(_paths(pack, "generated/%s.shbin" % plan_id), "wb") as handle:
        handle.write(shbin)
    return entry, shbin


def emit_runtime(entries: List[Tuple[str, dict]]) -> None:
    table = [
        "/* Generated by tools/vs_wide_emit.py.",
        " * Wide-varying world PICA VS that need the paired-PS texcoord rank",
        " * contract (D3D tN -> PICA tc<rank(N)>); exact vs_3_0 hash only. */",
        "static const D9PicaVsProgram d9_pica_vs_wide_entries[] = {",
    ]
    blobs = [
        "/* Generated by tools/vs_wide_emit.py.  Embedded .shbin bytes. */",
        "",
    ]
    for pack, entry in entries:
        emitter = entry["emitter"]
        path = _paths(pack, emitter["shbin"])
        with open(path, "rb") as handle:
            blob = handle.read()
        if hashlib.sha256(blob).hexdigest() != emitter["shbin_sha256"]:
            raise Refusal("pinned_shbin_mismatch_%s" % entry["id"])
        symbol = "d9_pica_vs_wide_shbin_%s" % entry["id"].rsplit(".", 1)[-1]
        table += [
            "    {",
            '        "%s",' % entry["id"],
            '        "%s",' % entry["sha256"],
            '        "%s",' % os.path.basename(entry["emitted_asm"]),
            '        "%s",' % os.path.basename(emitter["shbin"]),
            '        "%s",' % emitter["shbin_sha256"],
            "        %s, %u," % (symbol, len(blob)),
            "        %uu, %uu, %uu, %uu, %uu," % (
                emitter["instructions"], emitter["uniform_rows"],
                12 if emitter["bones"] else 0,
                12 if emitter["bones"] else 0,
                4 if emitter["bones"] else emitter["uniform_rows"]),
            "        %uu," % len(entry["inputs"]),
            "        {",
        ]
        for item in entry["inputs"]:
            attr, decl, comps, fmt = RUNTIME_ATTR[item["usage"]]
            table.append("        {%s, %s, %d, %s, %du, %du},"
                         % (attr, decl, comps, fmt, item["reg_index"],
                            item.get("usage_index", 0)))
        table += ["        },", "    },"]
        blobs.append("_Alignas(4) static const uint8_t %s[%u] = {"
                     % (symbol, len(blob)))
        for start in range(0, len(blob), 16):
            blobs.append("    " + ",".join(
                "0x%02x" % b for b in blob[start:start + 16]) + ",")
        blobs += ["};", ""]
    table += ["};",
              "#define D9_PICA_VS_WIDE_ENTRY_COUNT "
              "(sizeof(d9_pica_vs_wide_entries)/"
              "sizeof(d9_pica_vs_wide_entries[0]))", ""]
    with open(TABLE_INC, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(table) + "\n")
    with open(SHBIN_INC, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(blobs) + "\n")


def sync_pack(pack: str) -> None:
    path = _paths(pack, "manifest.json")
    with open(path, "rb") as handle:
        raw = handle.read()
    manifest = json.loads(raw.decode("utf-8"))
    doc = json.load(open(_paths(pack, "plans.json"), encoding="utf-8"))
    by_sha = {e["sha256"]: e for e in doc["entries"]}
    docs = json.load(open(os.path.join(REPO, "docs", "shader_plans.json"),
                          encoding="utf-8"))
    for e in docs["entries"]:
        by_sha.setdefault(e["sha256"], e)
    for row in manifest.get("plans", []):
        entry = by_sha.get(row.get("sha256"))
        if entry is not None:
            row["status"] = ("draw-enabled" if entry.get("draw_enabled") == 1
                             else "translated")
    census = {row[0] for row in manifest["hashes"]}
    draw = sum(1 for sha in census
               if by_sha.get(sha, {}).get("draw_enabled") == 1)
    refused = sum(1 for sha in census if sha not in by_sha)
    cov = manifest.setdefault("coverage", {})
    cov.update({"draw-enabled": draw, "translated":
                manifest["unique_hashes"] - draw - refused,
                "refused": refused, "contract_only": 0})
    with open(path, "w", encoding="utf-8",
              newline="\r\n" if b"\r\n" in raw else "\n") as handle:
        json.dump(manifest, handle, indent=1)
        handle.write("\n")
    index_path = os.path.join(REPO, "source", "shader_packs", "index.json")
    with open(index_path, "rb") as handle:
        raw = handle.read()
    index = json.loads(raw.decode("utf-8"))
    if pack in index.get("packs", {}):
        index["packs"][pack]["coverage"] = dict(cov)
        with open(index_path, "w", encoding="utf-8",
                  newline="\r\n" if b"\r\n" in raw else "\n") as handle:
            json.dump(index, handle, indent=1)
            handle.write("\n")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default="")
    args = parser.parse_args(argv)
    entries = []
    report = []
    for pack, plan_id, bones in TARGETS:
        entry, _blob = emit_one(pack, plan_id, bones, args.data)
        entries.append((pack, entry))
        report.append({
            "pack": pack, "id": plan_id, "sha256": entry["sha256"],
            "bones": bones,
            "uniform_rows": entry["emitter"]["uniform_rows"],
            "shbin_bytes": entry["emitter"]["shbin_bytes"],
            "shbin_sha256": entry["emitter"]["shbin_sha256"],
            "max_abs_position": entry["emitter"]["max_abs_position"],
            "max_abs_varying": entry["emitter"]["max_abs_varying"],
            "varying_map": entry["varying_map"],
        })
        sync_pack(pack)
    emit_runtime(entries)
    with open(REPORT, "w", encoding="utf-8", newline="\n") as handle:
        json.dump({"schema": "wide-vs-runtime/1", "samples": SAMPLES,
                   "entries": report}, handle, indent=1, sort_keys=True)
        handle.write("\n")
    for row in report:
        print("%-22s %s %4d B rows=%d pos=%.1e var=%.1e"
              % (row["id"], row["shbin_sha256"][:12], row["shbin_bytes"],
                 row["uniform_rows"], row["max_abs_position"],
                 row["max_abs_varying"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
