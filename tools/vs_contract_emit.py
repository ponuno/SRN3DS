#!/usr/bin/env python3
"""Emit and verify the lighting/UI ``pica_vs`` census contracts (host only).

The phase-1 planner classified 130 lighting and 40 UI vertex shaders as
``pica_vs`` but committed no descriptor for them (``contract_only``).  This
module closes that queue with the same three-layer proof the skinning emitter
uses:

1. canonical vs_3_0 IR from the retail ``Skyrim - Shaders.bsa``;
2. picasso assembly lowered to PICA200, compiled to a non-empty ``.shbin``;
3. a deterministic numeric run (256 samples, ``max |dpos| <= 1e-4`` and every
   kept varying within the same bound) plus a picasso pass.

PICA has one raster colour and three texture-coordinate varyings.  A D3D
program may declare more (``color1``, ``texcoord3..7``).  Such a contract is
lowered into the available slots:

* texture coordinates are assigned to ``texcoord0..2`` in declaration order;
* the first colour becomes ``color``;
* a second colour is packed into a free ``texcoord`` slot when one exists;
* anything that no longer fits is a *declared* dropped output.

A lowering that only renames a slot keeps the value exact but is still marked
``quality: approx`` because the D3D semantic moved.  A contract whose original
semantics fit PICA unchanged is ``quality: exact``.  Every entry keeps
``draw_enabled: 0`` / ``runtime: 0``: the shared plan table and the
``d3d9_pica_vs`` upload path only carry the skinning and GFx menu contracts
today, and this module does not invent a draw path.  ``emitted-verified`` plus
``draw_enabled: 0`` is the same state the skinning catalog uses for its
non-representative programs.

Two ISA gaps are lowered rather than refused:

* a relative ``c0[a0.x]`` palette lookup plus a c128+ absolute bank uses a
  compact uniform window (palette rows first, absolute rows packed after it),
  recorded as ``uniform_map: compact_palette`` and checked by ``plan_verify``;
* picasso has no ``sincos``, so sin/cos use a range-reduced degree 17/16
  Taylor polynomial (``flr`` + ``mad``); the numeric run compares against the
  original ``sincos`` IR within 1e-4 (relative 1e-6 for large magnitudes).

Every contract in this queue therefore reaches ``emitted-verified``.  A
physical/ISA gap would be a named refusal, but none remains.

Run::

    python tools\\vs_contract_emit.py --pack lighting --data F:\\Skyrim\\Data
    python tools\\vs_contract_emit.py --pack ui --data F:\\Skyrim\\Data
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from typing import Dict, List, Optional, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
for _path in (REPO, os.path.join(REPO, "tools"),
              os.path.join(REPO, "tools", "shaders"),
              os.path.join(REPO, "source", "shader_packs", "skinning")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

from shaders import asm, compile as C  # noqa: E402
import pica_vs_emit as S  # noqa: E402

ARCHIVE = "Skyrim - Shaders.bsa"
DATA_CANDIDATES = [r"F:\Skyrim\Data",
                   r"E:\SteamLibrary\steamapps\common\Skyrim\Data"]
SAMPLES = 256
TOLERANCE = 1e-4
PREFIX = {"lighting": "lit.vs", "ui": "ui.vs"}


class Refusal(Exception):
    pass


def pack_dir(pack: str) -> str:
    return os.path.join(REPO, "source", "shader_packs", pack)


def find_data(explicit: str = "") -> str:
    if explicit:
        return explicit
    for candidate in DATA_CANDIDATES:
        if os.path.exists(os.path.join(candidate, ARCHIVE)):
            return candidate
    raise Refusal("retail_archive_absent")


def contract_hashes(pack: str) -> Dict[str, dict]:
    path = os.path.join(pack_dir(pack), "manifest.json")
    with open(path, encoding="utf-8") as handle:
        manifest = json.load(handle)
    return {row[0]: {"kind": row[1], "class": row[2], "chunks": row[3]}
            for row in manifest["hashes"]
            if row[1] == "vs" and row[2] == "pica_vs"}


def load_irs(hashes, data_dir: str) -> Dict[str, Tuple[C.IR, str, str]]:
    from fxp_compile import _load_packages

    out = {}
    for package in _load_packages(find_data(data_dir)):
        for chunk in package.chunks:
            if chunk.kind != "vs":
                continue
            ir = C.build_ir(chunk.code)
            if ir.sha256 in hashes and ir.sha256 not in out:
                out[ir.sha256] = (ir, package.path, "0x%08x" % chunk.meta)
    missing = set(hashes) - set(out)
    if missing:
        raise Refusal("bytecode_for_%d_contracts_not_found" % len(missing))
    return out


def _mapped(output: dict, usage: int, index: int, name: str,
            mode: str) -> dict:
    item = dict(output)
    item["d3d_usage"] = output["usage"]
    item["d3d_usage_index"] = output["usage_index"]
    item["usage"] = usage
    item["usage_index"] = index
    item["usage_name"] = {0: "position", 5: "texcoord", 10: "color"}[usage]
    item["pica_name"] = name
    item["map_mode"] = mode
    return item


def lower_varyings(plan: dict) -> dict:
    """Assign D3D outputs to PICA's position / texcoord0..2 / color slots."""
    outputs = plan["outputs"]
    position = [o for o in outputs if o["usage"] == 0]
    texcoords = sorted((o for o in outputs if o["usage"] == 5),
                       key=lambda o: (o["usage_index"], o["reg_index"]))
    colors = sorted((o for o in outputs if o["usage"] == 10),
                    key=lambda o: (o["usage_index"], o["reg_index"]))
    if len(position) != 1:
        raise Refusal("position_output_count_%d" % len(position))
    free = [0, 1, 2]
    mapped: List[dict] = []
    varying_map: List[dict] = []
    dropped: List[dict] = []
    modes = set()

    def add(output, usage, index, name, mode):
        item = _mapped(output, usage, index, name, mode)
        mapped.append(item)
        varying_map.append({
            "reg_index": output["reg_index"],
            "d3d_usage": output["usage"],
            "d3d_usage_name": output["usage_name"],
            "d3d_usage_index": output["usage_index"],
            "pica_name": name,
            "pica_semantic": "position" if usage == 0 else
                             ("texcoord%d" % index if usage == 5 else "color"),
            "mode": mode,
        })
        modes.add(mode)

    add(position[0], 0, 0, "outpos", "natural")
    for output in texcoords:
        if not free:
            dropped.append(output)
            varying_map.append({
                "reg_index": output["reg_index"],
                "d3d_usage": output["usage"],
                "d3d_usage_name": output["usage_name"],
                "d3d_usage_index": output["usage_index"],
                "pica_name": None, "pica_semantic": None,
                "mode": "dropped",
            })
            modes.add("dropped")
            continue
        index = free.pop(0)
        mode = "natural" if index == output["usage_index"] else "remap"
        add(output, 5, index, "outtc%d" % index, mode)
    for position_index, output in enumerate(colors):
        if position_index == 0:
            mode = "natural" if output["usage_index"] == 0 else "remap"
            add(output, 10, 0, "outclr", mode)
        elif free:
            index = free.pop(0)
            add(output, 5, index, "outtc%d" % index, "packed_color")
        else:
            dropped.append(output)
            varying_map.append({
                "reg_index": output["reg_index"],
                "d3d_usage": output["usage"],
                "d3d_usage_name": output["usage_name"],
                "d3d_usage_index": output["usage_index"],
                "pica_name": None, "pica_semantic": None,
                "mode": "dropped",
            })
            modes.add("dropped")
    if not mapped or len(mapped) > 7:
        raise Refusal("lowered_outputs_%d" % len(mapped))
    approx = modes - {"natural"}
    return {"outputs": mapped, "varying_map": varying_map,
            "dropped": dropped, "approx": bool(approx)}


