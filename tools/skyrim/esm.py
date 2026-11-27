"""Minimal, read-only TES4/ESM record walker for per-cell asset lists.

This is not a game-data implementation: it extracts just enough to answer
"which meshes does one cell instance, and therefore which textures does it
pull in".  It understands the TES4 record/GRUP framing, compressed records
(zlib, 4-byte unpacked size prefix), WRLD ``EDID``/``DATA``/``NAM0``/``NAM9``,
CELL ``EDID``/``XCLC``/``DATA`` and REFR ``NAME``/``DATA`` subrecords, and
``MODL`` on the common placeable base objects.  Exterior CELLs inherit the
owning WRLD form id from the GRUP nesting (Exterior Cell Block/Sub-Block).

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
    world_form_id: int = 0

    @property
    def exterior(self) -> bool:
        return self.grid is not None

    def as_dict(self) -> dict:
        return {
            "form_id": "0x%08x" % self.form_id,
            "editor_id": self.editor_id,
            "grid": list(self.grid) if self.grid else None,
            "interior": self.interior,
            "world_form_id": "0x%08x" % self.world_form_id,
            "refs": self.ref_count,
        }


@dataclass
class WorldSummary:
    """A WRLD record and the grid bounds it declares.

    Skyrim stores the bounds in ``NAM0`` (min x/y) and ``NAM9`` (max x/y) as
    two floats each; ``DATA`` is a one-byte flag word, not the bounds.  Both
    are recorded so a caller can choose a window without guessing.
    """

    form_id: int
    editor_id: str = ""
    flags: int = 0
    bounds: Optional[Tuple[float, float, float, float]] = None

    def as_dict(self) -> dict:
        return {
            "form_id": "0x%08x" % self.form_id,
            "editor_id": self.editor_id,
            "flags": self.flags,
            "bounds": list(self.bounds) if self.bounds else None,
        }


@dataclass
class RefEntry:
    form_id: int
    base_form_id: int
    position: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    mesh: Optional[str] = None
    texture: Optional[str] = None


@dataclass
class CellFiles:
    summary: CellSummary
    refs: List[RefEntry]
    mesh_paths: List[str]
    texture_paths: List[str] = field(default_factory=list)

    def as_dict(self) -> dict:
        return {
            "cell": self.summary.as_dict(),
            "ref_count": len(self.refs),
            "mesh_count": len(self.mesh_paths),
            "meshes": self.mesh_paths,
            "texture_count": len(self.texture_paths),
            "textures": self.texture_paths,
        }


@dataclass
class PluginScan:
    cells: List[CellSummary]
    base_meshes: Dict[int, str]
    record_counts: Dict[str, int]
    file_size: int
    base_textures: Dict[int, str] = field(default_factory=dict)
    worlds: List[WorldSummary] = field(default_factory=list)

    def find_worlds(self, selector: str) -> List[WorldSummary]:
        """Match a WRLD by ``0x``/decimal form id or editor id."""
        form_id = None
        text = selector
        try:
            form_id = int(selector, 16) if selector.lower().startswith("0x") \
                else int(selector, 10)
            text = None
        except ValueError:
            pass
        out = []
        for world in self.worlds:
            if form_id is not None and world.form_id == form_id:
                out.append(world)
            elif text and world.editor_id.lower() == text.lower():
                out.append(world)
        return out

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


def _decode_tx00(payload: bytes) -> Optional[str]:
    """TXST stores its diffuse path in TX00 ("textures\\...dds")."""
    for kind, sub in _subrecords(payload):
        if kind == b"TX00":
            text = sub.split(b"\x00", 1)[0].decode("cp1252", "replace")
            text = text.strip()
            if not text:
                return None
            text = text.replace("/", "\\").lstrip("\\")
            if not text.lower().startswith("textures\\"):
                text = "textures\\" + text
            return text
    return None


def _decode_world(payload: bytes):
    """Return ``(editor_id, flags, bounds)`` from a WRLD record payload.

    A few test worldspaces (e.g. ``CWTestHold`` 0x1b44a in Skyrim.esm) carry
    trailing padding that does not form valid subrecords, so this scan stops
    at the first malformed one instead of rejecting the whole plugin.  The
    fields this tool needs appear before that padding.
    """
    editor_id = ""
    flags = 0
    nam0 = None
    nam9 = None
    pos = 0
    end = len(payload)
    while pos + 6 <= end:
        kind = payload[pos:pos + 4]
        size, = struct.unpack_from("<H", payload, pos + 4)
        start = pos + 6
        stop = start + size
        if stop > end:
            break
        sub = payload[start:stop]
        if kind == b"EDID":
            editor_id = sub.split(b"\x00", 1)[0].decode("cp1252", "replace")
        elif kind == b"DATA" and sub:
            flags = sub[0]
        elif kind == b"NAM0" and len(sub) >= 8:
            nam0 = struct.unpack_from("<ff", sub, 0)
        elif kind == b"NAM9" and len(sub) >= 8:
            nam9 = struct.unpack_from("<ff", sub, 0)
        pos = stop
    bounds = None
    if nam0 is not None or nam9 is not None:
        low = nam0 or (0.0, 0.0)
        high = nam9 or (0.0, 0.0)
        bounds = (low[0], low[1], high[0], high[1])
    return editor_id, flags, bounds


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
    parent_world: int = 0,
    exterior_group: bool = False,
) -> None:
    if depth > 32:
        raise EsmError("GRUP nesting deeper than 32")
    pos = start
    current_cell = parent_cell
    current_world = parent_world
    in_exterior = exterior_group
    while pos + 24 <= end:
        kind = data[pos : pos + 4]
        if kind == b"GRUP":
            size, = struct.unpack_from("<I", data, pos + 4)
            if size < 24 or pos + size > end:
                raise EsmError("invalid GRUP size %d at %d" % (size, pos))
            group_type, = struct.unpack_from("<i", data, pos + 12)
            _walk(
                data,
                pos + 24,
                pos + size,
                parent_cell=current_cell,
                state=state,
                depth=depth + 1,
                parent_world=current_world,
                # TES5 exterior cells live under "Exterior Cell Block" (4)
                # and "Exterior Cell Sub-Block" (5).  The world's persistent
                # CELL sits directly under World Children (1) and carries a
                # placeholder XCLC 0,0; it must not claim that grid.
                exterior_group=in_exterior or group_type in (4, 5),
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
        if kind == b"WRLD":
            payload = _payload(data, flags, raw)
            editor_id, world_flags, bounds = _decode_world(payload)
            state.worlds.append(WorldSummary(
                form_id=form_id, editor_id=editor_id, flags=world_flags,
                bounds=bounds))
            # Exterior CELL records live in descendant GRUPs of this WRLD.
            current_world = form_id
        elif kind == b"CELL":
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
                form_id=form_id, editor_id=editor_id, grid=grid,
                interior=interior,
                world_form_id=0 if (interior or not in_exterior)
                                else current_world,
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
                        texture=state.base_textures.get(base),
                    )
                )
        elif kind in PLACEABLE_TYPES:
            payload = _payload(data, flags, raw)
            modl = _decode_modl(payload)
            if modl:
                state.base_meshes[form_id] = modl
            if kind == b"TXST":
                texture = _decode_tx00(payload)
                if texture:
                    state.base_textures[form_id] = texture
        pos = rec_end


@dataclass
class _ScanState:
    cells: List[CellSummary] = field(default_factory=list)
    base_meshes: Dict[int, str] = field(default_factory=dict)
    base_textures: Dict[int, str] = field(default_factory=dict)
    worlds: List[WorldSummary] = field(default_factory=list)
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
            base_textures=state.base_textures,
            worlds=state.worlds,
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
    state.base_textures = dict(scan.base_textures)
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
    textures = []
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
        if ref.texture:
            key = ref.texture.lower()
            if key not in seen:
                seen.add(key)
                textures.append(ref.texture)
    return CellFiles(summary=state.watched_cell, refs=state.refs,
                    mesh_paths=sorted(meshes),
                    texture_paths=sorted(textures))
