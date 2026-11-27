#!/usr/bin/env python3
"""Emit, verify and promote the ``blit_post`` fullscreen-quad PICA VS family.

The blit/post pack is the largest single source of world chunks
(``blit.vs.c7ef0cd4`` alone is 38 chunks).  Every ``pica_vs`` row in the pack is
a straight-line ``vs_3_0`` program that transforms a fullscreen quad with two
``def`` literals and passes UV0 through.  This tool closes the family with the
same three-layer proof the skinning/lighting emitters use:

1. canonical vs_3_0 IR from the retail ``Skyrim - Shaders.bsa``;
2. picasso lowering to PICA200 with a real, non-empty ``.shbin``;
3. a deterministic numeric run (256 samples, ``max |dpos| <= 1e-4`` and every
   kept varying within the same bound) plus a picasso pass.

Only programs the *existing* generic runtime loader can bind are promoted to
``draw_enabled: 1`` / ``runtime: 1``: inputs position + texcoord0, one
``texcoord0`` varying, identity uniform bank, no dropped output.  A program
that writes ``texcoord1..`` or needs a remap stays ``draw_enabled: 0`` with a
note naming the missing loader capability -- it is still emitted and
numerically verified, never silently green.

Writes::

    source/shader_packs/blit_post/plans.json          (8 VS entries)
    source/shader_packs/blit_post/generated/*.v.pica  (picasso source)
    source/shader_packs/blit_post/generated/*.shbin   (pinned bytes)
    source/shader_packs/blit_post/blit_vs_report.json (numeric proof)
    source/d3d9_pica_vs_blit_shbin.inc                (embedded promoted bytes)
    source/d3d9_pica_vs_blit_table.inc                (exact-hash catalog rows)

Run::

    python tools\\blit_vs_emit.py --data E:\\SteamLibrary\\steamapps\\common\\Skyrim\\Data
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
import vs_contract_emit as V  # noqa: E402

PACK = "blit_post"
PACK_DIR = os.path.join(REPO, "source", "shader_packs", PACK)
PLANS = os.path.join(PACK_DIR, "plans.json")
MANIFEST = os.path.join(PACK_DIR, "manifest.json")
GENERATED = os.path.join(PACK_DIR, "generated")
REPORT = os.path.join(PACK_DIR, "blit_vs_report.json")
TABLE_INC = os.path.join(REPO, "source", "d3d9_pica_vs_blit_table.inc")
SHBIN_INC = os.path.join(REPO, "source", "d3d9_pica_vs_blit_shbin.inc")
SAMPLES = 256
PREFIX = "blit.vs"

#: D3D usage -> (PICA attribute, declared type, components, loader format).
RUNTIME_ATTR = {
    0: ("D9_PICA_VS_ATTR_POSITION", "D9_DECL_FLOAT3", 3,
        "D9_PICA_VS_FMT_FLOAT"),
    5: ("D9_PICA_VS_ATTR_TEXCOORD", "D9_DECL_FLOAT2", 2,
        "D9_PICA_VS_FMT_FLOAT"),
}


class Refusal(Exception):
    pass


def load_plans() -> dict:
    with open(PLANS, encoding="utf-8") as handle:
        return json.load(handle)


def find_irs(hashes, data_dir: str) -> Dict[str, Tuple[C.IR, str, str]]:
    from fxp_compile import _load_packages

    out = {}
    for package in _load_packages(V.find_data(data_dir)):
        for chunk in package.chunks:
            if chunk.kind != "vs":
                continue
            ir = C.build_ir(chunk.code)
            if ir.sha256 in hashes and ir.sha256 not in out:
                out[ir.sha256] = (ir, package.path, "0x%08x" % chunk.meta)
    missing = set(hashes) - set(out)
    if missing:
        raise Refusal("bytecode_for_%d_blit_vs_not_found" % len(missing))
    return out


def runtime_expressible(entry: dict) -> Tuple[bool, str]:
    """Can the existing generic loader bind this descriptor unchanged?"""
    for item in entry.get("inputs", []):
        if item["usage"] not in RUNTIME_ATTR:
            return False, ("input %s%d is outside the position/texcoord0 "
                           "loader" % (item["usage_name"],
                                       item["usage_index"]))
    texcoords = {o["usage_index"] for o in entry.get("outputs", [])
                 if o["usage"] == 5}
    colors = {o["usage_index"] for o in entry.get("outputs", [])
              if o["usage"] == 10}
    if texcoords - {0}:
        return False, ("writes texcoord%s; d9_pica_vs_declare binds texcoord0 "
                       "only" % ",".join(str(i) for i in sorted(texcoords - {0})))
    if colors:
        return False, "writes a colour varying the blit PS does not read"
    if entry.get("dropped_outputs") or entry.get("dropped_output_regs"):
        return False, "varying envelope drops a D3D output"
    return True, ""


def emit_program(entry: dict, ir: C.IR, package: str,
                 key: str) -> Tuple[dict, Optional[bytes]]:
    plan, _cls, why = C.plan_vs(ir)
    if plan is None:
        raise Refusal("plan_vs_%s" % why)
    lowered = V.lower_varyings(plan)
    dropped_regs = [[o["type"], o["reg_index"]] for o in lowered["dropped"]]
    abs_rows, uniform_rows = S._d3d_const_rows(ir, 0, True, 0)
    if uniform_rows > S.MAX_UNIFORMS:
        raise Refusal("uniform_rows_%d" % uniform_rows)

    out = dict(entry)
    out["codegen"] = "emitted-verified"
    out["quality"] = "exact" if not lowered["approx"] else "approx"
    out["inputs"] = plan["inputs"]
    out["outputs"] = lowered["outputs"]
    out["varying_map"] = lowered["varying_map"]
    out["dropped_output_regs"] = dropped_regs
    out["uniform_registers"] = uniform_rows
    out["uniform_map"] = "identity"
    out["constants"] = sorted({src.index for ins in ir.body for src in ins.srcs
                               if src.reg_type == asm.REG_CONST
                               and not src.relative and src.index not in ir.defs})
    out["emitted_asm"] = "generated/%s.v.pica" % entry["id"]
    out["source_package"] = package
    out["source_key"] = key
    if lowered["dropped"]:
        out["dropped_outputs"] = [
            {"usage": o["usage"], "usage_name": o["usage_name"],
             "usage_index": o["usage_index"]} for o in lowered["dropped"]]

    lowered_plan = dict(plan, id=entry["id"], outputs=lowered["outputs"],
                        uniform_registers=uniform_rows,
                        dropped_output_regs=dropped_regs)
    proof, shbin = S.verify(lowered_plan, ir, samples=SAMPLES, bones=0,
                            identity_consts=True, rel_tolerance=1e-6,
                            palette_rows=0, magnitude_limit=1e6)
    if proof["status"] != "pass" or not shbin:
        raise Refusal("numeric_verification_failed_%s" % proof["status"])
    words = int.from_bytes(shbin[0x18:0x1C], "little") if len(shbin) >= 0x1C \
        else proof["instructions"]
    out["emitter"] = {
        "samples": proof["samples"], "tolerance": proof["tolerance"],
        "rel_tolerance": proof["rel_tolerance"],
        "max_abs_position": proof["max_abs_position"],
        "max_abs_varying": proof["max_abs_varying"],
        "max_rel_varying": proof["max_rel_varying"],
        "magnitude_limit": proof["magnitude_limit"],
        "skipped_ill_conditioned": proof["skipped_ill_conditioned"],
        "picasso": proof["picasso"], "bones": 0,
        "uniform_rows": uniform_rows, "uniform_map": "identity",
        "instructions": words,
        "shbin": "generated/%s.shbin" % entry["id"],
        "shbin_sha256": proof["shbin_sha256"],
        "shbin_bytes": proof["shbin_bytes"],
        "inputs": [{"usage": i["usage"], "usage_name": i["usage_name"],
                    "reg_index": i["reg_index"]} for i in plan["inputs"]],
        "uniforms": [{"row": row, "d3d": row} for row in range(uniform_rows)],
        "varying_map": lowered["varying_map"],
    }
    return out, shbin


def emit_runtime(promoted: List[dict]) -> None:
    table = [
        "/* Generated by tools/blit_vs_emit.py emit-runtime.",
        " * Fullscreen-quad PICA VS: exact vs_3_0 SHA-256, identity uniform bank.",
        " * Draw is gated by exact hash + declaration + uniform mask. */",
        "static const D9PicaVsProgram d9_pica_vs_blit_entries[] = {",
    ]
    blobs = [
        "/* Generated by tools/blit_vs_emit.py emit-runtime.",
        " * Embedded fullscreen-quad .shbin bytes.  Do not edit. */",
        "",
    ]
    for entry in promoted:
        emitter = entry["emitter"]
        path = os.path.join(PACK_DIR, emitter["shbin"])
        with open(path, "rb") as handle:
            blob = handle.read()
        if hashlib.sha256(blob).hexdigest() != emitter["shbin_sha256"]:
            raise Refusal("pinned_shbin_mismatch_%s" % entry["id"])
        symbol = "d9_pica_vs_blit_shbin_%s" % entry["id"].rsplit(".", 1)[-1]
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
    table += ["};",
              "#define D9_PICA_VS_BLIT_ENTRY_COUNT "
              "(sizeof(d9_pica_vs_blit_entries)/"
              "sizeof(d9_pica_vs_blit_entries[0]))", ""]
    with open(TABLE_INC, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(table) + "\n")
    with open(SHBIN_INC, "w", encoding="ascii", newline="\n") as handle:
        handle.write("\n".join(blobs) + "\n")


def sync_manifest(doc: dict) -> None:
    """Keep the generated manifest rows and coverage counters in lock-step."""
    with open(MANIFEST, "rb") as handle:
        raw = handle.read()
    crlf = b"\r\n" in raw
    manifest = json.loads(raw.decode("utf-8"))
    by_sha = {e["sha256"]: e for e in doc.get("entries", [])}
    docs_path = os.path.join(REPO, "docs", "shader_plans.json")
    if os.path.isfile(docs_path):
        for entry in json.load(open(docs_path, encoding="utf-8")) \
                .get("entries", []):
            by_sha.setdefault(entry["sha256"], entry)
    for row in manifest.get("plans", []):
        entry = by_sha.get(row.get("sha256"))
        if entry is not None:
            row["status"] = ("draw-enabled" if entry.get("draw_enabled") == 1
                             else "translated")
    unique = manifest["unique_hashes"]
    census = {row[0] for row in manifest.get("hashes", [])}
    draw = sum(1 for sha in census
               if by_sha.get(sha, {}).get("draw_enabled") == 1)
    refused = sum(1 for sha in census if sha not in by_sha)
    coverage = manifest.setdefault("coverage", {})
    coverage["draw-enabled"] = draw
    coverage["translated"] = unique - draw - refused
    coverage["refused"] = refused
    coverage["contract_only"] = 0
    with open(MANIFEST, "w", encoding="utf-8",
              newline="\r\n" if crlf else "\n") as handle:
        json.dump(manifest, handle, indent=1)
        handle.write("\n")
    index_path = os.path.join(REPO, "source", "shader_packs", "index.json")
    if os.path.isfile(index_path):
        with open(index_path, "rb") as handle:
            raw = handle.read()
        index = json.loads(raw.decode("utf-8"))
        slot = index.get("packs", {}).get(manifest.get("pack"))
        if slot is not None:
            slot["coverage"] = dict(coverage)
            with open(index_path, "w", encoding="utf-8",
                      newline="\r\n" if b"\r\n" in raw else "\n") as handle:
                json.dump(index, handle, indent=1)
                handle.write("\n")


def main(argv=None) -> int:
    global SAMPLES
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default="")
    parser.add_argument("--samples", type=int, default=SAMPLES)
    parser.add_argument("--no-write", action="store_true")
    args = parser.parse_args(argv)
    SAMPLES = args.samples

    manifest = json.load(open(MANIFEST, encoding="utf-8"))
    vs_hashes = {row[0] for row in manifest["hashes"] if row[1] == "vs"}
    doc = load_plans()
    ours = [e for e in doc["entries"] if e.get("kind") == "pica_vs"]
    hashes = {e["sha256"] for e in ours}
    if not hashes <= vs_hashes:
        raise Refusal("plan_hash_outside_manifest")
    irs = find_irs(hashes, args.data)

    generated = GENERATED
    os.makedirs(generated, exist_ok=True)
    results = []
    promoted = []
    for entry in ours:
        sha = entry["sha256"]
        ir, package, key = irs[sha]
        try:
            new, shbin = emit_program(entry, ir, package, key)
        except Refusal as exc:
            entry.update({
                "codegen": "refused", "draw_enabled": 0, "runtime": 0,
                "refusal": str(exc),
                "note": "PICA VS named refusal (%s); no draw, no silent "
                        "approximation" % exc,
            })
            results.append({"id": entry["id"], "sha256": sha,
                            "status": "refused", "reason": str(exc)})
            continue
        ok, why = runtime_expressible(new)
        if ok:
            new["draw_enabled"] = 1
            new["runtime"] = 1
            new["preconditions"] = ["exact_hash_descriptor_match",
                                    "fullscreen_quad_semantics",
                                    "emitted_code_upload_path"]
            new["note"] = (
                "PICA VS emitted and numerically verified against the vs_3_0 "
                "IR (%d samples, max |dpos| %.1e, picasso pass, .shbin SHA-256 "
                "pinned); runtime-enabled: fullscreen quad, position + "
                "texcoord0, identity uniform bank"
                % (new["emitter"]["samples"],
                   new["emitter"]["max_abs_position"]))
            promoted.append(new)
        else:
            new["draw_enabled"] = 0
            new["runtime"] = 0
            new["preconditions"] = ["exact_hash_descriptor_match",
                                    "fullscreen_quad_semantics",
                                    "wider_varying_loader"]
            new["note"] = (
                "PICA VS emitted and numerically verified (%d samples, "
                "max |dpos| %.1e, picasso pass); draw disabled: %s"
                % (new["emitter"]["samples"],
                   new["emitter"]["max_abs_position"], why))
        entry.clear()
        entry.update(new)
        if not args.no_write:
            with open(os.path.join(generated, "%s.v.pica" % new["id"]),
                      "w", encoding="ascii", newline="\n") as handle:
                handle.write(S.emit(
                    dict(C.plan_vs(ir)[0], id=new["id"],
                         outputs=new["outputs"],
                         dropped_output_regs=new.get(
                             "dropped_output_regs", []),
                         uniform_registers=new.get("uniform_registers", 0)),
                    ir, bones=0, identity_consts=True, palette_rows=0))
            with open(os.path.join(generated, "%s.shbin" % new["id"]),
                      "wb") as handle:
                handle.write(shbin)
        results.append({
            "id": new["id"], "sha256": sha, "status": "pass",
            "draw_enabled": new["draw_enabled"],
            "max_abs_position": new["emitter"]["max_abs_position"],
            "max_abs_varying": new["emitter"]["max_abs_varying"],
            "picasso": new["emitter"]["picasso"],
            "shbin_bytes": new["emitter"]["shbin_bytes"],
            "shbin_sha256": new["emitter"]["shbin_sha256"],
            "reason": "" if ok else why,
        })

    if args.no_write:
        for row in results:
            print("%-22s %s draw=%s %s"
                  % (row["id"], row["status"], row.get("draw_enabled"),
                     row.get("reason", "")))
        return 0

    doc["note"] = (
        "pack catalog; draw_enabled=1 only for exact runtime descriptors (the "
        "one-stage TEV copy plus the six fullscreen-quad PICA VS the generic "
        "loader binds). Approximations carry quality=approx and an "
        "approximation field.")
    with open(PLANS, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(doc, handle, indent=1, sort_keys=True)
        handle.write("\n")
    sync_manifest(doc)

    emit_runtime(promoted)
    report = {
        "schema": "blit-vs-runtime/1", "pack": PACK,
        "samples": SAMPLES, "tolerance": S.TOLERANCE,
        "plans": len(ours), "promoted": len(promoted),
        "entries": results,
    }
    with open(REPORT, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, indent=1, sort_keys=True)
        handle.write("\n")
    print("blit_vs_emit: %d plans, %d runtime-enabled (%d samples)"
          % (len(ours), len(promoted), SAMPLES))
    for row in results:
        print("  %-22s draw=%d %s"
              % (row["id"], row.get("draw_enabled", 0), row.get("reason", "")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