def approximation_text(pack: str, lowered: dict) -> str:
    notes = []
    remapped = [v for v in lowered["varying_map"]
                if v["mode"] == "remap"]
    packed = [v for v in lowered["varying_map"]
              if v["mode"] == "packed_color"]
    if remapped:
        notes.append("remapped " + ", ".join(
            "%s%d->%s" % (v["d3d_usage_name"], v["d3d_usage_index"],
                          v["pica_semantic"]) for v in remapped))
    if packed:
        notes.append("packed " + ", ".join(
            "%s%d->%s" % (v["d3d_usage_name"], v["d3d_usage_index"],
                          v["pica_semantic"]) for v in packed))
    if lowered["dropped"]:
        notes.append("dropped " + ", ".join(
            "%s%d" % (o["usage_name"], o["usage_index"])
            for o in lowered["dropped"]))
    return ("PICA varying envelope: " + "; ".join(notes)) if notes else ""


def _plan_id(pack: str, sha: str) -> str:
    return "%s.%s" % (PREFIX[pack], sha[:8])


def _uniforms(rows: int) -> list:
    return [{"row": row, "d3d": row} for row in range(rows)]


def _refusal_reason(ir: C.IR) -> Optional[str]:
    body = [ins for ins in ir.body if ins.name not in ("def", "nop")]
    absolute = sorted({src.index for ins in body for src in ins.srcs
                       if src.reg_type == asm.REG_CONST
                       and not src.relative and src.index not in ir.defs})
    relative = sorted({src.index for ins in body for src in ins.srcs
                       if src.reg_type == asm.REG_CONST and src.relative})
    if not relative and absolute and max(absolute) >= S.MAX_UNIFORMS:
        return "pica_vs_uniform_bank_%d_rows" % (max(absolute) + 1)
    return None


