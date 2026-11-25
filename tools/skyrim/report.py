"""Command-line reports for the read-only Skyrim asset analysis.

Examples (run from the repository root)::

    .venv/Scripts/python.exe -m skyrim.report archives
    .venv/Scripts/python.exe -m skyrim.report textures
    .venv/Scripts/python.exe -m skyrim.report meshes
    .venv/Scripts/python.exe -m skyrim.report hkx
    .venv/Scripts/python.exe -m skyrim.report cell --cell Riverwood01
    .venv/Scripts/python.exe -m skyrim.report shaders
    .venv/Scripts/python.exe -m skyrim.report convert-demo
    .venv/Scripts/python.exe -m skyrim.report all

Requires ``PYTHONPATH=tools`` or an installed package.  All derived output is
written below ``--out-dir`` (default ``E:\\Skyrim_N3DS_build\\assets``); nothing
is written to the game installation.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from . import budget, convert, dds, esm, hkx, nif, shaders
from . import bsa

DEFAULT_DATA = r"E:\SteamLibrary\steamapps\common\Skyrim\Data"
DEFAULT_OUT = r"E:\Skyrim_N3DS_build\assets"

ARCHIVES = (
    "Skyrim - Textures.bsa",
    "Skyrim - Meshes.bsa",
    "Skyrim - Shaders.bsa",
    "Skyrim - Sounds.bsa",
    "Skyrim - Animations.bsa",
    "Skyrim - Interface.bsa",
    "Skyrim - Misc.bsa",
    "Skyrim - Voices.bsa",
    "Skyrim - VoicesExtra.bsa",
    "Update.bsa",
    "Dawnguard.bsa",
    "HearthFires.bsa",
    "Dragonborn.bsa",
)


def _write_json(path: str, value) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, sort_keys=False)
        handle.write("\n")


def _archive_path(data_dir: str, name: str) -> str:
    return os.path.join(data_dir, name)


def report_archives(data_dir: str, out_dir: str) -> dict:
    results = {}
    for name in ARCHIVES:
        path = _archive_path(data_dir, name)
        if not os.path.isfile(path):
            continue
        with bsa.open_archive(path) as archive:
            stat = archive.stat()
            results[name] = {
                "file_size": stat["file_size"],
                "header": stat["header"],
                "folders": stat["folder_count"],
                "files": stat["file_count"],
                "stored_file_bytes": stat["stored_file_bytes"],
                "extensions": stat["extensions"],
            }
            print(
                "%-28s v%d files=%6d folders=%5d flags=0x%04x compressed=%s embed=%s"
                % (
                    name,
                    stat["header"]["version"],
                    stat["file_count"],
                    stat["folder_count"],
                    stat["header"]["archive_flags"],
                    stat["header"]["compressed"],
                    stat["header"]["embedded_names"],
                )
            )
    _write_json(os.path.join(out_dir, "archives", "bsa_statistics.json"), results)
    return results


# ---------------------------------------------------------------------------
# Textures
# ---------------------------------------------------------------------------

SUFFIXES = (
    "_msn", "_nrm", "_n", "_s", "_e", "_m", "_g", "_sk", "_b", "_p", "_d",
    "_em", "_rm", "_rma", "_ao", "_diffuse", "_normal", "_specular", "_glow",
    "_parallax", "_height", "_mask", "_detail",
)


def texture_set_key(path: str) -> str:
    folder, _, name = path.rpartition("\\")
    stem = name.rsplit(".", 1)[0].lower()
    changed = True
    while changed:
        changed = False
        for suffix in SUFFIXES:
            if stem.endswith(suffix) and len(stem) > len(suffix):
                stem = stem[: -len(suffix)]
                changed = True
                break
    return (folder.lower() + "\\" + stem) if folder else stem


def scan_textures(archive: bsa.BsaArchive) -> Tuple[List[dict], dict]:
    rows: List[dict] = []
    for entry in archive.list_files():
        if entry.extension != "dds":
            continue
        try:
            prefix = archive.read_prefix(entry.path, 256)
            info = dds.parse_dds(prefix, require_payload=False)
            rows.append(
                {
                    "path": entry.path,
                    "stored_bytes": entry.size,
                    "width": info.width,
                    "height": info.height,
                    "mips": info.mip_count,
                    "format": info.format_name,
                    "kind": info.kind,
                    "bc_bytes": dds.bc_mipped_size(
                        info.width, info.height, info.kind, info.mip_count
                    )
                    if info.is_block_compressed
                    else info.all_mips_size,
                    "etc1_bytes": dds.etc1_mipped_size(info.width, info.height, info.mip_count),
                    "etc1a4_bytes": dds.etc1a4_mipped_size(
                        info.width, info.height, info.mip_count
                    ),
                    "needs_alpha": info.format_name in ("DXT3", "DXT5", "ATI2/BC5"),
                    "set": texture_set_key(entry.path),
                }
            )
        except Exception as exc:  # noqa: BLE001
            rows.append({"path": entry.path, "error": "%s: %s" % (type(exc).__name__, exc)})
    return rows, {}


def report_textures(data_dir: str, out_dir: str, top: Sequence[int] = (10, 25, 50)) -> dict:
    path = _archive_path(data_dir, "Skyrim - Textures.bsa")
    with bsa.open_archive(path) as archive:
        rows, _ = scan_textures(archive)
    errors = [r for r in rows if "error" in r]
    good = [r for r in rows if "error" not in r]
    by_format = Counter(r["format"] for r in good)
    by_dim = Counter("%dx%d" % (r["width"], r["height"]) for r in good)
    by_mips = Counter(str(r["mips"]) for r in good)
    total_bc = sum(r["bc_bytes"] for r in good)
    total_etc1 = sum(r["etc1_bytes"] for r in good)
    total_etc1a4 = sum(r["etc1a4_bytes"] for r in good)
    mixed_etc1 = sum(
        (r["etc1a4_bytes"] if r["needs_alpha"] else r["etc1_bytes"]) for r in good
    )
    per_format: Dict[str, dict] = {}
    for row in good:
        bucket = per_format.setdefault(
            row["format"],
            {"count": 0, "bc_bytes": 0, "etc1_bytes": 0, "etc1a4_bytes": 0,
             "needs_alpha": row["needs_alpha"]},
        )
        bucket["count"] += 1
        bucket["bc_bytes"] += row["bc_bytes"]
        bucket["etc1_bytes"] += row["etc1_bytes"]
        bucket["etc1a4_bytes"] += row["etc1a4_bytes"]
    bc_sizes = sorted(r["bc_bytes"] for r in good)
    etc_sizes = sorted(r["etc1_bytes"] for r in good)
    def _median(values: List[int]) -> int:
        return values[len(values) // 2] if values else 0
    sets: Dict[str, dict] = {}
    for row in good:
        bucket = sets.setdefault(
            row["set"],
            {"set": row["set"], "files": 0, "bc_bytes": 0, "etc1_bytes": 0,
             "etc1a4_bytes": 0, "max_width": 0, "max_height": 0},
        )
        bucket["files"] += 1
        bucket["bc_bytes"] += row["bc_bytes"]
        bucket["etc1_bytes"] += row["etc1_bytes"]
        bucket["etc1a4_bytes"] += row["etc1a4_bytes"]
        bucket["max_width"] = max(bucket["max_width"], row["width"])
        bucket["max_height"] = max(bucket["max_height"], row["height"])
    ranked = sorted(sets.values(), key=lambda item: -item["bc_bytes"])
    top_sets = {}
    for count in top:
        chunk = ranked[:count]
        top_sets[str(count)] = {
            "sets": count,
            "bc_bytes": sum(item["bc_bytes"] for item in chunk),
            "etc1_bytes": sum(item["etc1_bytes"] for item in chunk),
            "etc1a4_bytes": sum(item["etc1a4_bytes"] for item in chunk),
            "bc_MiB": round(sum(item["bc_bytes"] for item in chunk) / (1024 * 1024), 3),
            "etc1_MiB": round(sum(item["etc1_bytes"] for item in chunk) / (1024 * 1024), 3),
        }
    summary = {
        "dds_count": len(good),
        "errors": len(errors),
        "error_examples": errors[:10],
        "total_bc_bytes": total_bc,
        "total_bc_MiB": round(total_bc / (1024 * 1024), 2),
        "total_etc1_bytes": total_etc1,
        "total_etc1_MiB": round(total_etc1 / (1024 * 1024), 2),
        "total_etc1a4_bytes": total_etc1a4,
        "total_etc1_mixed_bytes": mixed_etc1,
        "etc1_to_bc_ratio": round(total_etc1 / total_bc, 4) if total_bc else 0,
        "etc1_mixed_to_bc_ratio": round(mixed_etc1 / total_bc, 4) if total_bc else 0,
        "per_format": {
            fmt: {
                **values,
                "bc_MiB": round(values["bc_bytes"] / (1024 * 1024), 2),
                "etc1_MiB": round(values["etc1_bytes"] / (1024 * 1024), 2),
            }
            for fmt, values in sorted(per_format.items(), key=lambda kv: -kv[1]["count"])
        },
        "median_bc_bytes": _median(bc_sizes),
        "median_etc1_bytes": _median(etc_sizes),
        "formats": dict(by_format.most_common()),
        "dimensions": dict(by_dim.most_common(30)),
        "mip_counts": dict(sorted(by_mips.items(), key=lambda kv: int(kv[0]))),
        "texture_sets": len(sets),
        "top_sets": top_sets,
        "vram": budget.vram_summary(),
        "largest_textures": sorted(
            ({"path": r["path"], "width": r["width"], "height": r["height"],
              "format": r["format"], "etc1_bytes": r["etc1_bytes"]}
             for r in good),
            key=lambda item: -item["etc1_bytes"],
        )[:20],
        "top_texture_sets": ranked[:50],
    }
    _write_json(os.path.join(out_dir, "textures", "texture_inventory.json"), summary)
    _write_json(os.path.join(out_dir, "textures", "textures.json"), good)
    print(
        "textures: %d DDS, BC %.1f MiB, ETC1 %.1f MiB, mixed %.1f MiB, ratios %.3f/%.3f, sets %d"
        % (
            len(good),
            total_bc / (1024 * 1024),
            total_etc1 / (1024 * 1024),
            mixed_etc1 / (1024 * 1024),
            total_etc1 / total_bc if total_bc else 0,
            mixed_etc1 / total_bc if total_bc else 0,
            len(sets),
        )
    )
    print("formats:", dict(by_format.most_common(8)))
    print("dimensions:", dict(by_dim.most_common(8)))
    for count, item in top_sets.items():
        print(
            "top %3s sets: BC %.2f MiB -> ETC1 %.2f MiB"
            % (count, item["bc_MiB"], item["etc1_MiB"])
        )
    return summary


# ---------------------------------------------------------------------------
# Meshes / NIF
# ---------------------------------------------------------------------------

def report_meshes(data_dir: str, out_dir: str, sample: int = 400) -> dict:
    path = _archive_path(data_dir, "Skyrim - Meshes.bsa")
    with bsa.open_archive(path) as archive:
        entries = [e for e in archive.list_files() if e.extension == "nif"]
        total_nifs = len(entries)
        hist: Counter = Counter()
        vertices = triangles = geo_bytes = payload = 0
        shapes = 0
        errors = 0
        sampled = 0
        unsupported: Counter = Counter()
        unresolved_shapes = 0
        for entry in entries[:sample]:
            try:
                data = archive.read_file(entry.path)
                parsed = nif.parse_nif(data)
                hist.update(parsed.histogram())
                geo = parsed.geometry_stats()
                vertices += geo["totals"]["vertices"]
                triangles += geo["totals"]["triangles"]
                shapes += geo["totals"]["shapes"]
                payload += len(data)
                sampled += 1
                for name, count in geo.get("unsupported_blocks", {}).items():
                    if count:
                        unsupported[name] += count
                for shape in geo["shapes"]:
                    if not shape.get("supported", True) and shape.get("data") not in nif.UNSUPPORTED_GEOMETRY_TYPES:
                        unresolved_shapes += 1
            except Exception:
                errors += 1
    geo_bytes = vertices * budget.BYTES_PER_VERTEX + triangles * 3 * budget.BYTES_PER_INDEX
    summary = {
        "nif_count": total_nifs,
        "sampled": sampled,
        "errors": errors,
        "block_type_histogram": dict(hist.most_common(60)),
        "sampled_vertices": vertices,
        "sampled_triangles": triangles,
        "sampled_shapes": shapes,
        "sampled_nif_payload_bytes": payload,
        "estimated_geometry_bytes": geo_bytes,
        "bytes_per_vertex": budget.BYTES_PER_VERTEX,
        "bytes_per_index": budget.BYTES_PER_INDEX,
        "unsupported_geometry": dict(unsupported),
        "unresolved_shape_refs": unresolved_shapes,
        "notes": [
            "Only NiTriShapeData counts are decoded; NiTriStripsData is counted as unsupported.",
            "BSTriShape/BSSubIndexTriShape are Skyrim SE formats and do not occur in the LE archive.",
            "Texture paths are harvested by a printable-string scan, not a material decode.",
        ],
    }
    _write_json(os.path.join(out_dir, "meshes", "nif_summary.json"), summary)
    print(
        "meshes: %d NIFs, sampled %d, errors %d, vertices %d, triangles %d, %.2f MiB"
        % (total_nifs, sampled, errors, vertices, triangles, geo_bytes / (1024 * 1024))
    )
    print("top block types:", dict(hist.most_common(12)))
    return summary


def report_hkx(data_dir: str, out_dir: str, sample: int = 300) -> dict:
    rows = []
    errors = []
    scanned = 0
    for name in ("Skyrim - Animations.bsa", "Skyrim - Meshes.bsa", "Dawnguard.bsa",
                 "Dragonborn.bsa", "HearthFires.bsa"):
        path = _archive_path(data_dir, name)
        if not os.path.isfile(path):
            continue
        with bsa.open_archive(path) as archive:
            entries = [e for e in archive.list_files() if e.extension == "hkx"]
            for entry in entries[: max(1, sample // 5)]:
                scanned += 1
                try:
                    info = hkx.classify(archive.read_file(entry.path))
                    row = info.as_dict()
                    row["archive"] = name
                    row["path"] = entry.path
                    rows.append(row)
                except Exception as exc:  # noqa: BLE001
                    errors.append({"archive": name, "path": entry.path, "error": str(exc)})
    kinds = Counter(r["kind"] for r in rows)
    versions = Counter(r["contents_version"] for r in rows)
    layouts = Counter(r["layout_rules"] for r in rows)
    summary = {
        "scanned": scanned,
        "ok": len(rows),
        "errors": errors,
        "kinds": dict(kinds),
        "contents_versions": dict(versions),
        "layout_rules": dict(layouts),
        "samples": rows[:30],
        "note": "Header/classification only; the Havok object graph is not decoded.",
    }
    _write_json(os.path.join(out_dir, "meshes", "hkx_summary.json"), summary)
    print("hkx:", dict(kinds), dict(versions), dict(layouts))
    return summary


# ---------------------------------------------------------------------------
# Cell budget
# ---------------------------------------------------------------------------

def report_cell(
    data_dir: str,
    out_dir: str,
    cell_ids: Sequence[str] = ("Riverwood01", "Riverwood04"),
    plugin: str = "Skyrim.esm",
) -> dict:
    esm_path = os.path.join(data_dir, plugin)
    scan = esm.scan_plugin(esm_path)
    mesh_archive = bsa.open_archive(_archive_path(data_dir, "Skyrim - Meshes.bsa"))
    texture_archive = bsa.open_archive(_archive_path(data_dir, "Skyrim - Textures.bsa"))
    results = {}
    try:
        for cell_id in cell_ids:
            cell_files = esm.extract_cell(esm_path, editor_id=cell_id, scan=scan)
            working = budget.measure_archived_cell(cell_files, mesh_archive, texture_archive)
            report = working.assess()
            report["mesh_details"] = [m.as_dict() for m in working.mesh_stats]
            report["texture_details"] = [t.as_dict() for t in working.texture_stats]
            report["mesh_list"] = cell_files.mesh_paths
            report["cell_plugin"] = plugin
            results[cell_id] = report
            print(
                "%-14s refs=%3d meshes=%3d textures=%3d verts=%6d tris=%6d "
                "geo=%.2f MiB (base %.2f) tex(ETC1)=%.2f MiB"
                % (
                    cell_id,
                    report["refs"],
                    report["unique_meshes"],
                    report["unique_textures"],
                    report["vertices"],
                    report["triangles"],
                    report["geometry_MiB"],
                    report["base_geometry_MiB"],
                    report["texture_etc1_MiB"],
                )
            )
    finally:
        mesh_archive.close()
        texture_archive.close()
    _write_json(os.path.join(out_dir, "budget", "cell_budgets.json"), results)
    return results


# ---------------------------------------------------------------------------
# Shaders
# ---------------------------------------------------------------------------

def report_shaders(data_dir: str, out_dir: str) -> dict:
    path = _archive_path(data_dir, "Skyrim - Shaders.bsa")
    with bsa.open_archive(path) as archive:
        inventory = shaders.inventory_archive(archive)
    summary = inventory.summary()
    summary["files"] = inventory.files
    summary["source_files_list"] = inventory.source_files
    _write_json(os.path.join(out_dir, "shaders", "shader_inventory.json"), summary)
    _write_json(os.path.join(out_dir, "shaders", "shader_chunks.json"), inventory.rows())
    # CSV for humans
    csv_path = os.path.join(out_dir, "shaders", "shader_chunks.csv")
    os.makedirs(os.path.dirname(csv_path), exist_ok=True)
    with open(csv_path, "w", encoding="utf-8", newline="") as handle:
        import csv

        writer = csv.writer(handle)
        writer.writerow(
            ["source", "offset", "kind", "version", "size", "instructions",
             "comments", "has_ctab", "texture_samples", "branches", "loops",
             "calls", "mrt_writes", "port_subset_exact", "bytecode_sha1"]
        )
        for chunk in inventory.chunks:
            writer.writerow(
                [chunk.source, chunk.offset, chunk.kind, chunk.version_name,
                 chunk.size, chunk.instruction_count, chunk.comment_count,
                 chunk.has_ctab, chunk.texture_samples, chunk.branch_count,
                 chunk.loop_count, chunk.call_count, chunk.mrt_writes,
                 chunk.uses_port_subset, chunk.bytecode_sha1]
            )
    print(
        "shaders: %d files, %d chunks (%s), unique bytecode %d, subset %d (%.3f%%), sources %d"
        % (
            summary["file_count"],
            summary["chunk_count"],
            summary["versions"],
            summary["unique_bytecode"],
            summary["port_subset_exact"],
            100.0 * summary["port_subset_exact_fraction"],
            summary["source_files"],
        )
    )
    print("top opcodes:", list(summary["opcode_histogram"].items())[:15])
    return summary


# ---------------------------------------------------------------------------
# Texture conversion demo
# ---------------------------------------------------------------------------

def _pick_demo_textures(archive: bsa.BsaArchive, wanted: Sequence[Tuple[int, int, str]]) -> List[dict]:
    chosen = []
    used = set()
    for width, height, fmt in wanted:
        for entry in archive.list_files():
            if entry.extension != "dds" or entry.path in used:
                continue
            try:
                info = dds.parse_dds(archive.read_prefix(entry.path, 256), require_payload=False)
            except Exception:
                continue
            if (info.width, info.height, info.format_name) == (width, height, fmt):
                chosen.append({"entry": entry, "info": info})
                used.add(entry.path)
                break
    return chosen


def report_convert_demo(
    data_dir: str,
    out_dir: str,
    *,
    tex3ds: Optional[str] = None,
    limit: int = 4,
) -> dict:
    texture_dir = os.path.join(out_dir, "textures", "converted")
    path = _archive_path(data_dir, "Skyrim - Textures.bsa")
    wanted = [
        (512, 512, "DXT1"),
        (1024, 1024, "DXT1"),
        (512, 512, "DXT5"),
        (1024, 1024, "DXT5"),
    ]
    with bsa.open_archive(path) as archive:
        picks = _pick_demo_textures(archive, wanted)[:limit]
        results = []
        for pick in picks:
            entry = pick["entry"]
            data = archive.read_file(entry.path)
            fmt = "etc1a4" if pick["info"].format_name in ("DXT3", "DXT5") else "etc1"
            result = convert.convert_dds_file(data, entry.path, texture_dir, fmt=fmt, tex3ds=tex3ds)
            results.append(
                {
                    "name": entry.path,
                    "width": result.width,
                    "height": result.height,
                    "source_format": result.source_format,
                    "bc_bytes": result.bc_bytes,
                    "etc1_bytes": result.etc1_bytes,
                    "t3x_bytes": result.t3x_bytes,
                    "png_bytes": result.png_bytes,
                    "t3x_over_bc": round(result.t3x_bytes / result.bc_bytes, 4),
                    "t3x_over_etc1": round(result.t3x_bytes / result.etc1_bytes, 4),
                    "t3x_path": result.t3x_path,
                }
            )
            print(
                "%-58s %4dx%-4d %-5s BC %7d ETC %7d T3X %7d (%.3fx BC)"
                % (
                    entry.path,
                    result.width,
                    result.height,
                    result.source_format,
                    result.bc_bytes,
                    result.etc1_bytes,
                    result.t3x_bytes,
                    result.t3x_bytes / result.bc_bytes,
                )
            )
    summary = {
        "tex3ds": convert.find_tex3ds(tex3ds),
        "tex3ds_version": convert.tex3ds_version(tex3ds),
        "results": results,
        "note": "T3X container bytes vs top-level BC/ETC1 surface bytes; -z none, no generated mips.",
    }
    _write_json(os.path.join(out_dir, "textures", "conversion_demo.json"), summary)
    return summary


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["archives", "textures", "meshes", "hkx",
                                            "cell", "shaders", "convert-demo", "all"])
    parser.add_argument("--data-dir", default=os.environ.get("SKYRIM_N3DS_DATA", DEFAULT_DATA))
    parser.add_argument("--out-dir", default=os.environ.get("SKYRIM_N3DS_ASSETS", DEFAULT_OUT))
    parser.add_argument("--tex3ds", default=None)
    parser.add_argument("--cell", action="append", default=None)
    parser.add_argument("--sample", type=int, default=400)
    args = parser.parse_args(argv)

    if not os.path.isdir(args.data_dir):
        parser.error("data directory not found: %s" % args.data_dir)
    os.makedirs(args.out_dir, exist_ok=True)

    if args.command in ("archives", "all"):
        report_archives(args.data_dir, args.out_dir)
    if args.command in ("textures", "all"):
        report_textures(args.data_dir, args.out_dir)
    if args.command in ("meshes", "all"):
        report_meshes(args.data_dir, args.out_dir, sample=args.sample)
    if args.command in ("hkx", "all"):
        report_hkx(args.data_dir, args.out_dir, sample=min(args.sample, 300))
    if args.command in ("cell", "all"):
        report_cell(args.data_dir, args.out_dir, cell_ids=args.cell or ("Riverwood01", "Riverwood04"))
    if args.command in ("shaders", "all"):
        report_shaders(args.data_dir, args.out_dir)
    if args.command in ("convert-demo", "all"):
        report_convert_demo(args.data_dir, args.out_dir, tex3ds=args.tex3ds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
