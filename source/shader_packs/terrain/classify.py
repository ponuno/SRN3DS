#!/usr/bin/env python3
"""Build the terrain pack's append-only, IR-derived classification table.

This is a host-only report.  It reads the canonical MojoShader IR and the
read-only retail archive for CTAB names, but writes hashes/metadata only.  It
never emits or enables a shader plan: the measured terrain subset has no member
that fits the current six-stage / three-unit single-pass contract.

Usage::

    python source/shader_packs/terrain/classify.py --corpus E:\\host_sm3
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
import statistics
import sys
from pathlib import Path

PACK_DIR = Path(__file__).resolve().parent
REPO = PACK_DIR.parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

from shaders import asm, differential  # noqa: E402
from skyrim import bsa, shaders  # noqa: E402

SCHEMA = "terrain-shader-classes/1"
DIRECT_OPS = {
    "mov", "add", "sub", "mad", "mul", "dp3", "lrp", "min", "max",
    "texld", "texkill",
}
FLOW_OPS = {
    "loop", "endloop", "rep", "endrep", "if", "ifc", "else", "endif",
    "break", "breakc", "breakp", "call", "callnz", "ret", "label",
}
CLASS_ORDER = (
    "terrain.height_layers",
    "terrain.multitexture_12_13",
    "terrain.multitexture_14_15_lod",
    "terrain.lod_noise_overlay",
    "terrain.world_map_overlay",
    "terrain.lod_world_map_overlay",
)


def _load(path: Path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _number(value: float):
    if abs(value) < 1e-8:
        return 0
    return round(value, 6)


def _ctab_metadata(data_dir: str, wanted_paths: set[str], wanted_hashes: set[str]) -> dict:
    """Read CTAB vocabulary for pack hashes; no bytecode leaves the host."""
    archive_path = os.path.join(data_dir, "Skyrim - Shaders.bsa")
    if not os.path.exists(archive_path):
        raise SystemExit("retail archive absent: %s" % archive_path)
    lowered = {path.lower() for path in wanted_paths}
    found = {}
    archive = bsa.open_archive(archive_path)
    try:
        for entry in archive.list_files():
            if entry.path.lower() not in lowered:
                continue
            package = shaders.parse_package(entry.path, archive.read_file(entry.path))
            for chunk in package.chunks:
                if chunk.kind != "ps":
                    continue
                sha = hashlib.sha256(chunk.code).hexdigest()
                if sha not in wanted_hashes:
                    continue
                program = asm.parse_program(chunk.code)
                features = asm.feature_vector(program)
                found[sha] = {
                    "meta": "0x%08x" % chunk.meta,
                    "constant_names": sorted(features["constant_names"]),
                    "inputs": features["inputs"],
                }
    finally:
        archive.close()
    missing = wanted_hashes - set(found)
    if missing:
        raise SystemExit("%d terrain hashes were not found in the archive" % len(missing))
    return found


def _classify(features: dict, texture_layers: int) -> str:
    names = set(features["constant_names"])
    overlay = any(name.startswith("WorldMapOverlay") for name in names)
    lod = "LODNoiseSampler" in names
    multitexture = "MultiTextureDiffuse0Sampler" in names
    if overlay and lod:
        return "terrain.lod_world_map_overlay"
    if overlay:
        return "terrain.world_map_overlay"
    if multitexture and texture_layers >= 12:
        return ("terrain.multitexture_14_15_lod" if lod
                else "terrain.multitexture_12_13")
    if lod:
        return "terrain.lod_noise_overlay"
    if "HeightSampler" in names:
        return "terrain.height_layers"
    raise SystemExit("unclassified terrain hash with %d textures" % texture_layers)


def build_table(corpus_dir: Path, manifest_path: Path, data_dir: str = "") -> dict:
    manifest = _load(manifest_path)
    corpus_path = corpus_dir / "corpus.json"
    ir_path = corpus_dir / "ps_ir.bin"
    corpus = _load(corpus_path)
    if not data_dir:
        data_dir = corpus["data_dir"]
    pack_counts = {row[0]: row[3] for row in manifest["hashes"]}
    rows = [row for row in corpus["rows"] if row["sha256"] in pack_counts]
    by_hash = {}
    for row in rows:
        by_hash.setdefault(row["sha256"], []).append(row)
    if set(by_hash) != set(pack_counts):
        raise SystemExit("terrain manifest and corpus hash sets differ")

    ir = differential.read_ir_dump(str(ir_path))
    metadata = _ctab_metadata(data_dir, {row["package"] for row in rows}, set(by_hash))
    result = []
    for sha in sorted(by_hash):
        chunks = by_hash[sha]
        representative = chunks[0]
        status, text = ir[representative["index"]]
        if status:
            raise SystemExit("canonical IR parse failure for %s" % sha)
        parsed = differential.parse_mojoshader(text)
        body = parsed["body"]
        op_counts = collections.Counter(
            asm.OPCODE_NAMES.get(ins["opcode"], "0x%02x" % ins["opcode"])
            for ins in body
        )
        sampled = set()
        for ins in body:
            if ins["opcode"] not in (0x42, 0x5D, 0x5F):
                continue
            for src in ins["srcs"]:
                if src["type"] == asm.REG_SAMPLER:
                    sampled.add(src["index"])
        defs = []
        hard_values = set()
        for reg, values in parsed["defs"]:
            vals = [_number(value) for value in values]
            outside = [i for i, value in enumerate(vals) if value < 0 or value > 1]
            hard_values.update(value for value in vals
                               if (value < 0 or value > 1) and value not in (-1, 0, 1))
            defs.append({
                "reg": reg,
                "values": vals,
                "out_of_range_components": ["xyzw"[i] for i in outside],
            })
        unsupported = sorted(set(op_counts) - DIRECT_OPS)
        flow = sorted(set(op_counts) & FLOW_OPS)
        texture_layers = len(sampled)
        stage_demand_min = len(body) - op_counts["texld"]
        features = metadata[sha]
        class_id = _classify(features, texture_layers)
        names = set(features["constant_names"])
        overlay = any(name.startswith("WorldMapOverlay") for name in names)
        lod = "LODNoiseSampler" in names
        alpha_blend = ("required_by_overlay_contract"
                       if class_id in ("terrain.lod_noise_overlay",
                                       "terrain.world_map_overlay",
                                       "terrain.lod_world_map_overlay")
                       else "unknown_external_state")
        if texture_layers > 3:
            refusal = {
                "name": "terrain.needs_multipass",
                "texture_layers": texture_layers,
                "minimum_passes": math.ceil(texture_layers / 3),
                "detail": "single-pass TEV has three texture units; no pass list exists in d9-shader-plan-v1",
            }
        else:
            refusal = {
                "name": "terrain.stage_budget",
                "stage_demand_min": stage_demand_min,
                "stage_limit": 6,
                "detail": "static non-texture instruction count already exceeds six stages",
            }
        blockers = ["terrain.normal_basis_prebake_required",
                    "terrain.single_accumulator_scheduler_required",
                    "terrain.material_contract_missing"]
        if unsupported:
            blockers.append("terrain.unsupported_opcodes")
        if flow:
            blockers.append("terrain.control_flow")
        if hard_values:
            blockers.append("terrain.def_literal_out_of_range")
        if texture_layers > 3:
            blockers.append("terrain.needs_multipass")
        result.append({
            "sha256": sha,
            "chunks": len(chunks),
            "class": class_id,
            "package": representative["package"],
            "meta": features["meta"],
            "texture_layers": texture_layers,
            "samplers": sorted(sampled),
            "instructions": len(body),
            "stage_demand_min": stage_demand_min,
            "opcodes": dict(sorted(op_counts.items())),
            "unsupported_opcodes": unsupported,
            "control_flow": flow,
            "defs": defs,
            "def_hard_values": sorted(hard_values),
            "features": {
                "height": "HeightSampler" in names,
                "lod": lod,
                "world_map_overlay": overlay,
                "alpha_blend": alpha_blend,
                "blend_evidence": ("pipeline overlay contract; not encoded in PS"
                                   if alpha_blend.startswith("required")
                                   else "D3D9 render state, absent from IR"),
            },
            "refusal": refusal,
            "blockers": blockers,
            "translated": 0,
            "draw_enabled": 0,
        })

    classes = []
    for class_id in CLASS_ORDER:
        subset = [row for row in result if row["class"] == class_id]
        if not subset:
            continue
        demands = [row["stage_demand_min"] for row in subset]
        signatures = collections.Counter(
            json.dumps([definition["values"] for definition in row["defs"]],
                       separators=(",", ":"))
            for row in subset
        )
        classes.append({
            "id": class_id,
            "hashes": len(subset),
            "chunks": sum(row["chunks"] for row in subset),
            "texture_layers": dict(sorted(collections.Counter(
                str(row["texture_layers"]) for row in subset).items())),
            "instructions": {"min": min(row["instructions"] for row in subset),
                             "max": max(row["instructions"] for row in subset)},
            "stage_demand_min": {
                "min": min(demands), "median": statistics.median(demands),
                "max": max(demands), "over_6": sum(value > 6 for value in demands),
            },
            "def_count": dict(sorted(collections.Counter(
                str(len(row["defs"])) for row in subset).items())),
            "def_signatures": [
                {"values": json.loads(signature), "hashes": count}
                for signature, count in signatures.most_common()
            ],
            "control_flow_hashes": sum(bool(row["control_flow"]) for row in subset),
            "unsupported_opcode_hashes": dict(sorted(collections.Counter(
                op for row in subset for op in row["unsupported_opcodes"]).items())),
            "refusals": dict(sorted(collections.Counter(
                row["refusal"]["name"] for row in subset).items())),
            "alpha_blend": subset[0]["features"]["alpha_blend"],
            "translated": 0,
            "draw_enabled": 0,
            "refused": len(subset),
        })

    total_values = collections.Counter(
        value for row in result
        for value in {component for definition in row["defs"]
                      for component in definition["values"]
                      if component < 0 or component > 1}
    )
    return {
        "schema": SCHEMA,
        "pack": "terrain",
        "append_only": True,
        "source": {
            "corpus": corpus_path.name,
            "corpus_sha256": _sha256(corpus_path),
            "canonical_ir": ir_path.name,
            "canonical_ir_sha256": _sha256(ir_path),
            "manifest": manifest_path.name,
            "manifest_sha256": _sha256(manifest_path),
            "retail_archive": "Skyrim - Shaders.bsa (read-only; no bytecode stored)",
        },
        "limits": {"tev_stages": 6, "texture_units": 3},
        "method": {
            "texture_layers": "distinct samplers read by texld/texldd/texldl",
            "instructions": "canonical executable body; dcl/def excluded",
            "stage_demand_min": "instructions minus texld; lower bound, not an emitted TEV plan",
            "lod_overlay": "CTAB vocabulary; alpha blend is an external pipeline contract",
            "simple_subset": "1..3 textures, stage_demand_min<=6, no control flow, direct opcodes, defs only 0/1/-1 outside [0,1]",
        },
        "totals": {
            "hashes": len(result),
            "chunks": sum(row["chunks"] for row in result),
            "translated": 0,
            "draw_enabled": 0,
            "refused": len(result),
            "simple_subset_hashes": sum(
                row["texture_layers"] <= 3 and row["stage_demand_min"] <= 6
                and not row["control_flow"] and not row["unsupported_opcodes"]
                and not row["def_hard_values"] for row in result),
            "needs_multipass_hashes": sum(
                row["refusal"]["name"] == "terrain.needs_multipass" for row in result),
            "needs_multipass_chunks": sum(
                row["chunks"] for row in result
                if row["refusal"]["name"] == "terrain.needs_multipass"),
            "stage_budget_hashes": sum(
                row["refusal"]["name"] == "terrain.stage_budget" for row in result),
            "stage_budget_chunks": sum(
                row["chunks"] for row in result
                if row["refusal"]["name"] == "terrain.stage_budget"),
            "control_flow_hashes": sum(bool(row["control_flow"]) for row in result),
            "hard_def_hashes": sum(bool(row["def_hard_values"]) for row in result),
        },
        "out_of_range_value_hashes": [
            {"value": value, "hashes": count}
            for value, count in total_values.most_common()
        ],
        "ctab_vocabulary_hashes": [
            {"name": name, "hashes": count}
            for name, count in collections.Counter(
                name for row in result for name in metadata[row["sha256"]]["constant_names"]
            ).most_common()
        ],
        "classes": classes,
        "hashes": result,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=Path(r"E:\host_sm3"))
    parser.add_argument("--data", default="")
    parser.add_argument("--manifest", type=Path, default=PACK_DIR / "manifest.json")
    parser.add_argument("--out", type=Path, default=PACK_DIR / "terrain_classes.json")
    args = parser.parse_args(argv)
    table = build_table(args.corpus, args.manifest, args.data)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(table, handle, indent=2, sort_keys=False)
        handle.write("\n")
    print("wrote %s: %d hashes / %d chunks, simple subset %d" % (
        args.out, table["totals"]["hashes"], table["totals"]["chunks"],
        table["totals"]["simple_subset_hashes"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