def emit_contract(pack: str, sha: str, ir: C.IR, package: str,
                  key: str) -> Tuple[dict, Optional[bytes]]:
    plan, _cls, why = C.plan_vs(ir)
    if plan is None:
        raise Refusal("plan_vs_%s" % why)
    lowered = lower_varyings(plan)
    dropped_regs = [[o["type"], o["reg_index"]]
                    for o in lowered["dropped"]]
    plan_id = _plan_id(pack, sha)
    base = {
        "id": plan_id, "sha256": sha, "sha1": ir.sha1,
        "version": "0x%08x" % ir.version, "stage": "vs_3_0",
        "kind": "pica_vs", "family": pack,
        "instructions": len([ins for ins in ir.body
                             if ins.name not in ("def", "nop")]),
        "inputs": plan["inputs"], "outputs": lowered["outputs"],
        "constants": sorted({src.index for ins in ir.body for src in ins.srcs
                             if src.reg_type == asm.REG_CONST
                             and not src.relative and src.index not in ir.defs}),
        "expandable": plan.get("expandable", []),
        "skinning": False,
        "source_package": package, "source_key": key,
        "varying_map": lowered["varying_map"],
        "dropped_output_regs": dropped_regs,
        "draw_enabled": 0, "runtime": 0,
    }
    reason = _refusal_reason(ir)
    if reason is not None:
        base.update({
            "codegen": "refused", "quality": "exact",
            "refusal": reason,
            "note": ("PICA VS named refusal (%s): no draw, no silent "
                     "approximation" % reason),
            "preconditions": ["pica_vs_emit_request", reason],
        })
        return base, None

    body = [ins for ins in ir.body if ins.name not in ("def", "nop")]
    relative = sorted({src.index for ins in body for src in ins.srcs
                       if src.reg_type == asm.REG_CONST and src.relative})
    palette_rows = (max(relative) + 1) if relative else 0
    identity = palette_rows == 0
    abs_rows, uniform_rows = S._d3d_const_rows(ir, 0, identity, palette_rows)
    if identity:
        uniforms = _uniforms(uniform_rows)
    else:
        rows_by_d3d = {row: row for row in range(palette_rows)}
        rows_by_d3d.update({d3d: row for d3d, row in abs_rows.items()})
        uniforms = [{"row": row, "d3d": d3d}
                    for d3d, row in sorted(rows_by_d3d.items(),
                                           key=lambda kv: kv[1])]
    base["uniform_registers"] = uniform_rows
    base["uniform_map"] = "identity" if identity else "compact_palette"
    lowered_plan = dict(plan)
    lowered_plan["id"] = plan_id
    lowered_plan["outputs"] = lowered["outputs"]
    lowered_plan["uniform_registers"] = uniform_rows
    lowered_plan["dropped_output_regs"] = dropped_regs
    entry, shbin = S.verify(lowered_plan, ir, samples=SAMPLES, bones=0,
                            identity_consts=identity, rel_tolerance=1e-6,
                            palette_rows=palette_rows,
                            magnitude_limit=1e6)
    if entry["status"] != "pass" or not shbin:
        raise Refusal("numeric_verification_failed_%s" % entry["status"])
    words = (int.from_bytes(shbin[0x18:0x1C], "little")
             if len(shbin) >= 0x1C else entry["instructions"])
    approx = approximation_text(pack, lowered)
    if any(ins.name == "sincos" for ins in ir.body):
        note = ("sincos lowered to a range-reduced degree 17/16 Taylor "
                "polynomial (max |sin/cos| error < 1e-8)")
        approx = (approx + "; " + note) if approx else (
            "PICA ISA: " + note)
    quality = "approx" if approx else "exact"
    base.update({
        "codegen": "emitted-verified", "quality": quality,
        "uniform_registers": uniform_rows,
        "emitted_asm": "generated/%s.v.pica" % plan_id,
        "emitter": {
            "samples": entry["samples"], "tolerance": entry["tolerance"],
            "rel_tolerance": entry["rel_tolerance"],
            "max_abs_position": entry["max_abs_position"],
            "max_abs_varying": entry["max_abs_varying"],
            "max_rel_varying": entry["max_rel_varying"],
            "magnitude_limit": entry["magnitude_limit"],
            "skipped_ill_conditioned": entry["skipped_ill_conditioned"],
            "picasso": entry["picasso"], "bones": 0,
            "uniform_rows": uniform_rows, "instructions": words,
            "shbin": "generated/%s.shbin" % plan_id,
            "shbin_sha256": entry["shbin_sha256"],
            "shbin_bytes": entry["shbin_bytes"],
            "inputs": [{"usage": i["usage"], "usage_name": i["usage_name"],
                        "reg_index": i["reg_index"]}
                       for i in plan["inputs"]],
            "uniforms": uniforms,
            "uniform_map": base["uniform_map"],
        },
        "note": (
            "PICA VS emitted and numerically verified against the vs_3_0 IR "
            "(%d samples, max |dpos| %.1e, picasso pass); draw disabled: only "
            "the six runtime-certified lighting representatives are in the "
            "PICA VS catalog" % (entry["samples"], entry["max_abs_position"])),
        "preconditions": ["exact_hash_descriptor_match", "pica_vs_catalog",
                          "runtime_upload_path_pending"],
    })
    if approx:
        base["approximation"] = approx
    base["emitter"]["varying_map"] = lowered["varying_map"]
    if lowered["dropped"]:
        base["dropped_outputs"] = [
            {"usage": o["usage"], "usage_name": o["usage_name"],
             "usage_index": o["usage_index"]} for o in lowered["dropped"]]
    return base, shbin


