"""Minimal, read-only TES4/ESM record walker for per-cell asset lists.

This is not a game-data implementation: it extracts just enough to answer
"which meshes does one cell instance, and therefore which textures does it
pull in".  It understands the TES4 record/GRUP framing, compressed records
(zlib, 4-byte unpacked size prefix), CELL ``EDID``/``XCLC``/``DATA`` and REFR
``NAME``/``DATA`` subrecords, and ``MODL`` on the common placeable base
objects.

Not implemented: VMAD scripts, navmesh, landscape (``LAND``) geometry,
encounter zones, leveled lists (a REFR pointing at a LVLI is reported without
resolving the list), actors/NPC bodies, and records from masters other than the
one being scanned.  Those omissions are stated in the budget report.
"""

from __future__ import annotations

import mmap
import os
import struct
import zlib
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

COMPRESSED_FLAG = 0x00040000
MAX_RECORDS = 5_000_000

PLACEABLE_TYPES = (
    b"STAT", b"MSTT", b"TREE", b"ACTI", b"CONT", b"DOOR", b"FLOR", b"FURN",
    b"LIGH", b"MOVE", b"ADDN", b"ARMO", b"BOOK", b"CLUT", b"INGR", b"WATR",
    b"SLGM", b"WEAP", b"AMMO", b"SCRL", b"ALCH", b"KEYM", b"NPC_", b"CREA",
    b"GRAS", b"IDLM", b"BNDS", b"SOUN", b"RFCT", b"SMEN", b"TXST",
)


class EsmError(Exception):
    """Malformed plugin data."""


@dataclass
class CellSummary:
    form_id: int
    editor_id: str = ""
    grid: Optional[Tuple[int, int]] = None
    interior: bool = False
    ref_count: int = 0

    @property
    def exterior(self) -> bool:
        return self.grid is not None

    def as_dict(self) -> dict:
        return {
            "form_id": "0x%08x" % self.form_id,
            "editor_id": self.editor_id,
            "grid": list(self.grid) if self.grid else None,
            "interior": self.interior,
            "refs": self.ref_count,
        }


@dataclass
class RefEntry:
    form_id: int
    base_form_id: int
    position: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    mesh: Optional[str] = None


@dataclass
class CellFiles:
    summary: CellSummary
    refs: List[RefEntry]
    mesh_paths: List[str]

    def as_dict(self) -> dict:
        return {
            "cell": self.summary.as_dict(),
            "ref_count": len(self.refs),
            "mesh_count": len(self.mesh_paths),
            "meshes": self.mesh_paths,
        }


@dataclass
class PluginScan:
    cells: List[CellSummary]
    base_meshes: Dict[int, str]
    record_counts: Dict[str, int]
    file_size: int

    def find_cells(
        self,
        name_contains: Optional[str] = None,
        grid: Optional[Tuple[int, int]] = None,
        exterior: Optional[bool] = None,
        min_refs: int = 0,
    ) -> List[CellSummary]:
        out = []
        needle = (name_contains or "").lower()
        for cell in self.cells:
            if needle and needle not in cell.editor_id.lower():
                continue
            if grid is not None and cell.grid != grid:
                continue
            if exterior is not None and cell.exterior != exterior:
                continue
            if cell.ref_count < min_refs:
                continue
            out.append(cell)
        return out


def _u32(data: bytes, pos: int) -> int:
    return struct.unpack_from("<I", data, pos)[0]


def _i32(data: bytes, pos: int) -> int:
    return struct.unpack_from("<i", data, pos)[0]


def _subrecords(payload: bytes) -> Iterable[Tuple[bytes, bytes]]:
    pos = 0
    end = len(payload)
    while pos + 6 <= end:
        kind = payload[pos : pos + 4]
        size, = struct.unpack_from("<H", payload, pos + 4)
        start = pos + 6
        stop = start + size
        if stop > end:
            raise EsmError("subrecord %r runs past its record" % kind)
        yield kind, payload[start:stop]
        pos = stop


def _payload(data: bytes, flags: int, raw: bytes) -> bytes:
    if not (flags & COMPRESSED_FLAG):
        return raw
    if len(raw) < 4:
        raise EsmError("compressed record is shorter than its size prefix")
    expected, = struct.unpack_from("<I", raw, 0)
    try:
        out = zlib.decompress(raw[4:])
    except zlib.error as exc:
        raise EsmError("compressed record deflate error: %s" % exc) from exc
    if len(out) != expected:
        raise EsmError(
            "compressed record unpacked to %d bytes, expected %d" % (len(out), expected)
        )
    return out


def _decode_modl(payload: bytes) -> Optional[str]:
    for kind, sub in _subrecords(payload):
        if kind == b"MODL":
            text = sub.split(b"\x00", 1)[0].decode("cp1252", "replace")
            return text.strip()
    return None


def _archive_mesh_path(modl: str) -> str:
    if modl.lower().startswith("meshes\\"):
        return modl
    return "meshes\\" + modl.lstrip("\\/")


