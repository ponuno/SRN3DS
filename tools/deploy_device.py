#!/usr/bin/env python3
"""Turn a Skyrim LE install into a device-ready tree for the New 3DS.

Two commands:

    python tools\\deploy_device.py plan  --data F:\\Skyrim\\Data ^
        --tesv F:\\Skyrim\\TESV.exe --out E:\\stage --json E:\\stage_plan.json
    python tools\\deploy_device.py stage --data F:\\Skyrim\\Data ^
        --tesv F:\\Skyrim\\TESV.exe --ir E:\\census_ir2\\ir_container\\tesv.irc ^
        --out E:\\stage

``plan`` is read-only: it resolves the load order and every asset winner,
classifies textures by container/format, reports what a copy would cost and
which vanilla entries a mod already hides.  ``stage`` writes the device tree:
core files, loose winners, and textures converted to a format the port can
load.

The policy is deliberately conservative: textures the device already supports
are copied byte-for-byte, unsupported containers are converted, and anything
that cannot be converted is reported as a named refusal rather than silently
copied.  No game asset is stored in the repository; both commands only read the
install the user points them at.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
from collections import Counter
from typing import Dict, Iterable, List, Optional, Tuple

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from skyrim import bsa, bsa_write, dds  # noqa: E402
from skyrim import data_overlay  # noqa: E402

try:  # executed as ``python tools/deploy_device.py``
    import mod_optimize  # noqa: E402
except Exception:  # pragma: no cover - import guard for package use
    from tools import mod_optimize  # type: ignore  # noqa: E402

SCHEMA = "skyrim-device-deploy/1"
DDS_MAGIC = b"DDS "
DDS_HEADER_BYTES = 148  # 4 magic + 124 header + 20 DX10 header

# Formats the port's CheckDeviceFormat accepts.  A texture already in one of
# these can be copied without a decode/encode round trip.
SUPPORTED_KINDS = {"bc1", "bc2", "bc3", "rgba8", "bgra8", "bgrx8"}
KEEP_AS_IS = {"bc1": "DXT1", "bc2": "DXT3", "bc3": "DXT5"}

OFFICIAL_PLUGINS = {"skyrim.esm", "update.esm", "dawnguard.esm",
                    "dragonborn.esm", "hearthfires.esm"}

# Files that must stay outside a cooked archive: plugins are loaded by name,
# configuration is read as text, and the intro is our own .bkr transcode.
COOK_SKIP_EXT = {".esm", ".esp", ".esl", ".ini", ".txt", ".bik", ".bkr"}
COOK_DEFAULT_CAP = 1800 * 1024 * 1024

# Files that must exist on the device and are not part of the asset overlay.
INI_NAMES = ("Skyrim.ini", "SkyrimPrefs.ini", "Plugins.txt", "loadorder.txt")


class DeployError(Exception):
    """A requested deployment cannot be produced as specified."""


# ---------------------------------------------------------------------------
# Asset enumeration


def _norm(path: str) -> str:
    return data_overlay.normalize_virtual_path(path).lower()


def _archive_by_host(overlay) -> Dict[str, bsa.BsaArchive]:
    out = {}
    for mount in overlay.archives:
        if not os.path.isfile(mount.host_path):
            continue
        key = os.path.normcase(os.path.abspath(mount.host_path))
        out[key] = overlay._open(mount)
    return out


def resolve_winners(overlay) -> Tuple[Dict[str, data_overlay.ResolvedAsset], Dict]:
    """Virtual path -> winning source, highest priority first.

    Loose files win over every archive (the overlay contract), and a later
    plugin's archive wins over an earlier one.  The second return value counts
    entries that lost, which is exactly what a cooked pack could drop.
    """
    winners: Dict[str, data_overlay.ResolvedAsset] = {}
    overridden = Counter()
    archives = _archive_by_host(overlay)
    for mount in reversed(overlay.archives):
        archive = archives.get(os.path.normcase(os.path.abspath(mount.host_path)))
        if archive is None:
            continue
        for entry in archive.list_files():
            key = _norm(entry.path)
            if key in winners:
                overridden["archive"] += 1
                continue
            winners[key] = data_overlay.ResolvedAsset(
                data_overlay.normalize_virtual_path(entry.path), "bsa",
                mount.host_path, mount.priority, mount.plugin, mount.name,
                entry.path)
    for rel in overlay.loose_files():
        key = _norm(rel)
        if key in winners:
            overridden[winners[key].kind] += 1
        winners[key] = data_overlay.ResolvedAsset(
            data_overlay.normalize_virtual_path(rel), "loose",
            os.path.join(overlay.data_root, rel), 1 << 30)
    return winners, {"overridden_entries": dict(overridden),
                     "winners": len(winners)}


def _read_prefix(asset: data_overlay.ResolvedAsset, archives, count: int) -> bytes:
    if asset.kind == "loose":
        with open(asset.host_path, "rb") as handle:
            return handle.read(count)
    archive = archives.get(os.path.normcase(os.path.abspath(asset.host_path)))
    if archive is None:
        return b""
    return archive.read_prefix(asset.entry_path, count)


def _read_all(asset: data_overlay.ResolvedAsset, archives) -> bytes:
    if asset.kind == "loose":
        with open(asset.host_path, "rb") as handle:
            return handle.read()
    archive = archives.get(os.path.normcase(os.path.abspath(asset.host_path)))
    if archive is None:
        raise DeployError("archive vanished: %s" % asset.host_path)
    return archive.read_file(asset.entry_path)


def _asset_size(asset: data_overlay.ResolvedAsset, archives) -> int:
    if asset.kind == "loose":
        try:
            return os.path.getsize(asset.host_path)
        except OSError:
            return 0
    archive = archives.get(os.path.normcase(os.path.abspath(asset.host_path)))
    if archive is None:
        return 0
    entry = archive.find(asset.entry_path)
    return int(entry.size) if entry is not None else 0


# ---------------------------------------------------------------------------
# Texture policy


def _nearest_pot(value: int) -> int:
    power = 1
    while power * 2 <= value:
        power *= 2
    return max(1, power)


def _max_dim_for(resize: str) -> int:
    return {"none": 0, "half": 512, "quarter": 256}.get(resize, 0)


def classify_texture(prefix: bytes, stored_size: int, max_dim: int) -> dict:
    """Decide what the deploy should do with one DDS."""
    row = {"stored_bytes": stored_size, "format": "?", "kind": "?",
           "width": 0, "height": 0, "mips": 0, "action": "refuse",
           "reason": ""}
    if len(prefix) < 128 or prefix[:4] != DDS_MAGIC:
        row["reason"] = "not a DDS"
        return row
    try:
        info = dds.parse_dds(prefix, require_payload=False)
    except dds.DdsError as exc:
        row["reason"] = "unreadable DDS: %s" % exc
        return row
    row.update(width=info.width, height=info.height,
               mips=info.mip_count, format=info.format_name, kind=info.kind,
               payload_bytes=info.payload_size)
    if info.width <= 0 or info.height <= 0:
        row["reason"] = "zero dimension"
        return row
    if info.width & (info.width - 1) or info.height & (info.height - 1):
        row["action"] = "rgba8"
        row["reason"] = "non-power-of-two"
    elif max_dim and max(info.width, info.height) > max_dim:
        if info.kind in SUPPORTED_KINDS:
            row["action"] = "rgba8"
            row["reason"] = "downscale %dx%d -> max %d" % (
                info.width, info.height, max_dim)
        else:
            row["reason"] = "downscale needs a decoder this kind lacks"
    elif info.kind in ("bc1", "bc2", "bc3"):
        row["action"] = "keep"
        row["reason"] = "device format " + KEEP_AS_IS[info.kind]
    elif info.kind in ("rgba8", "bgra8", "bgrx8"):
        row["action"] = "keep"
        row["reason"] = "uncompressed RGB(A) is supported"
    elif info.kind in ("rgb24", "bgr24"):
        row["action"] = "bc1"
        row["reason"] = "24-bit RGB -> DXT1"
    elif info.kind in ("bc4", "bc5"):
        row["action"] = "refuse"
        row["reason"] = "BC4/BC5 has no encode path on the device"
    else:
        row["reason"] = "unsupported container %s" % info.format_name
    if info.mip_count <= 1 and row["action"] in ("keep", "bc1", "rgba8"):
        row["mips_missing"] = True
    return row


# ---------------------------------------------------------------------------
# Report


def _budget(rows: Iterable[dict], winners, archives) -> dict:
    by_kind = Counter()
    by_action = Counter()
    stored = 0
    kept = 0
    converted = 0
    refused = 0
    for row in rows:
        by_kind[row["kind"]] += 1
        by_action[row["action"]] += 1
        stored += row["stored_bytes"]
        if row["action"] == "keep":
            kept += row["stored_bytes"]
        elif row["action"] in ("bc1", "rgba8"):
            # Estimate: BC1 is 4 bpp, RGBA8 is 32 bpp, with a mip chain.
            bpp = 4.0 if row["action"] == "bc1" else 32.0
            levels = max(1, row["mips"] or 1)
            converted += int(row["width"] * row["height"] * bpp / 8 * (1 if levels == 1 else 1.34))
        else:
            refused += row["stored_bytes"]
    return {"textures": len(rows), "formats": dict(sorted(by_kind.items())),
            "actions": dict(sorted(by_action.items())),
            "stored_bytes": stored, "kept_bytes": kept,
            "converted_bytes_estimate": converted, "refused_bytes": refused}


def plan_report(overlay, args) -> dict:
    winners, stats = resolve_winners(overlay)
    archives = _archive_by_host(overlay)
    textures = []
    non_textures = Counter()
    mod_assets = 0
    mod_bytes = 0
    max_dim = _max_dim_for(args.resize)
    for key in sorted(winners):
        asset = winners[key]
        if not key.endswith(".dds"):
            non_textures[os.path.splitext(key)[1] or "(none)"] += 1
            if asset.plugin.lower() not in OFFICIAL_PLUGINS:
                mod_assets += 1
            continue
        prefix = _read_prefix(asset, archives, DDS_HEADER_BYTES)
        row = classify_texture(prefix, _asset_size(asset, archives), max_dim)
        row.update(path=asset.virtual_path, source=asset.kind,
                   plugin=asset.plugin, archive=asset.archive)
        textures.append(row)
        if asset.plugin.lower() not in OFFICIAL_PLUGINS:
            mod_assets += 1
            mod_bytes += row["stored_bytes"]
    cores = core_files(overlay, args)
    cooked = None
    if args.pack == "cooked":
        refusals: List[dict] = []
        entries = cook_entries(winners, archives, max_dim, refusals,
                               _csv(args.exclude_groups),
                               _csv(args.exclude_ext))
        by_group = Counter(entry["group"] for entry in entries)
        by_group_bytes = Counter()
        for entry in entries:
            by_group_bytes[entry["group"]] += entry["estimate"]
        cap = args.max_archive_mb * 1024 * 1024
        grouped: Dict[str, List[dict]] = {}
        for entry in entries:
            grouped.setdefault(entry["group"], []).append(entry)
        cooked = {
            "cap_bytes": cap,
            "archives_estimate": sum(
                sum(1 for _ in _batches(items, cap))
                for items in grouped.values()),
            "files": len(entries),
            "bytes_estimate": sum(entry["estimate"] for entry in entries),
            "groups": {name: {"files": by_group[name],
                              "bytes": by_group_bytes[name]}
                       for name in sorted(by_group)},
            "refused": len(refusals),
        }
    return {
        "schema": SCHEMA,
        "command": "plan",
        "data_root": overlay.data_root,
        "load_order": overlay.load_report,
        "archives": [mount.as_dict() for mount in overlay.archives],
        "resolve": stats,
        "assets": {"total": len(winners), "textures": len(textures),
                   "other": dict(sorted(non_textures.items())),
                   "from_mods": mod_assets, "mod_bytes": mod_bytes},
        "textures": _budget(textures, winners, archives),
        "core": cores["report"],
        "cooked": cooked,
        "refusals": [row for row in textures if row["action"] == "refuse"][:50],
        "texture_rows": textures if args.full_rows else None,
    }


def core_files(overlay, args, include_archives: bool = True) -> dict:
    data_root = overlay.data_root
    root = os.path.dirname(os.path.abspath(data_root))
    entries = []
    tesv = args.tesv or os.path.join(root, "TESV.exe")
    _add_core(entries, "TESV.exe", tesv, required=True)
    for loaded in overlay.load_order:
        if not loaded.active:
            continue
        _add_core(entries, "Data/" + os.path.basename(loaded.header.path),
                  loaded.header.path, required=True)
    for name in INI_NAMES:
        for candidate in (os.path.join(data_root, name),
                          os.path.join(root, name),
                          os.path.join(os.path.expandvars(r"%LOCALAPPDATA%"),
                                       "Skyrim", name)):
            if os.path.isfile(candidate):
                _add_core(entries, "Data/" + name, candidate, required=False)
                break
    if args.ir:
        _add_core(entries, "ir/tesv.irc", args.ir, required=False)
    if not getattr(args, "skip_archives", False) and include_archives:
        for mount in overlay.archives:
            if os.path.isfile(mount.host_path):
                _add_core(entries, "Data/" + mount.name,
                          mount.host_path, required=False)
    video = os.path.join(data_root, "Video")
    if os.path.isdir(video):
        for name in sorted(os.listdir(video)):
            if name.lower().endswith(".bkr"):
                _add_core(entries, "Data/Video/" + name,
                          os.path.join(video, name), required=False)
    return {"entries": entries,
            "report": {"files": len(entries),
                       "bytes": sum(entry["bytes"] for entry in entries)}}


def _add_core(entries: List[dict], device_path: str, host_path: str,
              *, required: bool) -> None:
    if not os.path.isfile(host_path):
        if required:
            raise DeployError("required core file is missing: %s" % host_path)
        entries.append({"device_path": device_path, "host_path": host_path,
                        "bytes": 0, "missing": True})
        return
    entries.append({"device_path": device_path, "host_path": host_path,
                    "bytes": os.path.getsize(host_path)})


# ---------------------------------------------------------------------------
# Staging


def _rgba8_dds_bytes(levels: List[Tuple[int, int, bytes]]) -> bytes:
    width, height = levels[0][0], levels[0][1]
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (0x20000 if len(levels) > 1 else 0)
    header = struct.pack(
        "<4s7I44x32s5I",
        DDS_MAGIC, 124, flags, height, width,
        width * 4, 0, len(levels),
        struct.pack("<II4s5I", 32, 0x41, b"\0\0\0\0", 32,
                    0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000),
        0x1000, 0, 0, 0, 0)
    return header + b"".join(level[2] for level in levels)


def _write_rgba8_dds(path: str, levels: List[Tuple[int, int, bytes]]) -> int:
    blob = _rgba8_dds_bytes(levels)
    with open(path, "wb") as handle:
        handle.write(blob)
    return len(blob)


def texture_payload(asset, row, archives, max_dim: int) -> Tuple[bytes, str]:
    """Return the device bytes for one DDS plus the container name."""
    data = _read_all(asset, archives)
    if row["action"] == "keep":
        return data, "copied"
    width, height, rgba = dds.decode_dds(data, 0)
    target_w, target_h = width, height
    if width & (width - 1) or height & (height - 1):
        target_w, target_h = _nearest_pot(width), _nearest_pot(height)
    if max_dim:
        target_w = min(target_w, max_dim)
        target_h = min(target_h, max_dim)
    if (target_w, target_h) != (width, height):
        rgba = mod_optimize.resize_rgba(rgba, width, height, target_w, target_h)
        width, height = target_w, target_h
    levels = mod_optimize._mip_chain(rgba, width, height)
    if row["action"] == "bc1":
        has_alpha = any(rgba[i] < 255 for i in range(3, len(rgba), 4))
        if not has_alpha:
            return mod_optimize.bc1_dds_bytes(levels), "dds-bc1"
    return _rgba8_dds_bytes(levels), "dds-rgba8"


def convert_texture(asset, row, archives, out_root: str, max_dim: int) -> dict:
    payload, container = texture_payload(asset, row, archives, max_dim)
    rel = asset.virtual_path.replace("\\", os.sep).lstrip(os.sep)
    out_path = os.path.join(out_root, "Data", rel)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as handle:
        handle.write(payload)
    digest = hashlib.sha256(payload).hexdigest()
    return {"path": asset.virtual_path, "output": out_path, "bytes": len(payload),
            "container": container, "sha256": digest,
            "source": {"kind": asset.kind, "plugin": asset.plugin,
                       "archive": asset.archive, "width": row.get("width"),
                       "height": row.get("height"), "format": row.get("format")},
            "action": row["action"]}


# ---------------------------------------------------------------------------
# Cooked archives


def _csv(value: str) -> List[str]:
    return [item.strip() for item in (value or "").split(",") if item.strip()]


def _group_of(virtual_path: str) -> str:
    parts = virtual_path.replace("/", "\\").split("\\")
    if len(parts) < 2:
        return "root"
    group = parts[0].lower()
    return group if group not in ("", ".") else "root"


def cook_entries(winners, archives, max_dim: int,
                 refusals: List[dict], exclude_groups=(), exclude_ext=()) -> List[dict]:
    """Everything a cooked archive would contain, with an estimated size."""
    entries = []
    groups = {name.strip().lower() for name in exclude_groups if name.strip()}
    exts = {name.strip().lower() if name.strip().startswith(".") else
            "." + name.strip().lower() for name in exclude_ext if name.strip()}
    for key in sorted(winners):
        asset = winners[key]
        ext = os.path.splitext(key)[1]
        group = _group_of(asset.virtual_path)
        if ext in COOK_SKIP_EXT or key.startswith("video\\"):
            continue
        if group in groups or ext in exts:
            continue
        if key.endswith(".dds"):
            prefix = _read_prefix(asset, archives, DDS_HEADER_BYTES)
            row = classify_texture(prefix, _asset_size(asset, archives), max_dim)
            if row["action"] == "refuse":
                refusals.append({"path": asset.virtual_path,
                                 "reason": row["reason"],
                                 "plugin": asset.plugin})
                continue
            if row["action"] == "keep":
                estimate = _asset_size(asset, archives)
            else:
                bpp = 4 if row["action"] == "bc1" else 32
                estimate = int(row["width"] * row["height"] * bpp / 8 * 1.34)
        else:
            row = {"action": "copy", "reason": "non-texture"}
            estimate = _asset_size(asset, archives)
        entries.append({"asset": asset, "row": row, "estimate": estimate,
                        "group": group})
    return entries


def _batches(entries: List[dict], cap: int):
    batch: List[dict] = []
    size = 0
    for entry in entries:
        if batch and size + entry["estimate"] > cap:
            yield batch
            batch, size = [], 0
        batch.append(entry)
        size += entry["estimate"]
    if batch:
        yield batch


def _write_cooked_ini(out_data: str, source: str, names: List[str]) -> str:
    lines: List[str] = []
    if source and os.path.isfile(source):
        with open(source, "r", encoding="utf-8", errors="replace") as handle:
            lines = handle.read().replace("\r\n", "\n").split("\n")
    value = ", ".join(names)
    out: List[str] = []
    replaced = False
    seen_archive = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("["):
            seen_archive = stripped.lower() == "[archive]"
        low = stripped.lower()
        if low.startswith("sresourcearchivelist2"):
            out.append("sResourceArchiveList2=")
            continue
        if low.startswith("sresourcearchivelist="):
            out.append("sResourceArchiveList=" + value)
            replaced = True
            continue
        out.append(line)
    if not replaced:
        if not seen_archive:
            out.append("[Archive]")
        out.append("sResourceArchiveList=" + value)
    text = "\n".join(out).rstrip("\n") + "\n"
    path = os.path.join(out_data, "Skyrim.ini")
    with open(path, "w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(text)
    return path


def stage_cooked(overlay, args) -> dict:
    winners, stats = resolve_winners(overlay)
    archives = _archive_by_host(overlay)
    max_dim = _max_dim_for(args.resize)
    os.makedirs(args.out, exist_ok=True)
    cores = core_files(overlay, args, include_archives=False)
    copied = []
    for entry in cores["entries"]:
        if entry.get("missing"):
            continue
        dest = os.path.join(args.out, entry["device_path"].replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(entry["host_path"], dest)
        copied.append({"path": entry["device_path"], "bytes": entry["bytes"],
                       "sha256": hashlib.sha256(open(dest, "rb").read()).hexdigest(),
                       "class": "core"})
    refusals: List[dict] = []
    entries = cook_entries(winners, archives, max_dim, refusals,
                           _csv(args.exclude_groups),
                           _csv(args.exclude_ext))
    cap = args.max_archive_mb * 1024 * 1024
    archive_rows = []
    file_rows = []
    data_dir = os.path.join(args.out, "Data")
    os.makedirs(data_dir, exist_ok=True)
    groups: Dict[str, List[dict]] = {}
    for entry in entries:
        groups.setdefault(entry["group"], []).append(entry)
    for group in sorted(groups):
        for part, batch in enumerate(_batches(groups[group], cap), 1):
            name = "Skyrim - Cooked %s%s.bsa" % (
                group, "" if part == 1 else " %d" % part)
            rows: List[dict] = []

            def payloads(batch=batch, rows=rows):
                for entry in batch:
                    asset, row = entry["asset"], entry["row"]
                    try:
                        if row["action"] == "copy":
                            payload, container = _read_all(asset, archives), "copied"
                        else:
                            payload, container = texture_payload(
                                asset, row, archives, max_dim)
                    except (OSError, bsa.BsaError, dds.DdsError, DeployError) as exc:
                        refusals.append({"path": asset.virtual_path,
                                         "reason": "conversion failed: %s" % exc,
                                         "plugin": asset.plugin})
                        continue
                    rows.append({"path": asset.virtual_path, "bytes": len(payload),
                                 "sha256": hashlib.sha256(payload).hexdigest(),
                                 "container": container,
                                 "source": {"kind": asset.kind,
                                            "plugin": asset.plugin,
                                            "archive": asset.archive}})
                    yield asset.virtual_path, payload

            result = bsa_write.write_bsa_stream(
                os.path.join(data_dir, name), payloads(), compressed=False)
            archive_rows.append({"name": name, "files": len(rows),
                                 "bytes": result["bytes"],
                                 "sha256": hashlib.sha256(
                                     open(result["path"], "rb").read()).hexdigest()})
            file_rows.extend(rows)
    if not archive_rows:
        raise DeployError("cooked stage produced no archives")
    ini_source = ""
    for entry in cores["entries"]:
        if entry["device_path"].lower() == "data/skyrim.ini":
            ini_source = entry["host_path"]
            break
    _write_cooked_ini(data_dir, ini_source,
                      [row["name"] for row in archive_rows])
    manifest = {
        "schema": SCHEMA, "command": "stage", "cooked": True,
        "data_root": overlay.data_root, "out_root": os.path.abspath(args.out),
        "resize": args.resize, "load_order": overlay.load_report,
        "resolve": stats, "core": cores["report"],
        "archives": archive_rows, "files": file_rows,
        "refused": refusals,
        "bytes": sum(row["bytes"] for row in copied)
        + sum(row["bytes"] for row in archive_rows),
    }
    with open(os.path.join(args.out, "deploy_manifest.json"), "w",
              encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=1, sort_keys=True)
        handle.write("\n")
    return manifest


def stage(overlay, args) -> dict:
    winners, stats = resolve_winners(overlay)
    archives = _archive_by_host(overlay)
    os.makedirs(args.out, exist_ok=True)
    max_dim = _max_dim_for(args.resize)
    cores = core_files(overlay, args)
    copied = []
    for entry in cores["entries"]:
        if entry.get("missing"):
            continue
        dest = os.path.join(args.out, entry["device_path"].replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(entry["host_path"], dest)
        copied.append({"path": entry["device_path"], "bytes": entry["bytes"],
                       "sha256": hashlib.sha256(open(dest, "rb").read()).hexdigest(),
                       "class": "core"})
    converted = []
    skipped = []
    kept_in_archives = 0
    for key in sorted(winners):
        asset = winners[key]
        if key.endswith(".dds"):
            prefix = _read_prefix(asset, archives, DDS_HEADER_BYTES)
            row = classify_texture(prefix, _asset_size(asset, archives), max_dim)
            row.update(path=asset.virtual_path, source=asset.kind,
                       plugin=asset.plugin, archive=asset.archive)
            if row["action"] == "keep" and asset.kind == "bsa":
                kept_in_archives += 1
                continue
            if row["action"] == "refuse":
                skipped.append({"path": asset.virtual_path,
                                "reason": row["reason"],
                                "plugin": asset.plugin})
                continue
            if row["action"] == "keep":
                dest = os.path.join(args.out, "Data",
                                    asset.virtual_path.replace("\\", os.sep))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                data = _read_all(asset, archives)
                with open(dest, "wb") as handle:
                    handle.write(data)
                converted.append({"path": asset.virtual_path, "bytes": len(data),
                                  "sha256": hashlib.sha256(data).hexdigest(),
                                  "container": "copied",
                                  "source": {"kind": asset.kind,
                                             "plugin": asset.plugin}})
                continue
            converted.append(convert_texture(asset, row, archives, args.out, max_dim))
            continue
        if asset.kind != "loose":
            continue  # vanilla meshes/sounds stay in their archive
        dest = os.path.join(args.out, "Data",
                            asset.virtual_path.replace("\\", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(asset.host_path, dest)
        copied.append({"path": asset.virtual_path,
                       "bytes": os.path.getsize(dest),
                       "sha256": hashlib.sha256(open(dest, "rb").read()).hexdigest(),
                       "class": "loose"})
    manifest = {
        "schema": SCHEMA, "command": "stage", "data_root": overlay.data_root,
        "out_root": os.path.abspath(args.out), "resize": args.resize,
        "load_order": overlay.load_report,
        "resolve": stats,
        "core": cores["report"],
        "copied": copied, "textures": converted, "kept_in_archives": kept_in_archives,
        "refused": skipped,
        "bytes": sum(row["bytes"] for row in copied)
        + sum(row["bytes"] for row in converted),
    }
    manifest_path = os.path.join(args.out, "deploy_manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=1, sort_keys=True)
        handle.write("\n")
    return manifest


# ---------------------------------------------------------------------------
# CLI


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("plan", "stage"):
        p = sub.add_parser(name)
        p.add_argument("--data", default="")
        p.add_argument("--plugins", default=None)
        p.add_argument("--loadorder", default=None)
        p.add_argument("--tesv", default="")
        p.add_argument("--ir", default="")
        p.add_argument("--out", required=(name == "stage"))
        p.add_argument("--resize", choices=("none", "half", "quarter"),
                       default="none")
        p.add_argument("--json", default="")
        p.add_argument("--full-rows", action="store_true")
        p.add_argument("--skip-archives", action="store_true",
                       help="do not copy the mounted BSA files")
        p.add_argument("--pack", choices=("none", "cooked"), default="none",
                       help="cooked writes only winning assets into new archives")
        p.add_argument("--max-archive-mb", type=int, default=1800,
                       help="split cooked archives at this size")
        p.add_argument("--exclude-groups", default="",
                       help="comma list of top-level groups, e.g. grass,sound")
        p.add_argument("--exclude-ext", default="",
                       help="comma list of extensions, e.g. .bto,.btr,.hkx")
    args = parser.parse_args(argv)
    try:
        with data_overlay.DataOverlay(args.data or None,
                                      plugins_path=args.plugins,
                                      loadorder_path=args.loadorder) as overlay:
            report = (stage_cooked(overlay, args)
                      if args.command == "stage" and args.pack == "cooked"
                      else stage(overlay, args) if args.command == "stage"
                      else plan_report(overlay, args))
    except (data_overlay.OverlayError, DeployError, bsa.BsaError,
            dds.DdsError, OSError) as exc:
        print("deploy: %s" % exc, file=sys.stderr)
        return 2
    if args.command == "plan":
        view = {key: value for key, value in report.items()
                if key != "texture_rows"}
        print(json.dumps(view, indent=2, sort_keys=True))
    elif args.pack == "cooked":
        print(json.dumps({k: report[k] for k in
                          ("schema", "out_root", "cooked", "core")},
                         indent=2, sort_keys=True))
        print("archives=%d files=%d refused=%d"
              % (len(report["archives"]), len(report["files"]),
                 len(report["refused"])))
    else:
        print(json.dumps({k: report[k] for k in
                          ("schema", "out_root", "core", "kept_in_archives",
                           "bytes")}, indent=2, sort_keys=True))
        print("copied=%d textures=%d refused=%d"
              % (len(report["copied"]), len(report["textures"]),
                 len(report["refused"])))
    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=1, sort_keys=True)
            handle.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