RUNTIME_ATTR = {
    0: ("D9_PICA_VS_ATTR_POSITION", "D9_DECL_FLOAT3", 3,
        "D9_PICA_VS_FMT_FLOAT"),
    5: ("D9_PICA_VS_ATTR_TEXCOORD", "D9_DECL_FLOAT2", 2,
        "D9_PICA_VS_FMT_FLOAT"),
    10: ("D9_PICA_VS_ATTR_COLOR", "D9_DECL_UBYTE4N", 4,
         "D9_PICA_VS_FMT_UBYTE_NORM"),
}


def runtime_candidates(doc: dict) -> list:
    """Exact contracts whose inputs/outputs the generic runtime loader carries.

    ``d9_pica_vs_declare`` matches D3D usage index 0 only, so a second
    texcoord is not runtime-expressible today.  Normals/tangents are not in
    the ``D9PicaVsAttr`` enum either.
    """
    out = []
    for entry in doc.get("entries", []):
        if entry.get("kind") != "pica_vs":
            continue
        if entry.get("codegen") != "emitted-verified":
            continue
        if entry.get("quality") != "exact":
            continue
        if entry.get("id", "").startswith("menu."):
            continue
        usages = {item["usage"] for item in entry.get("inputs", [])}
        texcoords = {item["usage_index"] for item in entry.get("outputs", [])
                     if item["usage"] == 5}
        colors = {item["usage_index"] for item in entry.get("outputs", [])
                  if item["usage"] == 10}
        if usages <= {0, 5, 10} and texcoords <= {0} and colors <= {0}:
            out.append(entry)
    return out