def _walk(
    data: bytes,
    start: int,
    end: int,
    *,
    parent_cell: Optional[CellSummary],
    state: "_ScanState",
    depth: int = 0,
) -> None:
    if depth > 32:
        raise EsmError("GRUP nesting deeper than 32")
    pos = start
    current_cell = parent_cell
    while pos + 24 <= end:
        kind = data[pos : pos + 4]
        if kind == b"GRUP":
            size, = struct.unpack_from("<I", data, pos + 4)
            if size < 24 or pos + size > end:
                raise EsmError("invalid GRUP size %d at %d" % (size, pos))
            _walk(
                data,
                pos + 24,
                pos + size,
                parent_cell=current_cell,
                state=state,
                depth=depth + 1,
            )
            pos += size
            continue

        data_size, flags, form_id = struct.unpack_from("<III", data, pos + 4)
        rec_end = pos + 24 + data_size
        if rec_end > end:
            raise EsmError("record %r at %d runs past its group" % (kind, pos))
        state.record_counts[kind.decode("latin-1", "replace")] = (
            state.record_counts.get(kind.decode("latin-1", "replace"), 0) + 1
        )
        state.records += 1
        if state.records > MAX_RECORDS:
            raise EsmError("record count exceeds the safety limit")

        raw = data[pos + 24 : rec_end]
        if kind == b"CELL":
            payload = _payload(data, flags, raw)
            editor_id = ""
            grid = None
            interior = False
            for sub_kind, sub in _subrecords(payload):
                if sub_kind == b"EDID":
                    editor_id = sub.split(b"\x00", 1)[0].decode("cp1252", "replace")
                elif sub_kind == b"XCLC" and len(sub) >= 8:
                    grid = struct.unpack_from("<ii", sub, 0)
                elif sub_kind == b"DATA" and sub:
                    interior = bool(sub[0] & 0x01)
            current_cell = CellSummary(
                form_id=form_id, editor_id=editor_id, grid=grid, interior=interior
            )
            state.cells.append(current_cell)
            if state.watch_form_id == form_id:
                state.watched_cell = current_cell
        elif kind == b"REFR" and current_cell is not None:
            current_cell.ref_count += 1
            if state.watch_form_id == current_cell.form_id:
                payload = _payload(data, flags, raw)
                base = 0
                position = (0.0, 0.0, 0.0)
                for sub_kind, sub in _subrecords(payload):
                    if sub_kind == b"NAME" and len(sub) >= 4:
                        base = struct.unpack_from("<I", sub, 0)[0]
                    elif sub_kind == b"DATA" and len(sub) >= 12:
                        position = struct.unpack_from("<fff", sub, 0)
                state.refs.append(
                    RefEntry(
                        form_id=form_id,
                        base_form_id=base,
                        position=position,
                        mesh=_archive_mesh_path(state.base_meshes[base])
                        if base in state.base_meshes
                        else None,
                    )
                )
        elif kind in PLACEABLE_TYPES:
            payload = _payload(data, flags, raw)
            modl = _decode_modl(payload)
            if modl:
                state.base_meshes[form_id] = modl
        pos = rec_end


@dataclass
class _ScanState:
    cells: List[CellSummary] = field(default_factory=list)
    base_meshes: Dict[int, str] = field(default_factory=dict)
    record_counts: Dict[str, int] = field(default_factory=dict)
    refs: List[RefEntry] = field(default_factory=list)
    records: int = 0
    watch_form_id: Optional[int] = None
    watched_cell: Optional[CellSummary] = None


def _open_mmap(path: str):
    handle = open(path, "rb")
    try:
        mm = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
    except Exception:
        handle.close()
        raise
    return handle, mm


def scan_plugin(path: str) -> PluginScan:
    """Walk a plugin once and collect cell summaries and base-object meshes."""
    handle, mm = _open_mmap(path)
    try:
        state = _ScanState()
        _walk(mm, 0, len(mm), parent_cell=None, state=state)
        return PluginScan(
            cells=state.cells,
            base_meshes=state.base_meshes,
            record_counts=state.record_counts,
            file_size=len(mm),
        )
    finally:
        mm.close()
        handle.close()


def extract_cell(
    path: str,
    *,
    editor_id: Optional[str] = None,
    form_id: Optional[int] = None,
    grid: Optional[Tuple[int, int]] = None,
    scan: Optional[PluginScan] = None,
) -> CellFiles:
    """Collect references and unique mesh paths for one cell."""
    if not scan:
        scan = scan_plugin(path)
    candidates = scan.find_cells(name_contains=editor_id, grid=grid)
    if form_id is not None:
        candidates = [cell for cell in candidates if cell.form_id == form_id]
    if not candidates:
        raise EsmError(
            "no cell matching editor_id=%r grid=%r form_id=%r"
            % (editor_id, grid, hex(form_id) if form_id is not None else None)
        )
    if len(candidates) > 1 and editor_id:
        exact = [cell for cell in candidates if cell.editor_id.lower() == editor_id.lower()]
        if exact:
            candidates = exact
    if len(candidates) > 1:
        raise EsmError(
            "ambiguous cell selector; candidates: %s"
            % ", ".join("%s (0x%08x)" % (c.editor_id, c.form_id) for c in candidates[:8])
        )
    target = candidates[0]
    state = _ScanState()
    state.base_meshes = dict(scan.base_meshes)
    state.watch_form_id = target.form_id
    handle, mm = _open_mmap(path)
    try:
        _walk(mm, 0, len(mm), parent_cell=None, state=state)
    finally:
        mm.close()
        handle.close()
    if state.watched_cell is None:
        raise EsmError("cell 0x%08x was not seen on the second pass" % target.form_id)
    meshes = []
    seen = set()
    for ref in state.refs:
        if ref.mesh:
            # MODL is relative to Data\Meshes; the archive stores "meshes\...".
            archive_path = ref.mesh
            if not archive_path.lower().startswith("meshes\\"):
                archive_path = "meshes\\" + archive_path.lstrip("\\/")
            key = archive_path.lower()
            if key not in seen:
                seen.add(key)
                meshes.append(archive_path)
    return CellFiles(summary=state.watched_cell, refs=state.refs, mesh_paths=sorted(meshes))
