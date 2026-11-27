#!/usr/bin/env python3
"""Read-only DDS format census for the retail corpus and installed mods.

The question this answers is narrower than the ETC1 pipeline inventory:
*which DDS pixel formats must the bridge recognise*, and how much of the
corpus is BC5 (ATI2/BC5U), BC7 (DX10 DXGI 98/99), uncompressed, A8/R8, or a
cube/volume resource.  Headers only are read; no pixel data is decoded and
nothing is written into the game installation.

Usage (PowerShell, repository root)::

    $env:PYTHONPATH='tools'
    .venv\\Scripts\\python.exe tools\\texture_formats.py ^
        --json E:\\Skyrim_N3DS_build\\assets\\textures\\format_census.json

Exit code 0 when the scan completed; 1 when a requested source is missing.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from skyrim import bsa  # noqa: E402

DEFAULT_DATA = r"E:\SteamLibrary\steamapps\common\Skyrim\Data"
# Archives that can carry textures.  Meshes/Sounds/Voices are skipped: they are
# large and there is no texture folder in them.
ARCHIVE_HINTS = ("textures", "update", "dawnguard", "hearthfires", "dragonborn")
DEFAULT_MOD_DIRS = (
    r"E:\SkyrimMods\mods",
    r"E:\Vortex Mods\skyrimse\mods",
)

DDPF_ALPHAPIXELS = 0x1
DDPF_FOURCC = 0x4
DDPF_RGB = 0x40
DDPF_LUMINANCE = 0x20000
DDSCAPS2_CUBEMAP = 0x200
DDSCAPS2_VOLUME = 0x200000

FOURCC = {
    b"DXT1": "DXT1", b"DXT2": "DXT2", b"DXT3": "DXT3",
    b"DXT4": "DXT4", b"DXT5": "DXT5",
    b"ATI1": "ATI1/BC4", b"BC4U": "ATI1/BC4",
    b"ATI2": "ATI2/BC5", b"BC5U": "ATI2/BC5",
    b"DX10": "DX10",
}

DXGI = {
    28: "R8G8B8A8_UNORM", 29: "R8G8B8A8_UNORM_SRGB",
    49: "R8G8_UNORM", 61: "R8_UNORM", 65: "A8_UNORM",
    71: "BC1_UNORM", 72: "BC1_UNORM_SRGB",
    74: "BC2_UNORM", 75: "BC2_UNORM_SRGB",
    77: "BC3_UNORM", 78: "BC3_UNORM_SRGB",
    80: "BC4_UNORM", 81: "BC4_SNORM",
    83: "BC5_UNORM", 84: "BC5_SNORM",
    85: "B5G6R5_UNORM", 86: "B5G5R5A1_UNORM",
    87: "B8G8R8A8_UNORM", 88: "B8G8R8X8_UNORM",
    90: "B8G8R8A8_UNORM_SRGB", 91: "B8G8R8X8_UNORM_SRGB",
    95: "BC6H_UF16", 96: "BC6H_SF16",
    98: "BC7_UNORM", 99: "BC7_UNORM_SRGB",
}


def classify(prefix: bytes) -> dict:
    """Classify a DDS header without trusting the payload."""
    if len(prefix) < 128 or prefix[:4] != b"DDS ":
        return {"error": "not a DDS header"}
    if struct.unpack_from("<I", prefix, 4)[0] != 124:
        return {"error": "bad DDS dwSize"}
    flags, height, width = struct.unpack_from("<III", prefix, 8)
    depth = struct.unpack_from("<I", prefix, 24)[0] if flags & 0x800000 else 1
    mips = struct.unpack_from("<I", prefix, 28)[0] if flags & 0x20000 else 1
    pf_size, pf_flags, fourcc_raw, bits, rmask, gmask, bmask, amask = \
        struct.unpack_from("<8I", prefix, 76)
    caps2 = struct.unpack_from("<I", prefix, 112)[0]
    fourcc = struct.pack("<I", fourcc_raw)
    out = {
        "width": width, "height": height, "mips": max(1, mips),
        "depth": max(1, depth),
        "cube": bool(caps2 & DDSCAPS2_CUBEMAP),
        "volume": bool(caps2 & DDSCAPS2_VOLUME),
        "fourcc": fourcc.decode("ascii", "replace").rstrip("\x00"),
        "dxgi": None, "bits": bits,
        "masks": [rmask, gmask, bmask, amask],
        "format": None,
    }
    if pf_size != 32:
        out["error"] = "bad DDS_PIXELFORMAT size %d" % pf_size
        return out
    if pf_flags & DDPF_FOURCC:
        if fourcc == b"DX10":
            if len(prefix) < 148:
                out["error"] = "truncated DX10 header"
                return out
            dxgi, dim, _misc, array_size, _m2 = struct.unpack_from("<5I", prefix, 128)
            out["dxgi"] = dxgi
            out["format"] = DXGI.get(dxgi, "DXGI_%d" % dxgi)
            out["array_size"] = array_size
            out["dimension"] = dim
        else:
            out["format"] = FOURCC.get(fourcc, "fourcc_%s" % out["fourcc"])
    elif pf_flags & DDPF_LUMINANCE:
        out["format"] = {8: "L8", 16: "A8L8"}.get(bits, "LUM%d" % bits)
    elif pf_flags & DDPF_RGB:
        key = (bits, rmask, gmask, bmask, amask)
        out["format"] = {
            (32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000): "A8R8G8B8",
            (32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0): "X8R8G8B8",
            (32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000): "A8B8G8R8",
            (32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0): "X8B8G8R8",
            (24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0): "R8G8B8",
            (16, 0xF800, 0x07E0, 0x001F, 0): "R5G6B5",
            (16, 0x7C00, 0x03E0, 0x001F, 0x8000): "A1R5G5B5",
            (16, 0x0F00, 0x00F0, 0x000F, 0xF000): "A4R4G4B4",
        }.get(key, "RGB%d_%08x%08x%08x%08x" % (bits, rmask, gmask, bmask, amask))
    else:
        out["format"] = "uncompressed_flags_0x%x" % pf_flags
    if out["format"] is None:
        out["format"] = "unknown"
    return out


def _merge(bucket: dict, info: dict) -> None:
    bucket["files"] += 1
    bucket["stored_bytes"] += info.get("stored_bytes", 0)
    bucket["cube"] += 1 if info.get("cube") else 0
    bucket["volume"] += 1 if info.get("volume") else 0
    if info.get("error"):
        bucket["errors"] += 1


def scan_archive(path: str, rows: list, verbose=True) -> None:
    with bsa.open_archive(path) as archive:
        for entry in archive.list_files():
            if entry.extension != "dds":
                continue
            try:
                prefix = archive.read_prefix(entry.path, 160)
                info = classify(prefix)
            except Exception as exc:  # noqa: BLE001
                info = {"error": "%s: %s" % (type(exc).__name__, exc)}
            info["path"] = "bsa:%s\\%s" % (os.path.basename(path), entry.path)
            info["stored_bytes"] = entry.size
            info["source"] = "bsa"
            rows.append(info)
    if verbose:
        print("scanned %-30s %d DDS" % (os.path.basename(path),
              sum(1 for r in rows if r.get("source") == "bsa" and
                  os.path.basename(path) in r.get("path", ""))))


def scan_tree(root: str, rows: list, label: str) -> int:
    if not os.path.isdir(root):
        return 0
    count = 0
    for base, _dirs, files in os.walk(root):
        for name in files:
            if not name.lower().endswith(".dds"):
                continue
            path = os.path.join(base, name)
            try:
                with open(path, "rb") as handle:
                    prefix = handle.read(160)
                info = classify(prefix)
            except OSError as exc:
                info = {"error": str(exc)}
            info["path"] = path
            info["stored_bytes"] = os.path.getsize(path)
            info["source"] = label
            rows.append(info)
            count += 1
    return count


def summarize(rows: list) -> dict:
    by_source = defaultdict(Counter)
    by_format = Counter()
    bytes_by_format = Counter()
    category = Counter()
    errors = []
    cubes = Counter()
    volumes = Counter()
    for row in rows:
        source = row.get("source", "?")
        fmt = row.get("format") or "error"
        by_source[source][fmt] += 1
        by_format[fmt] += 1
        bytes_by_format[fmt] += row.get("stored_bytes", 0)
        if row.get("error"):
            errors.append({"path": row.get("path"), "error": row["error"]})
        if row.get("cube"):
            cubes[fmt] += 1
        if row.get("volume"):
            volumes[fmt] += 1
        if fmt.startswith("BC7") or fmt.startswith("BC6"):
            category["bc7_or_bc6"] += 1
        elif fmt in ("ATI2/BC5", "BC5_UNORM", "BC5_SNORM"):
            category["bc5"] += 1
        elif fmt in ("DXT1", "BC1_UNORM", "BC1_UNORM_SRGB"):
            category["dxt1"] += 1
        elif fmt in ("DXT3", "BC2_UNORM", "BC2_UNORM_SRGB"):
            category["dxt3"] += 1
        elif fmt in ("DXT5", "BC3_UNORM", "BC3_UNORM_SRGB"):
            category["dxt5"] += 1
        elif fmt in ("A8R8G8B8", "X8R8G8B8", "A8B8G8R8", "X8B8G8R8",
                     "R8G8B8A8_UNORM", "R8G8B8A8_UNORM_SRGB",
                     "B8G8R8A8_UNORM", "B8G8R8X8_UNORM",
                     "B8G8R8A8_UNORM_SRGB", "B8G8R8X8_UNORM_SRGB",
                     "R8G8B8", "R5G6B5", "A1R5G5B5", "A4R4G4B4",
                     "B5G6R5_UNORM", "B5G5R5A1_UNORM"):
            category["uncompressed"] += 1
        elif fmt in ("A8_UNORM", "L8", "A8L8", "LUM8", "LUM16"):
            category["a8_l8"] += 1
        elif fmt in ("R8_UNORM", "R8G8_UNORM"):
            category["r8_rg8"] += 1
        else:
            category["other"] += 1
    return {
        "files": len(rows),
        "formats": dict(by_format.most_common()),
        "format_bytes": {k: bytes_by_format[k] for k in by_format},
        "categories": dict(category.most_common()),
        "by_source": {s: dict(c.most_common()) for s, c in by_source.items()},
        "cubes": dict(cubes.most_common()),
        "volumes": dict(volumes.most_common()),
        "errors": errors[:20],
        "error_count": len(errors),
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", default=DEFAULT_DATA)
    ap.add_argument("--mod-dir", action="append", default=None,
                    help="mod root; each immediate child is scanned for textures/")
    ap.add_argument("--json", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    rows: list = []
    missing = []
    for name in sorted(os.listdir(args.data)):
        if not name.lower().endswith(".bsa"):
            continue
        stem = name[:-4].lower()
        if not any(h in stem for h in ARCHIVE_HINTS):
            continue
        path = os.path.join(args.data, name)
        scan_archive(path, rows, verbose=not args.quiet)
    loose = scan_tree(os.path.join(args.data, "textures"), rows, "data_loose")
    if not args.quiet:
        print("scanned %-30s %d DDS" % ("Data\\textures", loose))
    mod_dirs = args.mod_dir if args.mod_dir is not None else list(DEFAULT_MOD_DIRS)
    for root in mod_dirs:
        if not os.path.isdir(root):
            missing.append(root)
            continue
        for child in sorted(os.listdir(root)):
            sub = os.path.join(root, child)
            if not os.path.isdir(sub):
                continue
            textures = os.path.join(sub, "textures")
            n = scan_tree(textures, rows, "mod:" + child)
            if n and not args.quiet:
                print("scanned mod %-28s %d DDS" % (child[:28], n))
    summary = summarize(rows)
    summary["sources"] = {
        "data": args.data,
        "mod_dirs": mod_dirs,
        "missing_sources": missing,
    }
    summary["rows"] = rows
    if args.json:
        parent = os.path.dirname(args.json)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(summary, handle, indent=2)
            handle.write("\n")
    print("files: %d  formats: %d  errors: %d" %
          (summary["files"], len(summary["formats"]), summary["error_count"]))
    print("categories:", json.dumps(summary["categories"], sort_keys=True))
    for fmt, count in list(summary["formats"].items())[:20]:
        print("  %-22s %6d  cubes=%d volumes=%d" %
              (fmt, count, summary["cubes"].get(fmt, 0),
               summary["volumes"].get(fmt, 0)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
