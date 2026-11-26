"""Classify a Skyrim LE mod for the New 3DS port (Tier 1/2/3 + budget).

The scanner is read-only and bounded: it reads plugin headers, archive
directories, DDS headers and (optionally) whole small NIFs.  It never copies
game assets anywhere.

Tier 3 is a hard "does not run": SKSE DLLs, ESL/SE plugins, ENB, HDT/SMP,
Nemesis/FNIS.  Tier 2 needs the offline converter (downscale, ETC1/BC1, mesh
LOD, custom SWF).  Tier 1 is ESP/ESM + assets the existing pipeline handles.

Focus: **performance / simplification mods**.  The report separates edits
that remove work (objects, occlusion, grass, NPCs, particles) from "performance"
mods that quietly add Papyrus or SKSE requirements.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

try:
    from . import bsa, data_overlay, dds, nif
except ImportError:  # direct script execution
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from skyrim import bsa, data_overlay, dds, nif

VRAM_BUDGET = 4 * 1024 * 1024          # docs/MEMORY_BUDGET.md
GUEST_PLUGIN_WARN = 16 * 1024 * 1024   # plugin bytes read by the VM
BSA_INDEX_WARN = 8 * 1024 * 1024
PLUGIN_SCAN_BYTES = 32 * 1024 * 1024
MESH_PARSE_LIMIT = 4 * 1024 * 1024

TIER3_MARKERS = (
    b"skse", b"skse64", b"skse_1_", b"papyrusutil", b"jcontainers",
    b"enbseries", b"enblocal", b"enbseries.ini", b"d3d9.dll",
    b"hdtphysics", b"hdt-smp", b"hdtsmp", b"hdt.dll",
    b"nemesis", b"fnis",
)
TIER2_MARKERS = (b"skyui", b"uiextensions", b"mcm quest", b"_mcm")
OFFICIAL_PLUGINS = {"skyrim.esm", "update.esm", "dawnguard.esm",
                    "hearthfires.esm", "dragonborn.esm"}
PERFORMANCE_HINTS = {
    "removal": (b"remove", b"delete", b"disable", b"insignificant", b"cleanup"),
    "occlusion": (b"occlusion", b"occl", b"project optimization", b"spo"),
    "grass": (b"no grass", b"less grass", b"grass"),
    "npc": (b"no npc", b"less npc", b"greeting", b"dialogue"),
    "particles": (b"particle", b"smoke", b"fog", b"embers"),
    "lod": (b"lod", b"distant", b"distant detail"),
    "textures": (b"optimized", b"lowres", b"low-res", b"texture pack"),
}


@dataclass
class Limits:
    max_textures: int = 4000
    max_meshes: int = 500
    texture_cap: int = 256
    deep_mesh: bool = False


def _texture_bucket(width: int, height: int) -> str:
    side = max(width, height)
    if side <= 256:
        return "<=256"
    if side <= 512:
        return "512"
    if side <= 1024:
        return "1K"
    if side <= 2048:
        return "2K"
    return "4K+"


def _etc1_bytes(width: int, height: int, mips: int, cap: int,
                alpha: bool = False) -> int:
    w, h = min(width, cap), min(height, cap)
    total = 0
    for _ in range(max(1, mips)):
        if w < 8 or h < 8:
            break
        total += ((w + 3) // 4) * ((h + 3) // 4) * (16 if alpha else 8)
        w, h = max(1, w // 2), max(1, h // 2)
    return total


def _empty_textures() -> dict:
    return {
        "count": 0,
        "classified": 0,
        "formats": {},
        "buckets": {},
        "vram_etc1_cap": 0,
        "largest": [],
        "unreadable": 0,
        "truncated": False,
    }


def _record_texture(stats: dict, name: str, info, limits: Limits) -> None:
    stats["count"] += 1
    if stats["classified"] >= limits.max_textures:
        stats["truncated"] = True
        return
    stats["classified"] += 1
    bucket = _texture_bucket(info.width, info.height)
    stats["buckets"][bucket] = stats["buckets"].get(bucket, 0) + 1
    fmt = info.format_name
    stats["formats"][fmt] = stats["formats"].get(fmt, 0) + 1
    alpha = info.alpha_mask != 0 or info.format_name in ("DXT3", "DXT5", "BC5", "BC7")
    stats["vram_etc1_cap"] += _etc1_bytes(info.width, info.height,
                                          info.mip_count, limits.texture_cap, alpha)
    largest = stats["largest"]
    largest.append({"name": name, "width": info.width, "height": info.height,
                    "format": fmt, "mips": info.mip_count})
    largest.sort(key=lambda row: (row["width"] * row["height"], row["name"]),
                 reverse=True)
    del largest[8:]


def scan_plugin(path: str, *, limits: Optional[Limits] = None) -> dict:
    limits = limits or Limits()
    header = data_overlay.read_plugin_header(path)
    size = header.size
    with open(path, "rb") as handle:
        blob = handle.read(min(size, PLUGIN_SCAN_BYTES)).lower()
    scanned_all = size <= PLUGIN_SCAN_BYTES
    markers = {marker.decode("ascii"): blob.count(marker)
               for marker in TIER3_MARKERS + TIER2_MARKERS}
    vmad = blob.count(b"vmad")
    scripts = blob.count(b"scpt")
    hints = sorted(kind for kind, needles in PERFORMANCE_HINTS.items()
                   if any(needle in blob for needle in needles))

    tier = 1
    reasons: List[str] = []
    official = header.name.lower() in OFFICIAL_PLUGINS
    if header.is_light:
        tier = 3
        reasons.append("ESL/light master is SE-only")
    if header.form_version >= 43:
        tier = 3
        reasons.append("form version %d is SE, not LE" % header.form_version)
    skse = markers.get("skse", 0) + markers.get("skse64", 0) + \
        markers.get("skse_1_", 0)
    dll = markers.get("d3d9.dll", 0)
    if skse or dll:
        tier = 3
        reasons.append("SKSE/DLL marker (%d skse, %d dll)" % (skse, dll))
    for marker in ("enbseries", "enblocal", "enbseries.ini", "hdtphysics",
                   "hdt-smp", "hdtsmp", "hdt.dll", "nemesis", "fnis",
                   "papyrusutil", "jcontainers"):
        if markers.get(marker, 0):
            tier = 3
            reasons.append("%s marker" % marker.upper())
    if tier < 3 and (vmad >= 100 or scripts >= 50 or size > 8 * 1024 * 1024):
        tier = 2
        reasons.append("script-heavy (%d VMAD, %d SCPT, %d bytes)"
                       % (vmad, scripts, size))
    if tier < 3 and any(markers.get(m, 0) for m in
                        ("skyui", "uiextensions", "mcm quest", "_mcm")):
        tier = 2
        reasons.append("SkyUI/MCM/custom UI marker")
    if official:
        # The vanilla master always carries thousands of scripts; the port
        # must consume it, so its record volume is not a mod tier signal.
        tier = 1
        reasons = ["official master (script volume is baseline, not a mod)"]
    elif tier == 1:
        reasons.append("ESP/ESM only; no SKSE or heavy script marker")

    return {
        "name": header.name,
        "path": header.path,
        "size": size,
        "official": official,
        "scanned_bytes": len(blob),
        "scanned_all": scanned_all,
        "masters": header.masters,
        "is_master": header.is_master,
        "is_light": header.is_light,
        "form_version": header.form_version,
        "hed_version": header.hed_version,
        "record_count": header.record_count,
        "vmad_count": vmad,
        "scpt_count": scripts,
        "markers": {k: v for k, v in markers.items() if v},
        "performance_hints": hints,
        "tier": tier,
        "tier_reasons": reasons,
        "requires_skse": bool(skse or markers.get("papyrusutil")
                              or markers.get("jcontainers")),
    }


def scan_bsa(path: str, *, limits: Optional[Limits] = None) -> dict:
    limits = limits or Limits()
    stats = _empty_textures()
    meshes = {"count": 0, "bytes": 0, "triangles": 0, "parsed": 0,
              "unsupported": 0, "errors": 0}
    other = {"scripts": 0, "swf": 0, "ini": 0, "nif": 0}
    total_unpacked = 0
    with bsa.BsaArchive(path) as archive:
        header = archive.header
        for entry in archive.iter_files():
            lower = entry.path.lower()
            if lower.endswith(".dds"):
                if stats["classified"] < limits.max_textures:
                    try:
                        prefix = archive.read_prefix(entry.path, 148)
                        info = dds.parse_dds(prefix, require_payload=False)
                        _record_texture(stats, entry.path, info, limits)
                    except (bsa.BsaError, ValueError, IndexError):
                        stats["unreadable"] += 1
                else:
                    stats["count"] += 1
                    stats["truncated"] = True
                total_unpacked += entry.size
            elif lower.endswith(".nif"):
                meshes["count"] += 1
                meshes["bytes"] += entry.size
                other["nif"] += 1
                if limits.deep_mesh and meshes["parsed"] < limits.max_meshes \
                        and entry.size <= MESH_PARSE_LIMIT:
                    try:
                        raw = archive.read_file(entry.path)
                        parsed = nif.parse_nif(raw)
                        result = parsed.geometry_stats()
                        meshes["triangles"] += result["totals"]["triangles"]
                        meshes["parsed"] += 1
                    except Exception:
                        meshes["errors"] += 1
            elif lower.endswith((".pex", ".psc")):
                other["scripts"] += 1
            elif lower.endswith(".swf"):
                other["swf"] += 1
            elif lower.endswith((".ini", ".txt")):
                other["ini"] += 1
        header_info = header.as_dict()
        index_bytes = (24 * header_info["file_count"]
                       + header_info["total_folder_name_length"]
                       + header_info["total_file_name_length"])
    return {
        "path": os.path.abspath(path),
        "name": os.path.basename(path),
        "header": header_info,
        "index_bytes": index_bytes,
        "stored_bytes_sampled": total_unpacked,
        "textures": stats,
        "meshes": meshes,
        "other": other,
        "bsa_compressed": bool(header_info.get("compressed")),
        "bsa_version": header_info.get("version"),
    }


def scan_loose_dir(root: str, *, limits: Optional[Limits] = None) -> dict:
    limits = limits or Limits()
    stats = _empty_textures()
    meshes = {"count": 0, "bytes": 0, "triangles": 0, "parsed": 0,
              "unsupported": 0, "errors": 0}
    other = {"scripts": 0, "swf": 0, "ini": 0, "nif": 0}
    total_bytes = 0
    for base, dirs, files in os.walk(root):
        dirs.sort(key=str.lower)
        for name in sorted(files, key=str.lower):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, root)
            lower = name.lower()
            try:
                size = os.path.getsize(full)
            except OSError:
                continue
            total_bytes += size
            if lower.endswith(".dds"):
                try:
                    with open(full, "rb") as handle:
                        info = dds.parse_dds(handle.read(148),
                                             require_payload=False)
                    _record_texture(stats, rel, info, limits)
                except (ValueError, OSError, IndexError):
                    stats["unreadable"] += 1
            elif lower.endswith(".nif"):
                meshes["count"] += 1
                meshes["bytes"] += size
                other["nif"] += 1
                if limits.deep_mesh and meshes["parsed"] < limits.max_meshes \
                        and size <= MESH_PARSE_LIMIT:
                    try:
                        parsed = nif.parse_nif_file(full)
                        result = parsed.geometry_stats()
                        meshes["triangles"] += result["totals"]["triangles"]
                        meshes["parsed"] += 1
                    except Exception:
                        meshes["errors"] += 1
            elif lower.endswith((".pex", ".psc")):
                other["scripts"] += 1
            elif lower.endswith(".swf"):
                other["swf"] += 1
            elif lower.endswith((".ini", ".txt")):
                other["ini"] += 1
    return {"root": os.path.abspath(root), "bytes": total_bytes,
            "textures": stats, "meshes": meshes, "other": other}


def _budget(plugin_scans: Sequence[dict], archive_scans: Sequence[dict],
            loose_scans: Sequence[dict], cap: int) -> dict:
    plugin_bytes = sum(row["size"] for row in plugin_scans
                       if not row.get("official"))
    index_bytes = sum(row.get("index_bytes", 0) for row in archive_scans)
    vram = sum(row["textures"]["vram_etc1_cap"] for row in archive_scans) + \
        sum(row["textures"]["vram_etc1_cap"] for row in loose_scans)
    texture_count = sum(row["textures"]["count"] for row in archive_scans) + \
        sum(row["textures"]["count"] for row in loose_scans)
    warnings = []
    if vram > VRAM_BUDGET:
        warnings.append({
            "code": "vram-over",
            "message": "ETC1 at %dpx needs %.1f MiB; the texture budget is 4 MiB"
                       % (cap, vram / 1048576.0),
            "action": "downscale to 128px, drop unused textures, or stream cells",
        })
    if plugin_bytes > GUEST_PLUGIN_WARN:
        warnings.append({
            "code": "plugin-bytes",
            "message": "active plugins are %.1f MiB of guest address space"
                       % (plugin_bytes / 1048576.0),
            "action": "prefer a merged/simplified plugin; the loader reads only headers today",
        })
    if index_bytes > BSA_INDEX_WARN:
        warnings.append({
            "code": "bsa-index",
            "message": "BSA indexes need %.1f MiB" % (index_bytes / 1048576.0),
            "action": "merge archives or prune unused entries",
        })
    if texture_count > 2000:
        warnings.append({
            "code": "texture-count",
            "message": "%d textures in the mod" % texture_count,
            "action": "rely on the LRU/eviction path; do not preload the mod",
        })
    return {
        "plugin_bytes": plugin_bytes,
        "bsa_index_bytes": index_bytes,
        "texture_count": texture_count,
        "vram_etc1_bytes_at_cap": vram,
        "vram_budget": VRAM_BUDGET,
        "warnings": warnings,
    }


def scan_mod(data_root: Optional[str], *, plugins_path: Optional[str] = None,
             loadorder_path: Optional[str] = None,
             plugin: Optional[str] = None, bsa_path: Optional[str] = None,
             mod_dir: Optional[str] = None, auto_discover: bool = True,
             limits: Optional[Limits] = None) -> dict:
    limits = limits or Limits()
    result = {
        "data_root": os.path.abspath(data_root) if data_root else None,
        "plugins": [],
        "archives": [],
        "loose": [],
        "missing_masters": [],
    }
    if mod_dir:
        data = os.path.join(mod_dir, "Data")
        root = data if os.path.isdir(data) else mod_dir
        result["loose"].append(scan_loose_dir(root, limits=limits))
        for name in sorted(os.listdir(root), key=str.lower):
            if name.lower().endswith((".esp", ".esm", ".esl")):
                try:
                    result["plugins"].append(
                        scan_plugin(os.path.join(root, name), limits=limits))
                except data_overlay.OverlayError as exc:
                    result["missing_masters"].append(str(exc))
            elif name.lower().endswith(".bsa"):
                try:
                    result["archives"].append(
                        scan_bsa(os.path.join(root, name), limits=limits))
                except (bsa.BsaError, OSError) as exc:
                    result["missing_masters"].append(str(exc))
    else:
        overlay = data_overlay.DataOverlay(
            data_root, plugins_path=plugins_path,
            loadorder_path=loadorder_path, auto_discover=auto_discover)
        try:
            if plugin:
                overlay.load_order = [p for p in overlay.load_order
                                      if p.name.lower() == plugin.lower()]
                if not overlay.load_order:
                    raise data_overlay.MissingPluginError(
                        "plugin '%s' is not in the active load order" % plugin)
            for loaded in overlay.load_order:
                result["plugins"].append(scan_plugin(loaded.header.path,
                                                     limits=limits))
            for mount in overlay.archives:
                if not os.path.isfile(mount.host_path):
                    continue
                try:
                    result["archives"].append(scan_bsa(mount.host_path,
                                                       limits=limits))
                except (bsa.BsaError, OSError) as exc:
                    result["missing_masters"].append(
                        "%s: %s" % (mount.name, exc))
            result["overlay"] = overlay.manifest()
            if bsa_path:
                result["archives"].append(scan_bsa(bsa_path, limits=limits))
        finally:
            overlay.close()

    checks = result["plugins"]
    mod_checks = [row for row in checks if not row.get("official")] or checks
    tiers = [row["tier"] for row in mod_checks] or [1]
    result["tier"] = max(tiers)
    if any(row.get("requires_skse") for row in mod_checks):
        result["tier"] = 3
    result["tier_reasons"] = sorted({reason for row in mod_checks
                                     for reason in row["tier_reasons"]})
    result["budget"] = _budget(checks, result["archives"], result["loose"],
                               limits.texture_cap)
    result["performance_hints"] = sorted({hint for row in checks
                                          for hint in row["performance_hints"]})
    result["notes"] = [
        "Tier 3 is unsupported: no SKSE DLL loader, no ENB, no HDT/SMP, no Nemesis/FNIS.",
        "Tier 2 needs mod_optimize.py (texture downscale/ETC1, mesh LOD, SWF rewrite).",
        "The port reads DDS to estimate VRAM; the runtime samples ETC1 only.",
    ]
    return result


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default=None,
                        help="Data directory (default: TESV_DATA_ROOT)")
    parser.add_argument("--plugins", default=None, help="plugins.txt")
    parser.add_argument("--loadorder", default=None, help="loadorder.txt")
    parser.add_argument("--plugin", default=None, help="scan one plugin only")
    parser.add_argument("--bsa", default=None, help="extra BSA to scan")
    parser.add_argument("--mod-dir", default=None,
                        help="scan an unpacked mod dir (Data/ or mod root)")
    parser.add_argument("--texture-cap", type=int, default=256,
                        help="downscale target used for the VRAM estimate")
    parser.add_argument("--max-textures", type=int, default=4000)
    parser.add_argument("--max-meshes", type=int, default=500)
    parser.add_argument("--deep-mesh", action="store_true",
                        help="parse small NIFs for triangle counts")
    parser.add_argument("--json", default=None, help="write JSON here")
    args = parser.parse_args(argv)

    limits = Limits(max_textures=args.max_textures, max_meshes=args.max_meshes,
                    texture_cap=args.texture_cap, deep_mesh=args.deep_mesh)
    try:
        if args.mod_dir:
            report = scan_mod(None, mod_dir=args.mod_dir, limits=limits)
        else:
            data_root = data_overlay.resolve_data_root(args.data)
            report = scan_mod(data_root, plugins_path=args.plugins,
                              loadorder_path=args.loadorder,
                              plugin=args.plugin, bsa_path=args.bsa,
                              limits=limits)
    except data_overlay.OverlayError as exc:
        print("error[%s]: %s" % (exc.code, exc), file=sys.stderr)
        return 2
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