def emit_runtime(pack: str = "all", write: bool = True) -> dict:
    """Write one C table for every runtime-expressible exact contract."""
    packs = sorted(PREFIX) if pack == "all" else [pack]
    docs = {}
    chosen = []
    for name in packs:
        plans_path = os.path.join(pack_dir(name), "plans.json")
        with open(plans_path, encoding="utf-8") as handle:
            docs[name] = (plans_path, json.load(handle))
        chosen.extend((name, entry) for entry in
                      runtime_candidates(docs[name][1]))
    table = [
        "/* Generated by tools/vs_contract_emit.py emit-runtime.",
        " * Exact non-skinned PICA VS with the position/texcoord0/color "
        "identity bank.",
        " * Draw is gated by exact hash + declaration + uniform mask. */",
        "static const D9PicaVsProgram d9_pica_vs_contract_entries[] = {",
    ]
    blobs = [
        "/* Generated by tools/vs_contract_emit.py emit-runtime.",
        " * Embedded contract .shbin bytes.  Do not edit. */",
        "",
    ]
    for name, entry in chosen:
        emitter = entry["emitter"]
        path = os.path.join(pack_dir(name), emitter["shbin"])
        with open(path, "rb") as handle:
            blob = handle.read()
        if hashlib.sha256(blob).hexdigest() != emitter["shbin_sha256"]:
            raise Refusal("pinned_shbin_mismatch_%s" % entry["id"])
        symbol = "d9_pica_vs_contract_shbin_%s" % entry["id"].rsplit(
            ".", 1)[-1]
        table += [
            "    {",
            '        "%s",' % entry["id"],
            '        "%s",' % entry["sha256"],
            '        "%s",' % os.path.basename(entry["emitted_asm"]),
            '        "%s",' % os.path.basename(emitter["shbin"]),
            '        "%s",' % emitter["shbin_sha256"],
            "        %s, %u," % (symbol, len(blob)),
            "        %uu, %uu, 0u, 0u, %uu," % (
                emitter["instructions"], emitter["uniform_rows"],
                emitter["uniform_rows"]),
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
                "0x%02x" % byte for byte in blob[start:start + 16]) + ",")
        blobs += ["};", ""]
        entry["runtime"] = 1
        entry["draw_enabled"] = 1
        entry["note"] = (
            "PICA VS emitted and numerically verified; runtime-enabled for "
            "the position/texcoord0/color identity-bank contract "
            "(%d samples, max |dpos| %.1e, picasso pass)"
            % (emitter["samples"], emitter["max_abs_position"]))
    table += ["};",
              "#define D9_PICA_VS_CONTRACT_ENTRY_COUNT "
              "(sizeof(d9_pica_vs_contract_entries)/"
              "sizeof(d9_pica_vs_contract_entries[0]))", ""]
    if write:
        with open(os.path.join(REPO, "source",
                               "d3d9_pica_vs_contract_table.inc"),
                  "w", encoding="ascii", newline="\n") as handle:
            handle.write("\n".join(table) + "\n")
        with open(os.path.join(REPO, "source",
                               "d3d9_pica_vs_contract_shbin.inc"),
                  "w", encoding="ascii", newline="\n") as handle:
            handle.write("\n".join(blobs) + "\n")
        for name in packs:
            path, doc = docs[name]
            raw = open(path, "rb").read()
            newline = "\r\n" if b"\r\n" in raw else "\n"
            with open(path, "w", encoding="utf-8", newline=newline) as handle:
                json.dump(doc, handle, indent=1, sort_keys=(name == "ui"))
                handle.write("\n")
    return {"pack": pack, "runtime": len(chosen),
            "ids": [entry["id"] for _, entry in chosen]}


