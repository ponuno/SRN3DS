#!/usr/bin/env python3
"""Build explicit port-lite terrain approximations and verify their TEV plans.

The original retail shaders are not claimed equivalent.  Every entry names a
simplified specification, its approximation/error policy and a measured static
material contract.  No entry is runtime-enabled: the runtime has no terrain
approximation-policy selector yet.

Three approximate layer policies cover the family:

* ``unlit_diffuse_baseline`` / ``unlit_diffuse_overlay_pass`` -- one texture,
  one TEV stage; every other sampler, the normal basis and the lighting are
  dropped.
* ``dominant_three_layers`` -- keep texture units 0..2 of a 12/13-layer
  multitexture shader with a quantized equal-ish mix and drop every other
  layer.  The corpus has no per-cell weight histogram, so the weight error is
  reported as ``not_measured`` rather than invented.
* ``lod_ladder_default_near3`` -- the same three-unit plan for the 14/15-layer
  LOD shader, declared as the near tier of a near/mid/far = 3/2/1 ladder.

The original ``def`` vectors (``0.08``/``-0.04``/``2``/``3`` ...) are recorded
per plan: ``0``/``1``/``-1`` are described as foldable selectors, every other
value belongs to the dropped original expression and is explicitly listed as
unrepresentable in the simplified plan.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
import sys
from pathlib import Path

PACK_DIR = Path(__file__).resolve().parent
REPO = PACK_DIR.parents[2]
if str(PACK_DIR) not in sys.path:
    sys.path.insert(0, str(PACK_DIR))
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))

import classify
from classify import _load, _sha256
from shaders import asm
from skyrim import bsa, shaders

SRC_PRIMARY = 0
SRC_TEX0 = 3
SRC_CONST = 14
SRC_PREV = 15
FUNC_REPLACE = 0
FUNC_INTERPOLATE = 4
RGB_COLOR = 0
A_ALPHA = 0


def _write(path: Path, value: dict, indent=2) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, indent=indent, sort_keys=False)
        handle.write("\n")


def _load_programs(data_dir: str, paths: set[str], hashes: set[str]) -> dict:
    archive_path = os.path.join(data_dir, "Skyrim - Shaders.bsa")
    lowered = {path.lower() for path in paths}
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
                if sha not in hashes:
                    continue
                program = asm.parse_program(chunk.code)
                names = asm.constant_name_map(program)
                sampler_roles = {
                    decl.reg_index: names.get((3, decl.reg_index), "sampler%d" % decl.reg_index)
                    for decl in program.declarations if decl.reg_type == asm.REG_SAMPLER
                }
                inputs = {
                    decl.reg_index: (decl.usage_name, decl.usage_index)
                    for decl in program.declarations if decl.reg_type == asm.REG_INPUT
                }
                uv = collections.defaultdict(list)
                for ins in program.instructions:
                    if ins.opcode != 0x42 or len(ins.srcs) < 2:
                        continue
                    coord, sampler = ins.srcs[0], ins.srcs[1]
                    if sampler.reg_type == asm.REG_SAMPLER:
                        uv[sampler.index].append({
                            "reg_type": coord.reg_type, "reg_index": coord.index,
                            "swizzle": coord.swizzle, "modifier": coord.modifier,
                        })
                found[sha] = {
                    "program": program, "chunk": chunk, "sampler_roles": sampler_roles,
                    "inputs": inputs, "uv": uv,
                    "sha1": hashlib.sha1(chunk.code).hexdigest(),
                }
    finally:
        archive.close()
    missing = hashes - set(found)
    if missing:
        raise SystemExit("%d selected hashes absent from archive" % len(missing))
    return found


def _material_contract(row: dict, info: dict, selected: list[int], blend: str,
                       allow_uv_approx: bool) -> dict | None:
    roles = info["sampler_roles"]
    inputs = info["inputs"]
    textures = []
    for unit, sampler in enumerate(selected):
        role = roles.get(sampler, "")
        if sampler != unit:
            return None
        if row["class"].startswith("terrain.multitexture"):
            wanted = "MultiTextureDiffuse%dSampler" % sampler
        else:
            wanted = "DiffuseSampler" if sampler == 0 else role
        if role != wanted:
            return None
        samples = info["uv"].get(sampler, [])
        if not samples:
            return None
        coord = samples[0]
        direct = (coord["reg_type"] == asm.REG_INPUT and coord["reg_index"] == 0
                  and coord["modifier"] == 0 and coord["swizzle"][:2] == "xy"
                  and inputs.get(0) == ("texcoord", 0))
        if not direct and not allow_uv_approx:
            return None
        textures.append({
            "unit": unit, "sampler_reg": sampler, "role": role,
            "sampler_type": "2d", "texcoord_reg": 0,
            "format_policy": "ldr_2d_required",
            "uv_policy": "original_texcoord0" if direct else "replace_original_with_texcoord0",
            "original_uv": coord,
        })
    return {
        "status": "measured_static",
        "measured_from": ["dcl_2d", "CTAB sampler register set", "texld UV operand"],
        "binding_observation": "static_only; no live draw binding was observed",
        "textures": textures,
        "constants": [],
        "varyings": [],
        "blend": blend,
        "depth": "preserve_guest_state",
        "alpha_test": "preserve_guest_state",
    }


def _stage(func_rgb: int, func_a: int, src_rgb: list[int], src_a: list[int],
           literal: int = 0) -> dict:
    return {
        "func_rgb": func_rgb, "func_a": func_a,
        "src_rgb": src_rgb, "src_a": src_a,
        "op_rgb": [RGB_COLOR] * 3, "op_a": [A_ALPHA] * 3,
        "scale_rgb": 0, "scale_a": 0,
        "const_reg": 255, "const_is_literal": 1 if literal else 0,
        "const_rgba8": literal,
    }


def _single_stage() -> list[dict]:
    return [_stage(FUNC_REPLACE, FUNC_REPLACE,
                   [SRC_TEX0, SRC_PREV, SRC_PREV],
                   [SRC_TEX0, SRC_PREV, SRC_PREV])]


def _near3_stages() -> list[dict]:
    return [
        _single_stage()[0],
        _stage(FUNC_INTERPOLATE, FUNC_INTERPOLATE,
               [SRC_PREV, SRC_CONST, SRC_TEX0 + 1],
               [SRC_PREV, SRC_CONST, SRC_TEX0 + 1], 0x7F7F7F7F),
        _stage(FUNC_INTERPOLATE, FUNC_INTERPOLATE,
               [SRC_PREV, SRC_CONST, SRC_TEX0 + 2],
               [SRC_PREV, SRC_CONST, SRC_TEX0 + 2], 0xAAAAAAAA),
    ]


def _selector_comp(op: int, channel: int, alpha: bool) -> int:
    if op != 0:
        raise ValueError("approximation verifier only emits identity selectors")
    return 3 if alpha else channel


def _symbolic_output(plan: dict) -> tuple:
    prev = None
    for stage in plan["stage_data"]:
        current = []
        for channel in range(4):
            alpha = channel == 3
            func = stage["func_a"] if alpha else stage["func_rgb"]
            arity = 1 if func == FUNC_REPLACE else 3 if func == FUNC_INTERPOLATE else 0
            if not arity:
                raise ValueError("unexpected function %d" % func)
            values = []
            for index in range(arity):
                source = (stage["src_a"] if alpha else stage["src_rgb"])[index]
                op = (stage["op_a"] if alpha else stage["op_rgb"])[index]
                comp = _selector_comp(op, channel, alpha)
                if SRC_TEX0 <= source <= SRC_TEX0 + 2:
                    values.append(("tex", source - SRC_TEX0, comp))
                elif source == SRC_CONST:
                    values.append(("lit", stage["const_rgba8"], comp))
                elif source == SRC_PREV:
                    if prev is None:
                        raise ValueError("stage 0 reads PREVIOUS")
                    values.append(prev[comp])
                else:
                    raise ValueError("unexpected source %d" % source)
            if func == FUNC_REPLACE:
                current.append(values[0])
            elif func == FUNC_INTERPOLATE:
                # PICA: o1*o2 + o3*(1-o2) == mix(o3,o1,o2).
                current.append(("mix", values[2], values[0], values[1]))
            else:
                raise ValueError("unexpected function %d" % func)
        prev = tuple(current)
    return prev


def _verify(plan: dict, kind: str) -> dict:
    actual = _symbolic_output(plan)
    expected = []
    for channel in range(4):
        t0 = ("tex", 0, channel)
        if kind == "single":
            expected.append(t0)
        else:
            t1 = ("tex", 1, channel)
            t2 = ("tex", 2, channel)
            w1 = ("lit", 0x7F7F7F7F, channel)
            w2 = ("lit", 0xAAAAAAAA, channel)
            expected.append(("mix", t2, ("mix", t1, t0, w1), w2))
    if actual != tuple(expected):
        raise SystemExit("symbolic mismatch for %s" % plan.get("id", "(unnamed)"))
    # Every leaf and interpolation weight is in [0,1], so each TEV LDR clamp is
    # the identity on this simplified expression.
    return {
        "status": "pass", "reference": "declared simplified specification",
        "model": "PICA LDR clamp; all leaves/weights and mix intermediates are in [0,1]",
        "expression": ("oC0=tex(s0)" if kind == "single" else
                       "oC0=lerp_tev(prev,tex1,127/255); then lerp_tev(prev,tex2,170/255)"),
    }


def _def_policy(row: dict) -> dict:
    """Split original ``def`` literals into foldable and dropped values."""
    values = sorted({float(value) for definition in row["defs"]
                     for value in definition["values"]})
    hard = [value for value in values if value < 0 or value > 1]
    folded = [value for value in (-1.0, 0.0, 1.0) if value in values]
    uniform = [value for value in values if value not in (-1.0, 0.0, 1.0)]
    return {
        "literal_folded": ["0", "1", "-1"],
        "folded_present": folded,
        "uniform_or_approximated": uniform,
        "status": ("simplified plan has no def slots; folded values are named as "
                   "selectors and every other original literal is dropped with "
                   "the original expression, never silently clamped"),
        "hard_values_present": hard,
        "all_values_present": values,
    }


def _weight_error() -> dict:
    return {
        "status": "not_measured",
        "reason": ("the corpus stores shader bytecode/CTAB metadata, not terrain "
                   "cell weight tables; no per-cell layer histogram exists"),
        "reported_error": None,
    }


def _layer_approximation(policy: str, row: dict, original_roles: list[str],
                         *, lod: bool) -> dict:
    w1, w2 = 128 / 255.0, 85 / 255.0
    weights = [(1 - w2) * (1 - w1), (1 - w2) * w1, w2]
    dropped = row["texture_layers"] - 3
    approximation = {
        "policy": policy,
        "layers_kept": 3, "kept_samplers": [0, 1, 2],
        "dropped_layers": dropped, "textures_kept": 3,
        "layer_selection": "fixed_units_0_1_2",
        "dominance_evidence": "not_measured",
        "dropped_policy": ("keep MultiTextureDiffuse0..2; drop the remaining "
                           "diffuse layers, every normal/height sampler and all lighting"),
        "dropped_roles": original_roles[3:],
        "lighting": "dropped", "normal_maps": "dropped",
        "world_map_normal_overlay": "not_present",
        "lod_noise": "dropped" if lod else "not_present",
        "renormalize": True, "weights": weights,
        "literal_quantization": ["127/255", "170/255"],
        "max_weight_deviation_vs_equal": max(abs(weight - 1 / 3) for weight in weights),
        "max_colour_error_vs_equal": max(abs(weight - 1 / 3) for weight in weights),
        "weight_error_vs_original": _weight_error(),
        "max_channel_error_bound": 1.0,
        "error_basis": ("weight deviation is exact for convex [0,1] colours; the "
                        "original-error bound is conservative because dropped "
                        "layers, normal maps and lighting are not measured"),
        "def_policy": _def_policy(row),
        "reason": ("three PICA texture units; guest per-cell weights are not "
                   "available offline, so no layer is claimed dominant"),
        "expected_visual": ("smooth equal-ish three-layer albedo from fixed units "
                            "0..2; loses higher-layer dominance, normals and "
                            "lighting detail"),
    }
    if lod:
        approximation["lod_tiers"] = {"near": 3, "mid": 2, "far": 1}
        approximation["lod_tier_default"] = "near"
        approximation["lod_tier_note"] = (
            "one plan per hash; the near tier (3 layers) is the default, mid/far "
            "are runtime distance tiers and are not separate committed plans")
    return approximation


def _base_plan(row: dict, info: dict, kind: str,
               selected: list[int], blend: str, runtime_candidate: int) -> dict:
    stages = _single_stage() if kind == "single" else _near3_stages()
    textures = [{"texcoord_reg": 0, "sampler_reg": sampler, "sampler_type": 2}
                for sampler in selected]
    material = _material_contract(row, info, selected, blend, kind == "single")
    if material is None:
        raise SystemExit("material contract is incomplete for %s" % row["sha256"])
    original_roles = [info["sampler_roles"].get(index, "sampler%d" % index)
                      for index in row["samplers"]]
    if kind == "single":
        world_overlay = row["class"] in ("terrain.world_map_overlay",
                                         "terrain.lod_world_map_overlay")
        lod_overlay = row["class"] in ("terrain.lod_noise_overlay",
                                       "terrain.lod_world_map_overlay")
        overlay = world_overlay or lod_overlay
        approximation = {
            "policy": "unlit_diffuse_overlay_pass" if overlay else "unlit_diffuse_baseline",
            "layers_kept": 1, "kept_samplers": [0], "textures_kept": 1,
            "dropped_layers": max(0, row["texture_layers"] - 1),
            "dropped_policy": "keep DiffuseSampler only; drop all other samplers and lighting",
            "dropped_roles": original_roles[1:],
            "lighting": "dropped", "parallax": "dropped", "shadows": "dropped",
            "world_map_normal_overlay": "dropped" if world_overlay else "not_present",
            "lod_noise": "dropped" if lod_overlay else "not_present",
            "renormalize": False,
            "weight_error_vs_original": _weight_error(),
            "def_policy": _def_policy(row),
            "reason": "single-pass PICA port-lite baseline; no baked atlas or live weight table",
            "expected_visual": ("flat unlit albedo; loses normal lighting, parallax, shadows"
                                + (" and world-map normal detail" if world_overlay else "")
                                + (" and LOD-noise detail" if lod_overlay else "")),
            "max_channel_error_bound": 1.0,
            "error_basis": "conservative [0,1] colour bound; no per-cell weight histogram exists",
        }
        note = "port-lite unlit diffuse baseline; original terrain lighting/detail is explicitly dropped"
        preconditions = ["approx_policy_selected", "texture0_ldr_2d"]
    else:
        policy = ("lod_ladder_default_near3"
                  if row["class"] == "terrain.multitexture_14_15_lod"
                  else "dominant_three_layers")
        approximation = _layer_approximation(
            policy, row, original_roles,
            lod=row["class"] == "terrain.multitexture_14_15_lod")
        note = ("port-lite three-layer quantized equal-ish diffuse mix; original "
                "weights, dropped layers and lighting are replaced")
        preconditions = ["approx_policy_selected", "texture0_ldr_2d",
                         "texture1_ldr_2d", "texture2_ldr_2d"]
    plan = {
        "id": "terrain.approx.%s" % row["sha256"][:8],
        "sha256": row["sha256"], "sha1": info["sha1"],
        "stage": "ps_3_0", "kind": "tev", "family": "terrain",
        "version": "0xffff0300", "instructions": row["instructions"],
        "draw_enabled": 0, "runtime": 0, "runtime_candidate": runtime_candidate,
        "quality": "approx", "pass": 1,
        "source_package": row["package"], "source_key": row["meta"],
        "note": note, "preconditions": preconditions,
        "stages": len(stages), "stage_data": stages,
        "textures": textures, "inputs": [], "constants": [],
        "uses_vertex_color": 0, "requires_alpha_fixup": 0, "ldr_clamped": 1,
        "approximation": approximation,
        "simplified_spec": {
            "kind": kind, "selected_samplers": selected,
            "expression": ("oC0=tex(s0)" if kind == "single" else
                           "oC0=lerp_tev(prev,tex1,127/255); then lerp_tev(prev,tex2,170/255)"),
        },
        "material_contract": material,
        "symbolic_verify": _verify({"stage_data": stages}, kind),
        "draw_blockers": (["approximation_policy_not_wired", "runtime_binding_not_observed"]
                          if runtime_candidate else
                          ["multi_texture_or_blend_contract_not_wired",
                           "approximation_policy_not_wired", "runtime_binding_not_observed"]),
    }
    return plan


def _resolve_data_dir(corpus_dir: Path) -> str:
    corpus_path = corpus_dir / "corpus.json"
    if corpus_path.exists():
        with corpus_path.open(encoding="utf-8") as handle:
            configured = json.load(handle).get("data_dir", "")
        if configured and os.path.isdir(configured):
            return configured
    for candidate in (r"F:\Skyrim\Data", r"E:\Skyrim\Data"):
        if os.path.isdir(candidate):
            return candidate
    raise SystemExit("retail data dir absent; pass --data")


def build_plans(corpus_dir: Path, manifest_path: Path, data_dir: str = "") -> tuple[dict, dict]:
    table = _load(PACK_DIR / "terrain_classes.json")
    if not data_dir:
        data_dir = _resolve_data_dir(corpus_dir)
    rows = table["hashes"]

    # 11 one-texture height-layer baselines: the ten flow-free three-layer rows
    # get a complete static material contract (runtime candidates); the eleventh
    # is a four-layer row translated with the same baseline but no candidate.
    height = [row for row in rows if row["class"] == "terrain.height_layers"
              and row["texture_layers"] in (3, 4) and not row["control_flow"]]
    height.sort(key=lambda row: (row["texture_layers"], -row["chunks"], row["sha256"]))
    chosen_height = height[:11]
    overlays = [row for row in rows if row["class"] in (
        "terrain.lod_noise_overlay", "terrain.world_map_overlay",
        "terrain.lod_world_map_overlay")]
    multi = [row for row in rows if row["class"] in (
        "terrain.multitexture_12_13", "terrain.multitexture_14_15_lod")]
    multi.sort(key=lambda row: (row["class"], -row["chunks"], row["sha256"]))
    selected_rows = chosen_height + overlays + multi
    if len(selected_rows) != 138:
        raise SystemExit("expected 138 selected hashes, got %d" % len(selected_rows))

    selected_hashes = {row["sha256"] for row in selected_rows}
    infos = _load_programs(data_dir, {row["package"] for row in selected_rows}, selected_hashes)
    plans = []
    height_rank = {row["sha256"]: rank for rank, row in enumerate(chosen_height)}
    for row in selected_rows:
        if row in chosen_height:
            kind, selected, blend = "single", [0], "preserve_guest_state"
            candidate = 1 if height_rank[row["sha256"]] < 10 else 0
        elif row in overlays:
            kind, selected, blend, candidate = "single", [0], "src_alpha", 0
        else:
            kind, selected, blend, candidate = "near3", [0, 1, 2], "preserve_guest_state", 0
        plans.append(_base_plan(row, infos[row["sha256"]],
                                kind, selected, blend, candidate))
    plans.sort(key=lambda plan: plan["sha256"])
    document = {
        "schema": "d9-shader-plan-v1", "archive": "Skyrim - Shaders.bsa",
        "note": ("Explicit port-lite terrain approximations. Every plan is verified "
                 "against its declared simplified specification; none is runtime-enabled."),
        "entries": plans,
    }
    return document, table


def update_manifest(manifest_path: Path, plans: list[dict]) -> None:
    raw_before = manifest_path.read_bytes()
    manifest = _load(manifest_path)
    manifest["coverage"] = {
        "translated": len(plans), "draw-enabled": 0,
        "refused": manifest["unique_hashes"] - len(plans), "contract_only": 0,
    }
    manifest["plans"] = [{
        "id": plan["id"], "sha256": plan["sha256"], "status": "translated",
        "kind": plan["kind"], "family": plan["family"],
    } for plan in plans]
    _write(manifest_path, manifest, indent=1)
    if b"\r\n" in raw_before:
        manifest_path.write_bytes(manifest_path.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))


def build_report(plans: list[dict], table_path: Path) -> dict:
    table = _load(table_path)
    by_hash = {row["sha256"]: row for row in table["hashes"]}
    policies = collections.Counter(plan["approximation"]["policy"] for plan in plans)
    chunks = collections.Counter()
    classes = collections.Counter()
    for plan in plans:
        row = by_hash[plan["sha256"]]
        chunks[plan["approximation"]["policy"]] += row["chunks"]
        classes[row["class"]] += 1
    layer_plans = [plan for plan in plans
                   if plan["approximation"]["layers_kept"] > 1]
    w1, w2 = 128 / 255.0, 85 / 255.0
    weights = [(1 - w2) * (1 - w1), (1 - w2) * w1, w2]
    return {
        "schema": "terrain-approximations/1", "pack": "terrain",
        "source_table": table_path.name, "source_table_sha256": _sha256(table_path),
        "totals": {"hashes": len(plans), "draw_enabled": 0, "runtime": 0,
                   "runtime_candidates": sum(plan["runtime_candidate"] for plan in plans),
                   "chunks": sum(by_hash[plan["sha256"]]["chunks"] for plan in plans)},
        "classes": [{"class": cls, "hashes": count}
                    for cls, count in sorted(classes.items())],
        "policies": [{"policy": policy, "hashes": count, "chunks": chunks[policy]}
                     for policy, count in sorted(policies.items())],
        "weight_analysis": {
            "per_cell_histogram_available": False,
            "reason": ("the corpus has shader bytecode/CTAB metadata, not terrain "
                       "cell weight tables"),
            "layer_plan_hashes": len(layer_plans),
            "three_layer_weights": weights,
            "max_deviation_vs_equal": max(abs(weight - 1 / 3) for weight in weights),
            "weight_error_vs_original": "not_measured",
            "max_original_channel_error_bound": 1.0,
            "bound_note": ("conservative [0,1] colour bound after dropping layers, "
                           "normal maps and lighting; weights themselves are not measured"),
            "layer_selection": "fixed_units_0_1_2",
            "layer_selection_note": ("no per-cell histogram exists, so units 0..2 are "
                                     "kept as the stable base layers; no layer is "
                                     "claimed to be dominant by weight"),
        },
        "lod_ladder": {
            "tiers": {"near": 3, "mid": 2, "far": 1},
            "default_tier": "near",
            "committed_plans": "one three-layer plan per hash",
        },
        "def_policy": {
            "folded": ["0", "1", "-1"],
            "other_values": "recorded per plan and dropped with the original expression",
            "silent_clamp": False,
        },
        "plans": [{
            "sha256": plan["sha256"], "id": plan["id"],
            "policy": plan["approximation"]["policy"],
            "layers_kept": plan["approximation"]["layers_kept"],
            "stages": plan["stages"], "textures": len(plan["textures"]),
            "quality": plan["quality"], "runtime_candidate": plan["runtime_candidate"],
            "weight_error_vs_original":
                plan["approximation"]["weight_error_vs_original"]["status"],
            "expected_visual": plan["approximation"]["expected_visual"],
            "symbolic_verify": plan["symbolic_verify"]["status"],
        } for plan in plans],
        "blockers": ["terrain approximation policy is not selectable by the runtime",
                     "live texture/format/sampler bindings have not been observed",
                     "two/three-texture generic backend gate and overlay blend contract are not wired",
                     "no per-cell weight histogram: weight error is not_measured"],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=Path(r"E:\host_sm3"))
    parser.add_argument("--data", default="")
    args = parser.parse_args(argv)
    manifest_path = PACK_DIR / "manifest.json"
    plans_doc, _ = build_plans(args.corpus, manifest_path, args.data)
    _write(PACK_DIR / "plans.json", plans_doc)
    update_manifest(manifest_path, plans_doc["entries"])

    table_path = PACK_DIR / "terrain_classes.json"
    if (args.corpus / "corpus.json").exists():
        table = classify.build_table(args.corpus, manifest_path, args.data)
        _write(table_path, table)
    else:
        table = _load(table_path)
    planned = {plan["sha256"] for plan in plans_doc["entries"]}
    refusals = classify.build_refusals(table, planned)
    refusals["source_table_sha256"] = _sha256(table_path)
    _write(PACK_DIR / "refusals.json", refusals)
    _write(PACK_DIR / "approximations.json", build_report(plans_doc["entries"], table_path))
    print("wrote %d approximate plans; runtime candidates %d; refusals %d" % (
        len(plans_doc["entries"]),
        sum(plan["runtime_candidate"] for plan in plans_doc["entries"]),
        len(refusals["hashes"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
