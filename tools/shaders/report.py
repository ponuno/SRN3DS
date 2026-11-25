"""CLI: shader-family report, main-menu subset and TEV chunk dump.

    set PYTHONPATH=tools
    .venv\\Scripts\\python.exe -m shaders.report all

Writes derived JSON under ``E:\\Skyrim_N3DS_build\\assets\\shaders`` and prints
the tables quoted in ``docs/SHADERS.md``.  Nothing is written into the
repository and no game bytes are copied: the optional chunk dump is a local
build artefact used by ``tests/tev_real_smoke.c``.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
from collections import Counter
from typing import Dict, List, Optional, Sequence

try:
    from skyrim import bsa, nif, shaders
except ImportError:  # running from the repository root
    from tools.skyrim import bsa, nif, shaders  # type: ignore

from . import asm, packages

DEFAULT_DATA = r"E:\SteamLibrary\steamapps\common\Skyrim\Data"
DEFAULT_OUT = r"E:\Skyrim_N3DS_build\assets\shaders"


def _write_json(path: str, value) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, sort_keys=False)
        handle.write("\n")


def _package_rows(records: Sequence[packages.ChunkRecord]) -> List[dict]:
    rows = packages.package_summary(records)
    for row in rows:
        row["chunk_kind_histogram"] = {
            "ps": row["ps"], "vs": row["vs"],
        }
    return rows


def families_report(data_dir: str, out_dir: str,
                    include_update: bool = False) -> dict:
    all_packages = packages.load_packages(data_dir, include_update=include_update)
    effective = packages.effective_packages(all_packages)
    records = packages.build_records(effective)
    report = {
        "generated_for": data_dir,
        "include_update": include_update,
        "archives_read": ["Skyrim - Shaders.bsa"] + (["Update.bsa"] if include_update else []),
        "packages_read": len(all_packages),
        "packages_effective": len(effective),
        "dedup": packages.dedup_summary(records),
        "families": packages.family_summary(records),
        "family_rules": [{"family": family, "rule": rule}
                         for family, rule in packages.FAMILY_RULES],
        "packages": _package_rows(records),
        "tev_python_screen": packages.feasibility_table(records),
        "tev_python_screen_note": (
            "Conservative opcode/stage screen, not the C translator. The "
            "measured acceptance of source/d3d9_tev.c per family is in "
            "docs/SHADERS.md section 6 and is produced by tests/tev_real_smoke.c."),
    }
    _write_json(os.path.join(out_dir, "shader_families.json"), report)
    return report


def _read_nif(data_dir: str, path: str) -> Optional[dict]:
    archive_path = os.path.join(data_dir, "Skyrim - Meshes.bsa")
    if not os.path.exists(archive_path):
        return None
    archive = bsa.BsaArchive(archive_path)
    try:
        data = archive.read_file(path)
    except Exception:
        return None
    f = nif.parse_nif(data)
    shader_props = []
    for block in f.blocks:
        if block.type_name == "BSLightingShaderProperty" and block.size >= 24:
            raw = block.slice(data)
            flags1, flags2 = struct.unpack_from("<II", raw, 16)
            shader_props.append({"type": "lighting", "flags1": flags1, "flags2": flags2})
        elif block.type_name == "BSEffectShaderProperty":
            shader_props.append({"type": "effect", "flags1": None, "flags2": None})
    return {
        "path": path,
        "file_bytes": len(data),
        "block_histogram": f.histogram(),
        "textures": f.texture_paths(),
        "shader_properties": shader_props,
        "geometry": f.geometry_stats()["totals"],
    }


def _base_lit_group(records: Sequence[packages.ChunkRecord]) -> List[dict]:
    """Packages whose pixel CTAB is the unshadowed base lit vocabulary."""
    base_names = {
        "DiffuseSampler", "NormalSampler", "DirLightColor", "DirLightDirection",
        "DirectionalAmbient", "EmitColor", "FogColor", "MaterialData",
    }
    rows = []
    by_package: Dict[str, List[packages.ChunkRecord]] = {}
    for record in records:
        by_package.setdefault(record.package.path, []).append(record)
    for path, subset in by_package.items():
        ps = [r for r in subset if r.chunk.kind == "ps"]
        if not ps:
            continue
        if not all(base_names <= r.constants for r in ps):
            continue
        extra = set().union(*(r.constants for r in ps)) - base_names
        simplest = min(ps, key=lambda r: (r.features["instructions"], r.chunk.size))
        rows.append({
            "path": path,
            "file_bytes": ps[0].package.size,
            "chunks": len(ps),
            "distinct_instruction_streams": len({r.program_sha1 for r in ps}),
            "extra_constants": sorted(extra),
            "simplest_chunk": {
                "offset": simplest.chunk.offset,
                "bytes": simplest.chunk.size,
                "instructions": simplest.features["instructions"],
                "texture_samples": simplest.features["texture_samples"],
                "branch": simplest.features["branches"],
                "sha1": simplest.bytecode_sha1,
            },
        })
    rows.sort(key=lambda row: (len(row["extra_constants"]), row["file_bytes"]))
    return rows


def menu_report(data_dir: str, out_dir: str) -> dict:
    all_packages = packages.load_packages(data_dir, include_update=True)
    effective = packages.effective_packages(all_packages)
    records = packages.build_records(effective)
    base_records = records

    logo = _read_nif(data_dir, r"meshes\interface\logo\logo.nif")
    fog = _read_nif(data_dir, r"meshes\interface\intmenufogparticles.nif")

    by_name = {p.name.lower(): p for p in effective}

    def package_facts(name: str) -> Optional[dict]:
        package = by_name.get(name.lower())
        if not package:
            return None
        subset = [r for r in records if r.package.path == package.path]
        sizes = sorted(r.chunk.size for r in subset)
        return {
            "path": package.path,
            "name": package.name,
            "file_bytes": package.size,
            "chunks": len(subset),
            "vs": sum(1 for r in subset if r.chunk.kind == "vs"),
            "ps": sum(1 for r in subset if r.chunk.kind == "ps"),
            "distinct_instruction_streams": len({r.program_sha1 for r in subset}),
            "chunk_bytes_min": sizes[0] if sizes else 0,
            "chunk_bytes_median": sizes[len(sizes) // 2] if sizes else 0,
            "chunk_bytes_max": sizes[-1] if sizes else 0,
            "families": dict(Counter(r.family for r in subset)),
        }

    def pick(predicate) -> List[dict]:
        out = []
        for record in base_records:
            if record.chunk.kind != "ps":
                continue
            if predicate(record):
                out.append({
                    "package": record.package.name,
                    "offset": record.chunk.offset,
                    "bytes": record.chunk.size,
                    "instructions": record.features["instructions"],
                    "texture_samples": record.features["texture_samples"],
                    "branch": record.features["branches"],
                    "constants": sorted(record.constants),
                    "sha1": record.bytecode_sha1,
                })
        return out

    blits = pick(lambda r: (r.features["texture_samples"] == 1
                            and r.features["instructions"] <= 3
                            and not (r.constants & set(packages.LIGHTING_NAMES))))
    simplest_lit = pick(lambda r: ("FogColor" in r.constants
                                   and "DirLightColor" in r.constants
                                   and "NormalSampler" in r.constants
                                   and r.features["texture_samples"] == 2
                                   and not r.features["branches"]
                                   and not (r.constants & {
                                       "ShadowMaskSampler", "PointLightColor",
                                       "EnvSampler", "SpecularSampler",
                                       "SubSurfaceSampler", "BackLightMaskSampler",
                                   })))
    simplest_lit.sort(key=lambda row: (row["instructions"], row["bytes"]))

    report = {
        "evidence": {
            "engine_strings": [
                "shadersfx\\%s\\%03d\\V0%s.fxp",
                "shadersfx\\%s\\%03d\\P0%s.fxp",
                "SHADERSFX/Shaders%03d.fxp",
                "SHADERSFX/Imagespace%03d.fxp",
                "Interface/Logo/Logo.nif",
                "Interface/Logo/CameraPath",
                "fUIMistModel_* / fUIMistMenu_*",
            ],
            "logo_nif": logo,
            "menu_fog_nif": fog,
        },
        "packages": {
            "imagespace002": package_facts("imagespace002"),
            "shaders001": package_facts("shaders001"),
            "shaders002_update": package_facts("shaders002"),
            "p1": package_facts("p1"),
            "p1200005": package_facts("p1200005"),
            "p2400005": package_facts("p2400005"),
            "p2480005": package_facts("p2480005"),
        },
        "blit_programs": blits,
        "simplest_lit_programs": simplest_lit[:8],
        "base_lit_packages": _base_lit_group(base_records),
        "ranked_first": [
            {
                "rank": 1,
                "item": "fullscreen texture copy (imagespace002 / shaders001)",
                "why": "3-instruction ps_3_0 dcl,dcl,texld; the final composite and fade path",
                "bytes": min((row["bytes"] for row in blits), default=0),
                "tev": "1 stage, 1 texture, REPLACE",
            },
            {
                "rank": 2,
                "item": "base lit material (logo): diffuse + normal + dir light + fog",
                "why": "Interface/Logo/Logo.nif is a BSLightingShaderProperty with Logo02_d/n",
                "bytes": simplest_lit[0]["bytes"] if simplest_lit else 0,
                "tev": "3-5 stages after pre-specialisation; dp3 + fog + alpha test",
            },
            {
                "rank": 3,
                "item": "BSEffectShaderProperty menu fog (FXSteamThinAnim / GradSteamThin)",
                "why": "meshes/interface/intmenufogparticles.nif; no .fxp ships, engine-fixed",
                "bytes": 0,
                "tev": "texture * vertex colour, alpha blend; implement as a fixed pipeline",
            },
            {
                "rank": 4,
                "item": "UI flare / tinted pass (shaders001 FlarePS and BaseSampler-only)",
                "why": "menu logo flare and full-screen tint; 2 texld, <=24 instructions, no branch",
                "bytes": 340,
                "tev": "no lighting terms; 1-2 stages after folding",
            },
            {
                "rank": 5,
                "item": "shadow/point-light permutations",
                "why": "not needed by the menu scene: Logo.nif carries no shadow-mask material",
                "bytes": 0,
                "tev": "deferred; branches and shadow projection are outside the first family",
            },
        ],
    }
    _write_json(os.path.join(out_dir, "shader_menu.json"), report)
    return report


def dump_chunks(data_dir: str, out_path: str, include_update: bool = False,
                pixel_only: bool = True, family: str = "") -> int:
    all_packages = packages.load_packages(data_dir, include_update=include_update)
    effective = packages.effective_packages(all_packages)
    records = packages.build_records(effective)
    chunks = []
    for record in records:
        if pixel_only and record.chunk.kind != "ps":
            continue
        if family and record.family != family:
            continue
        chunks.append(record.chunk)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as handle:
        handle.write(struct.pack("<I", len(chunks)))
        for chunk in chunks:
            handle.write(struct.pack("<I", len(chunk.code)))
            handle.write(chunk.code)
    return len(chunks)


def print_families(report: dict) -> None:
    dedup = report["dedup"]
    print("packages: %d read / %d effective" %
          (report["packages_read"], report["packages_effective"]))
    print("chunks: %d  distinct bytecode: %d  distinct instruction streams: %d  "
          "distinct opcode sequences: %d" %
          (dedup["chunks"], dedup["distinct_bytecode"],
           dedup["distinct_instruction_streams"], dedup["distinct_opcode_sequences"]))
    print()
    print("%-28s %7s %6s %6s %8s %8s %6s" %
          ("family", "chunks", "ps", "vs", "streams", "opseq", "pkgs"))
    for family, row in report["families"].items():
        print("%-28s %7d %6d %6d %8d %8d %6d" %
              (family, row["chunks"], row["pixel_chunks"], row["vertex_chunks"],
               row["distinct_instruction_streams"], row["distinct_opcode_sequences"],
               row["packages"]))


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("part", choices=["all", "families", "menu", "dump", "summary"],
                        default="all", nargs="?")
    parser.add_argument("--data", default=DEFAULT_DATA)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--update", action="store_true",
                        help="include Update.bsa overrides (adds 1,726 chunks)")
    parser.add_argument("--dump-path", default="")
    parser.add_argument("--family", default="",
                        help="dump only one family (for per-family TEV measurements)")
    args = parser.parse_args(argv)

    if args.part in ("all", "families", "summary"):
        report = families_report(args.data, args.out, include_update=args.update)
        print_families(report)
    if args.part in ("all", "menu"):
        menu = menu_report(args.data, args.out)
        print()
        print("main-menu candidate packages:")
        for name, facts in menu["packages"].items():
            if facts:
                print("  %-24s %8d B  %4d chunks  %3d distinct" %
                      (name, facts["file_bytes"], facts["chunks"],
                       facts["distinct_instruction_streams"]))
        if menu["blit_programs"]:
            row = min(menu["blit_programs"], key=lambda item: item["bytes"])
            print("  simplest blit: %s +%d, %d B, %d instructions" %
                  (row["package"], row["offset"], row["bytes"], row["instructions"]))
        if menu["simplest_lit_programs"]:
            row = menu["simplest_lit_programs"][0]
            print("  simplest base-lit: %s +%d, %d B, %d instructions, %d texld" %
                  (row["package"], row["offset"], row["bytes"],
                   row["instructions"], row["texture_samples"]))
    if args.part == "dump":
        path = args.dump_path or os.path.join(args.out, "tev_chunks.bin")
        count = dump_chunks(args.data, path, include_update=args.update,
                            family=args.family)
        print("wrote %d pixel chunks to %s" % (count, path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