def generate(pack: str, data_dir: str = "", write: bool = True) -> dict:
    hashes = contract_hashes(pack)
    irs = load_irs(hashes, data_dir)
    plans_path = os.path.join(pack_dir(pack), "plans.json")
    with open(plans_path, encoding="utf-8") as handle:
        doc = json.load(handle)
    foreign = [entry for entry in doc.get("entries", [])
               if entry.get("sha256") not in hashes]
    ours = []
    results = []
    generated = os.path.join(pack_dir(pack), "generated")
    if write:
        os.makedirs(generated, exist_ok=True)
    for sha in sorted(hashes):
        ir, package, key = irs[sha]
        shbin = None
        try:
            entry, shbin = emit_contract(pack, sha, ir, package, key)
        except Refusal as exc:
            entry = {
                "id": _plan_id(pack, sha), "sha256": sha, "sha1": ir.sha1,
                "version": "0x%08x" % ir.version, "stage": "vs_3_0",
                "kind": "pica_vs", "family": pack,
                "inputs": C.plan_vs(ir)[0]["inputs"],
                "outputs": lower_varyings(C.plan_vs(ir)[0])["outputs"],
                "instructions": len([ins for ins in ir.body
                                     if ins.name not in ("def", "nop")]),
                "uniform_registers": 0, "codegen": "refused",
                "quality": "exact", "draw_enabled": 0, "runtime": 0,
                "refusal": str(exc),
                "note": "PICA VS named refusal (%s)" % exc,
                "preconditions": ["pica_vs_emit_request", str(exc)],
                "source_package": package, "source_key": key,
            }
        ours.append(entry)
        if shbin is not None and write:
            body = [ins for ins in ir.body if ins.name not in ("def", "nop")]
            relative = sorted({src.index for ins in body for src in ins.srcs
                               if src.reg_type == asm.REG_CONST
                               and src.relative})
            palette_rows = (max(relative) + 1) if relative else 0
            os.makedirs(generated, exist_ok=True)
            with open(os.path.join(generated, "%s.v.pica" % entry["id"]),
                      "w", encoding="ascii", newline="\n") as handle:
                handle.write(S.emit(
                    dict(C.plan_vs(ir)[0], id=entry["id"],
                         outputs=entry["outputs"],
                         dropped_output_regs=entry.get(
                             "dropped_output_regs", []),
                         uniform_registers=entry.get("uniform_registers", 0)),
                    ir, bones=0, identity_consts=palette_rows == 0,
                    palette_rows=palette_rows))
            with open(os.path.join(generated, "%s.shbin" % entry["id"]),
                      "wb") as handle:
                handle.write(shbin)
        results.append({
            "id": entry["id"], "sha256": sha,
            "status": "pass" if entry["codegen"] == "emitted-verified"
                      else "refused",
            "quality": entry.get("quality"),
            "max_abs_position": entry.get("emitter", {}).get(
                "max_abs_position"),
            "max_abs_varying": entry.get("emitter", {}).get(
                "max_abs_varying"),
            "picasso": entry.get("emitter", {}).get("picasso"),
            "reason": entry.get("refusal"),
        })
    doc["entries"] = foreign + ours
    # A committed wide-runtime row (tools/vs_wide_emit.py) carries its own
    # rank-mapped emitter proof; corpus regeneration must not roll it back.
    committed = {}
    if os.path.isfile(plans_path):
        with open(plans_path, encoding="utf-8") as handle:
            committed = {e["id"]: e for e in json.load(handle)["entries"]
                         if e.get("runtime") == 1
                         and e.get("codegen") == "emitted-verified"}
    doc["entries"] = [committed.get(e.get("id"), e)
                      for e in doc["entries"]]
    if write:
        with open(plans_path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(doc, handle, indent=1, sort_keys=(pack == "ui"))
            handle.write("\n")
    report = {
        "schema": "pica-vs-contracts/1", "pack": pack,
        "samples": SAMPLES, "tolerance": TOLERANCE,
        "contracts": len(hashes),
        "emitted": sum(1 for r in results if r["status"] == "pass"),
        "refused": sum(1 for r in results if r["status"] == "refused"),
        "exact": sum(1 for r in results if r.get("quality") == "exact"
                     and r["status"] == "pass"),
        "approx": sum(1 for r in results if r.get("quality") == "approx"
                      and r["status"] == "pass"),
        "entries": results,
    }
    if write:
        with open(os.path.join(pack_dir(pack), "vs_contracts_report.json"),
                  "w", encoding="utf-8", newline="\n") as handle:
            json.dump(report, handle, indent=1, sort_keys=True)
            handle.write("\n")
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack", choices=sorted(PREFIX),
                        required=True)
    parser.add_argument("--data", default="")
    parser.add_argument("--no-write", action="store_true")
    parser.add_argument("--emit-runtime", action="store_true",
                        help="promote exact runtime-expressible plans and "
                             "write the C table")
    args = parser.parse_args(argv)
    if args.emit_runtime:
        print(json.dumps(emit_runtime("all", write=not args.no_write),
                         indent=1))
        return 0
    report = generate(args.pack, args.data, write=not args.no_write)
    print(json.dumps({k: report[k] for k in
                      ("pack", "contracts", "emitted", "refused", "exact",
                       "approx")}, indent=1))
    for row in report["entries"]:
        if row["status"] != "pass":
            print("REFUSED %s %s" % (row["id"], row.get("reason")))
    return 0 if report["emitted"] == report["contracts"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refusal as exc:
        print("REFUSED: %s" % exc, file=sys.stderr)
        raise SystemExit(2)
