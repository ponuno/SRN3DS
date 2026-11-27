"""Build a cell-aware ``stream_index.bin`` for a Skyrim Data tree.

The device format is the one already parsed by ``source/asset_stream.c``
(schema 2, 64-byte cell rows).  This tool fills the cell table for interior
cells and exterior windows:

  * CELL -> REFR -> base record ``MODL`` gives the cell's NIF paths;
  * each NIF is read from Data (loose or the first matching BSA) and its
    ``BSShaderTextureSet`` strings give the DDS paths (the same strings a TXST
    record would carry; refs never point at TXST, so the NIF is the honest
    source for "which textures this cell pulls");
  * every resource is resolved to a real archive directory entry, and the
    physical range is computed from the stored prefix, never estimated.

Exterior cells are keyed by ``(worldspace_form_id, grid_x, grid_y)``.  The
WRLD pass records every worldspace (form id, editor id, ``DATA`` flags,
``NAM0``/``NAM9`` bounds) and the CELL walker remembers which WRLD owns each
exterior cell.  ``--world Tamriel --grid 0,0 --radius 1`` selects a bounded
window; the whole ESM is never indexed.  LOD keys (``.btr``/``.bto``/
``_lod.nif``) are *not* emitted: deriving their names needs the terrain LOD
grid, not just the cell grid, and inventing them would make every lookup a
miss.  See ``docs/STREAMING.md``.

The 64-byte row reserves exactly 8 resource ids, so a cell with more assets is
truncated to the 8 largest stored payloads.  That truncation is reported.
Schema 1 remains readable: it uses the same 64-byte rows, only the header
schema number differs.

Usage::

    python tools/stream_index_build.py --data "E:/.../Skyrim/Data" \
        --cell RiverwoodSleepingGiantInn --out build/stream_index.bin

    python tools/stream_index_build.py --world Tamriel --grid 0,0 --radius 1

    python tools/stream_index_build.py --cell 0x133C6 --cell WhiterunBanneredMare

The output is deterministic for the same inputs (row order is fixed by path,
cells by key, archives by the order the first resource needs them).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from skyrim import bsa, esm, nif  # noqa: E402

MAGIC = b"SRNX"
SCHEMA = 2
HEADER_SIZE = 80
ARCHIVE_ENTRY_SIZE = 56
RESOURCE_ENTRY_SIZE = 104
CELL_ENTRY_SIZE = 64
FLAG_LITTLE_ENDIAN = 0x0001
CELL_IDS = 8
CELL_KEY_BYTES = 24
RESOURCE_FLAG_COMPRESSED = 0x0001
RESOURCE_FLAG_EMBEDDED = 0x0002

DEFAULT_DATA = r"E:/SteamLibrary/steamapps/common/Skyrim/Data"
DEFAULT_ARCHIVES = (
    "Skyrim - Meshes.bsa",
    "Skyrim - Textures.bsa",
    "Update.bsa",
    "Dawnguard.bsa",
    "Dragonborn.bsa",
    "HearthFires.bsa",
)


class BuildError(Exception):
    pass


@dataclass
class Source:
    archive: str          # file name under Data
    entry: bsa.BsaFileEntry
    archive_size: int
    compressed: bool
    embedded: bool
    prefix: int           # bytes before the payload inside entry.offset
    unpacked: int


@dataclass
class Resource:
    path: str
    source: Source
    archive_index: int = -1
    stored_sha: bytes = b""

    @property
    def payload_length(self) -> int:
        return self.source.entry.size - self.source.prefix


class Resolver:
    """Loose files first, then the configured BSA list (retail overlay order)."""

    def __init__(self, data_dir: str, archives: Sequence[str]):
        self.data_dir = data_dir
        self._paths = [os.path.join(data_dir, name) for name in archives]
        self._archives: Dict[str, Optional[bsa.BsaArchive]] = {}
        self._sizes: Dict[str, int] = {}
        self.stats = {"loose": 0, "bsa": 0, "missing": 0}

    def _archive(self, path: str) -> Optional[bsa.BsaArchive]:
        if path not in self._archives:
            try:
                self._archives[path] = bsa.BsaArchive(path, verify=False)
            except (OSError, bsa.BsaError) as exc:
                print("stream_index_build: skip %s: %s" % (path, exc),
                      file=sys.stderr)
                self._archives[path] = None
        return self._archives[path]

    def resolve(self, key: str) -> Optional[Source]:
        key = key.replace("/", "\\").strip("\\")
        loose = os.path.join(self.data_dir, key)
        if os.path.isfile(loose):
            self.stats["loose"] += 1
            return None  # loose-only assets have no archive range
        for path in self._paths:
            archive = self._archive(path)
            if archive is None:
                continue
            entry = archive.find(key)
            if entry is None:
                continue
            compressed = bool(archive.header.compressed)
            embedded = bool(archive.header.embedded_names)
            prefix = 0
            if embedded:
                archive._file.seek(entry.offset)
                head = archive._file.read(1)
                if head:
                    prefix = 1 + head[0]
            if compressed:
                prefix += 4  # unpacked-size prefix
            unpacked = archive.unpacked_size(key)
            self.stats["bsa"] += 1
            return Source(
                archive=os.path.basename(path),
                entry=entry,
                archive_size=os.path.getsize(path),
                compressed=compressed,
                embedded=embedded,
                prefix=prefix,
                unpacked=unpacked,
            )
        self.stats["missing"] += 1
        return None

    def read(self, source: Source) -> bytes:
        path = os.path.join(self.data_dir, source.archive)
        archive = self._archive(path)
        if archive is None:
            raise BuildError("archive reopened failed: %s" % path)
        return archive.read_file(source.entry.path)

    def close(self) -> None:
        for archive in self._archives.values():
            if archive is not None:
                archive.close()

    def archive_info(self, name: str) -> Tuple[int, bsa.BsaArchive]:
        path = os.path.join(self.data_dir, name)
        archive = self._archive(path)
        if archive is None:
            raise BuildError("archive unavailable: %s" % path)
        return os.path.getsize(path), archive


def _select_cells(scan: esm.PluginScan, selectors: Sequence[str]) -> List[esm.CellSummary]:
    chosen: List[esm.CellSummary] = []
    seen = set()
    for selector in selectors:
        form_id = None
        text = selector
        try:
            form_id = int(selector, 16) if selector.lower().startswith("0x") \
                else int(selector, 10)
            text = None
        except ValueError:
            pass
        matches = []
        for cell in scan.cells:
            if form_id is not None and cell.form_id == form_id:
                matches.append(cell)
            elif text and cell.editor_id.lower() == text.lower():
                matches.append(cell)
        if not matches:
            raise BuildError("no cell matching %r" % selector)
        if len(matches) > 1:
            raise BuildError("ambiguous cell selector %r: %s" % (
                selector, ", ".join("0x%08x" % c.form_id for c in matches[:8])))
        cell = matches[0]
        if cell.form_id in seen:
            continue
        seen.add(cell.form_id)
        chosen.append(cell)
    return chosen


def _select_worlds(scan: esm.PluginScan,
                   selectors: Sequence[str]) -> List[esm.WorldSummary]:
    chosen: List[esm.WorldSummary] = []
    seen = set()
    for selector in selectors:
        matches = scan.find_worlds(selector)
        if not matches:
            raise BuildError("no worldspace matching %r" % selector)
        if len(matches) > 1:
            raise BuildError("ambiguous worldspace selector %r: %s" % (
                selector, ", ".join("%s (0x%08x)" % (w.editor_id, w.form_id)
                                    for w in matches[:8])))
        world = matches[0]
        if world.form_id in seen:
            continue
        seen.add(world.form_id)
        chosen.append(world)
    return chosen


def _select_exterior_window(scan: esm.PluginScan, world: esm.WorldSummary,
                            grid: Tuple[int, int],
                            radius: int) -> List[esm.CellSummary]:
    if radius < 0:
        raise BuildError("--radius must be >= 0")
    cells = []
    for cell in scan.cells:
        if not cell.exterior or cell.world_form_id != world.form_id:
            continue
        gx, gy = cell.grid
        if abs(gx - grid[0]) > radius or abs(gy - grid[1]) > radius:
            continue
        cells.append(cell)
    if not cells:
        raise BuildError(
            "worldspace %s (0x%08x) has no exterior cells within %d of %d,%d"
            % (world.editor_id, world.form_id, radius, grid[0], grid[1]))
    cells.sort(key=lambda c: (c.grid[0], c.grid[1]))
    return cells


def _cell_key(cell: esm.CellSummary) -> Tuple[int, int, int, int, int]:
    """Mirror the C comparator: (kind, form_id, gx, gy, world)."""
    if cell.interior:
        return (0, cell.form_id, 0, 0, 0)
    return (1, 0, cell.grid[0], cell.grid[1], cell.world_form_id)


def _cell_assets(esm_path: str, scan: esm.PluginScan,
                 cell: esm.CellSummary, resolver: Resolver) -> Tuple[List[Resource], dict]:
    files = esm.extract_cell(esm_path, form_id=cell.form_id, scan=scan)
    meshes: List[Tuple[str, Source]] = []
    seen = set()
    for mesh in files.mesh_paths:
        key = mesh.replace("/", "\\").strip("\\").lower()
        if key in seen:
            continue
        seen.add(key)
        source = resolver.resolve(key)
        if source is not None:
            meshes.append((key, source))

    resources: Dict[str, Resource] = {}
    txst_textures = 0
    for texture in files.texture_paths:
        key = texture.replace("/", "\\").strip("\\").lower()
        if key in resources:
            continue
        source = resolver.resolve(key)
        if source is not None:
            resources[key] = Resource(path=key, source=source)
            txst_textures += 1
    for key, source in meshes:
        resources.setdefault(key, Resource(path=key, source=source))
        try:
            data = resolver.read(source)
        except (OSError, bsa.BsaError, KeyError):
            continue
        for texture in nif.scan_texture_paths(data):
            texture = texture.replace("/", "\\").strip("\\").lower()
            if not texture.endswith(".dds") or texture in resources:
                continue
            tex_source = resolver.resolve(texture)
            if tex_source is not None:
                resources[texture] = Resource(path=texture, source=tex_source)
    stats = {
        "form_id": "0x%08x" % cell.form_id,
        "editor_id": cell.editor_id,
        "refs": cell.ref_count,
        "meshes_resolved": len(meshes),
        "txst_textures": txst_textures,
        "assets_total": len(resources),
    }
    return list(resources.values()), stats


def _read_stored(resolver: Resolver, res: Resource) -> bytes:
    path = os.path.join(resolver.data_dir, res.source.archive)
    archive = resolver._archive(path)
    entry = res.source.entry
    archive._file.seek(entry.offset)
    data = archive._file.read(entry.size)
    if len(data) != entry.size:
        raise BuildError("short read for %s" % res.path)
    return data


def build(data_dir: str, esm_path: str, selectors: Sequence[str],
          archive_names: Sequence[str], hash_archives: bool,
          *, worlds: Sequence[str] = (),
          grid: Tuple[int, int] = (0, 0),
          radius: int = 0) -> Tuple[bytes, dict]:
    if not os.path.isfile(esm_path):
        raise BuildError("ESM not found: %s" % esm_path)
    scan = esm.scan_plugin(esm_path)
    cells = _select_cells(scan, selectors)
    selected_worlds = _select_worlds(scan, worlds)
    resolver = Resolver(data_dir, archive_names)
    report = {"cells": [], "resource_archives": {}, "warnings": []}

    wanted: List[esm.CellSummary] = []
    seen_cells = set()

    def add_cell(cell: esm.CellSummary, why: str) -> None:
        if cell.form_id in seen_cells:
            return
        if not cell.interior and not cell.world_form_id:
            report["warnings"].append(
                "cell 0x%08x is exterior without an owning WRLD; skipped (%s)"
                % (cell.form_id, why))
            return
        if not cell.interior and cell.grid is None:
            report["warnings"].append(
                "cell 0x%08x is neither interior nor gridded; skipped (%s)"
                % (cell.form_id, why))
            return
        seen_cells.add(cell.form_id)
        wanted.append(cell)

    for cell in cells:
        add_cell(cell, "--cell")
    for world in selected_worlds:
        for cell in _select_exterior_window(scan, world, grid, radius):
            add_cell(cell, "world window")

    chosen: List[Tuple[esm.CellSummary, List[Resource]]] = []
    try:
        for cell in wanted:
            assets, stats = _cell_assets(esm_path, scan, cell, resolver)
            # Deterministic order: largest stored payload first, then path.
            assets.sort(key=lambda r: (-r.source.entry.size, r.path))
            picked = assets[:CELL_IDS]
            stats["kind"] = "interior" if cell.interior else "exterior"
            stats["grid"] = list(cell.grid) if cell.grid else None
            stats["world_form_id"] = "0x%08x" % cell.world_form_id
            stats["indexed"] = len(picked)
            stats["truncated"] = max(0, len(assets) - CELL_IDS)
            stats["indexed_bytes"] = sum(a.source.unpacked for a in picked)
            report["cells"].append(stats)
            chosen.append((cell, picked))

        archive_order: List[str] = []
        for _cell, assets in chosen:
            for asset in assets:
                if asset.source.archive not in archive_order:
                    archive_order.append(asset.source.archive)

        archive_rows = []
        for index, name in enumerate(archive_order):
            size, archive = resolver.archive_info(name)
            digest = b"\x00" * 32
            if hash_archives:
                h = hashlib.sha256()
                with open(os.path.join(data_dir, name), "rb") as handle:
                    for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
                        h.update(chunk)
                digest = h.digest()
            archive_rows.append({
                "name": name,
                "bytes": size,
                "sha256": digest,
                "priority": index,
                "files": 0,
                "compressed": archive.header.compressed,
            })

        all_resources: List[Resource] = []
        for _cell, assets in chosen:
            for asset in assets:
                asset.archive_index = archive_order.index(asset.source.archive)
                all_resources.append(asset)
        # The format requires strictly sorted paths and unique ids.
        by_path: Dict[str, Resource] = {}
        for asset in all_resources:
            by_path.setdefault(asset.path, asset)
        ordered = sorted(by_path.values(), key=lambda r: r.path)
        for resource in ordered:
            resource.stored_sha = hashlib.sha256(_read_stored(resolver, resource)).digest()
            archive_rows[resource.archive_index]["files"] += 1

        id_of = {resource.path: i for i, resource in enumerate(ordered)}

        pool = bytearray()
        path_offsets: List[Tuple[int, int]] = []
        for resource in ordered:
            raw = resource.path.encode("cp1252")
            path_offsets.append((len(pool), len(raw)))
            pool += raw
        while len(pool) % 8:
            pool.append(0)
        name_offsets = []
        for row in archive_rows:
            raw = row["name"].encode("cp1252")
            name_offsets.append((len(pool), len(raw)))
            pool += raw
        while len(pool) % 8:
            pool.append(0)

        archive_table = bytearray()
        for row, (off, length) in zip(archive_rows, name_offsets):
            archive_table += struct.pack(
                "<IIQ32sII", off, length, row["bytes"], row["sha256"],
                row["priority"], row["files"])

        resource_table = bytearray()
        for index, (resource, (off, length)) in enumerate(zip(ordered, path_offsets)):
            flags = 0
            if resource.source.compressed:
                flags |= RESOURCE_FLAG_COMPRESSED
            if resource.source.embedded:
                flags |= RESOURCE_FLAG_EMBEDDED
            payload = resource.payload_length
            physical = resource.source.entry.offset + resource.source.prefix
            resource_table += struct.pack(
                "<IIIIQIIII32s32s", index, off, length,
                resource.archive_index, physical, payload,
                resource.source.entry.size, resource.source.entry.offset,
                flags, resource.stored_sha, resource.stored_sha)

        seen_keys = {}
        for cell, _assets in chosen:
            key = _cell_key(cell)
            if key in seen_keys:
                raise BuildError(
                    "cell key %r collides: 0x%08x and 0x%08x"
                    % (key, seen_keys[key], cell.form_id))
            seen_keys[key] = cell.form_id

        requested = {cell.form_id: [r.path for r in assets]
                     for cell, assets in chosen}
        cell_table = bytearray()
        cells_written = []
        for cell, assets in sorted(chosen, key=lambda item: _cell_key(item[0])):
            ids = [id_of[asset.path] for asset in assets]
            ids += [0] * (CELL_IDS - len(ids))
            kind, form_id, gx, gy, ws = _cell_key(cell)
            cell_table += struct.pack(
                "<IIiiIIII8I", kind, form_id, gx, gy, ws, 0, len(assets), 0,
                *ids)
            cells_written.append({
                "form_id": "0x%08x" % cell.form_id,
                "kind": "interior" if cell.interior else "exterior",
                "grid": list(cell.grid) if cell.grid else None,
                "world_form_id": "0x%08x" % cell.world_form_id,
                "key": [kind, form_id, gx, gy, ws],
                "editor_id": cell.editor_id,
                "ids": len(assets),
                "paths": requested[cell.form_id],
            })

        archive_off = HEADER_SIZE
        resource_off = archive_off + len(archive_table)
        pool_off = resource_off + len(resource_table)
        cell_off = pool_off + len(pool)
        total = cell_off + len(cell_table) + 4
        generation = int.from_bytes(hashlib.sha256(
            bytes(archive_table) + bytes(resource_table) + bytes(pool) +
            bytes(cell_table)).digest()[:8], "little")
        header = struct.pack(
            "<4s6H4I6Q", MAGIC, SCHEMA, HEADER_SIZE, ARCHIVE_ENTRY_SIZE,
            RESOURCE_ENTRY_SIZE, CELL_ENTRY_SIZE, FLAG_LITTLE_ENDIAN,
            len(archive_rows), len(ordered), len(cells_written), 0,
            generation, archive_off, resource_off, pool_off, cell_off, total)
        blob = header + bytes(archive_table) + bytes(resource_table) + \
            bytes(pool) + bytes(cell_table)
        blob += struct.pack("<I", zlib.crc32(blob) & 0xFFFFFFFF)

        interior_count = sum(1 for cell, _ in chosen if cell.interior)
        exterior_count = len(chosen) - interior_count
        report.update({
            "data_dir": data_dir,
            "esm": esm_path,
            "schema": SCHEMA,
            "cell_count": len(cells_written),
            "interior_count": interior_count,
            "exterior_count": exterior_count,
            "resource_count": len(ordered),
            "archive_count": len(archive_rows),
            "archives": [row["name"] for row in archive_rows],
            "resolver": resolver.stats,
            # Loose winners have no archive range, so a schema-1/2 mount cannot
            # stage them.  They are counted here instead of silently indexed.
            "loose_skipped": resolver.stats["loose"],
            "cells_written": cells_written,
            "worlds": [world.as_dict() for world in selected_worlds],
            "window": {"grid": list(grid), "radius": radius} if selected_worlds
                      else None,
            "exterior": ("emitted with the owning WRLD form id"
                         if exterior_count else
                         "not emitted: no exterior cell selected"),
            "lod": ("not emitted: .btr/.bto/_lod.nif need the terrain LOD "
                    "grid, which this pass does not derive"),
            "cell_slot_limit": CELL_IDS,
        })
        return blob, report
    finally:
        resolver.close()


def _parse_grid(text: str) -> Tuple[int, int]:
    parts = text.replace(";", ",").split(",")
    if len(parts) != 2:
        raise BuildError("--grid needs two comma-separated ints, got %r" % text)
    try:
        return (int(parts[0], 0), int(parts[1], 0))
    except ValueError as exc:
        raise BuildError("--grid is not two ints: %r" % text) from exc


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default=os.environ.get("SKYRIM_DATA", DEFAULT_DATA))
    parser.add_argument("--esm", default=None)
    parser.add_argument("--out", default="stream_index.bin")
    parser.add_argument("--cell", action="append", default=[],
                        help="cell editor id, decimal or 0x-prefixed form id")
    parser.add_argument("--world", action="append", default=[],
                        help="worldspace editor id or form id (exterior window)")
    parser.add_argument("--grid", default="0,0",
                        help="exterior window centre gx,gy (default 0,0)")
    parser.add_argument("--radius", type=int, default=0,
                        help="window half-size in cells (default 0: one cell)")
    parser.add_argument("--archives", default=",".join(DEFAULT_ARCHIVES))
    parser.add_argument("--no-archive-sha", action="store_true",
                        help="leave archive SHA-256 zero (size is still real)")
    parser.add_argument("--report", default=None,
                        help="write the JSON report to this path too")
    args = parser.parse_args(argv)

    selectors = args.cell or ([] if args.world else ["RiverwoodSleepingGiantInn"])
    esm_path = args.esm or os.path.join(args.data, "Skyrim.esm")
    archives = [name for name in args.archives.split(",") if name]
    try:
        blob, report = build(args.data, esm_path, selectors, archives,
                             not args.no_archive_sha, worlds=args.world,
                             grid=_parse_grid(args.grid), radius=args.radius)
    except (BuildError, esm.EsmError, bsa.BsaError, OSError) as exc:
        print("stream_index_build: %s" % exc, file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as handle:
        handle.write(blob)
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    print(text)
    print("wrote %s: %d bytes" % (args.out, len(blob)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
