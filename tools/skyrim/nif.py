"""Skyrim (Gamebryo 20.2.0.7) NIF header and block-table parser.

What is implemented
-------------------
* Header string, version, endianness, user version, ``bsVersion`` (83 for
  Skyrim LE), author/process/export strings.
* Block-type string table, block-type indices, block sizes, string table and
  the block table itself.
* Block-type histograms for any NIF.
* Vertex/triangle counts for ``NiTriShapeData`` (the geometry class Skyrim LE
  uses) reached from ``NiTriShape``/``NiTriStrips`` blocks.
* A printable-string scan for ``textures\\...dds`` paths (used by the budget
  model).  This is a scan, not a full material decode.

What is **not** implemented
---------------------------
* Full shape decoding (vertex buffers, triangle indices, skinning, tangent
  spaces), collision (``bhk*``), animation, particle and shader blocks.
* ``BSTriShape``/``BSSubIndexTriShape`` (Skyrim SE/Fallout 4 geometry layout)
  and ``NiTriStripsData`` strip walks: detected and reported as unsupported.
* Shader/material semantics: only embedded path strings are harvested.

Header layout observed on the shipped archives (all 400 sampled NIFs)
--------------------------------------------------------------------
::

    "Gamebryo File Format, Version 20.2.0.7\\n"   39 bytes (ends at newline)
    u32 version        0x14020007
    u8  endian         1
    u32 userVersion    12
    u32 numBlocks
    u32 bsVersion      83
    bstring author     (u8 length including NUL)
    bstring process
    bstring export
    u16 numBlockTypes
    numBlockTypes * (u32 length + chars, no NUL)
    u16 blockTypeIndex[numBlocks]
    u32 blockSize[numBlocks]
    u32 numStrings
    u32 maxStringLength
    numStrings * (u32 length + chars, no NUL)
    u32 reserved       (observed 0)
    block data         sum(blockSize) bytes
    u32 numRoots       (observed 1)
    u32 rootIndex      (observed 0)

Several public descriptions put the BS header after the string table and omit
the final root list; the layout above is what the files actually contain and it
accounts for every byte (verified by exact-size arithmetic).
"""

from __future__ import annotations

import os
import re
import struct
from collections import Counter
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

SUPPORTED_VERSION = 0x14020007  # 20.2.0.7
SKYRIM_USER_VERSION = 12
SKYRIM_BS_VERSION = 83
TRAILER_SIZE = 8
MAX_ARRAY = 1_000_000

GEOMETRY_DATA_TYPES = ("NiTriShapeData", "NiTriStripsData")
SHAPE_TYPES = ("NiTriShape", "NiTriStrips", "BSLODTriShape", "BSSegmentedTriShape")
LOD_SHAPE_TYPES = ("BSLODTriShape",)
UNSUPPORTED_GEOMETRY_TYPES = ("BSTriShape", "BSSubIndexTriShape", "NiTriStripsData")


class NifError(Exception):
    """Malformed NIF data."""


@dataclass(frozen=True)
class NifBlock:
    index: int
    type_name: str
    offset: int
    size: int

    def slice(self, data: bytes) -> bytes:
        return data[self.offset : self.offset + self.size]


@dataclass
class NifFile:
    version: int
    user_version: int
    bs_version: int
    endian: int
    header_string: str
    author: str
    process_script: str
    export_script: str
    block_types: List[str]
    strings: List[str]
    blocks: List[NifBlock]
    data_size: int
    header_size: int
    trailer: bytes
    raw: bytes = field(repr=False, default=b"")

    # -- reporting --------------------------------------------------------

    def histogram(self) -> Dict[str, int]:
        return dict(Counter(block.type_name for block in self.blocks).most_common())

    def block(self, index: int) -> NifBlock:
        if index < 0 or index >= len(self.blocks):
            raise NifError("block index %d out of range (0..%d)" % (index, len(self.blocks) - 1))
        return self.blocks[index]

    def geometry_stats(self) -> dict:
        """Vertex/triangle counts for the geometry classes we understand."""
        shapes: List[dict] = []
        totals = {"vertices": 0, "triangles": 0, "shapes": 0, "unparsed": 0}
        base_totals = {"vertices": 0, "triangles": 0, "shapes": 0}
        lod_totals = {"vertices": 0, "triangles": 0, "shapes": 0}
        unsupported_blocks = {
            name: 0 for name in UNSUPPORTED_GEOMETRY_TYPES
        }
        data_cache: Dict[int, dict] = {}
        for block in self.blocks:
            if block.type_name in unsupported_blocks:
                unsupported_blocks[block.type_name] += 1
        for index, block in enumerate(self.blocks):
            if block.type_name not in SHAPE_TYPES:
                continue
            try:
                data_ref = _parse_shape_data_ref(block.slice(self.raw), block.size)
            except NifError:
                totals["unparsed"] += 1
                continue
            if data_ref is None or data_ref < 0 or data_ref >= len(self.blocks):
                totals["unparsed"] += 1
                continue
            target = self.blocks[data_ref]
            if target.type_name != "NiTriShapeData":
                totals["unparsed"] += 1
                shapes.append(
                    {
                        "shape": block.type_name,
                        "data": target.type_name,
                        "supported": False,
                    }
                )
                continue
            parsed = data_cache.get(data_ref)
            if parsed is None:
                try:
                    parsed = parse_nitrishapedata(target.slice(self.raw), target.size)
                except NifError as exc:
                    parsed = {"error": str(exc)}
                data_cache[data_ref] = parsed
            if "error" in parsed:
                totals["unparsed"] += 1
                continue
            totals["vertices"] += parsed["num_vertices"]
            totals["triangles"] += parsed["num_triangles"]
            totals["shapes"] += 1
            bucket = lod_totals if block.type_name in LOD_SHAPE_TYPES else base_totals
            bucket["vertices"] += parsed["num_vertices"]
            bucket["triangles"] += parsed["num_triangles"]
            bucket["shapes"] += 1
            shapes.append(
                {
                    "shape": block.type_name,
                    "data": target.type_name,
                    "data_block": data_ref,
                    "vertices": parsed["num_vertices"],
                    "triangles": parsed["num_triangles"],
                    "supported": True,
                }
            )
        return {
            "totals": totals,
            "base_totals": base_totals,
            "lod_totals": lod_totals,
            "shapes": shapes,
            "unsupported_blocks": unsupported_blocks,
        }

    def texture_paths(self) -> List[str]:
        return scan_texture_paths(self.raw)

    def describe(self) -> dict:
        hist = self.histogram()
        return {
            "version": "0x%08x" % self.version,
            "version_tuple": _version_tuple(self.version),
            "user_version": self.user_version,
            "bs_version": self.bs_version,
            "endian": self.endian,
            "block_count": len(self.blocks),
            "block_type_count": len(self.block_types),
            "string_count": len(self.strings),
            "file_size": self.data_size,
            "header_size": self.header_size,
            "block_type_histogram": hist,
        }


def _version_tuple(version: int) -> str:
    return "%d.%d.%d.%d" % (
        (version >> 24) & 0xFF,
        (version >> 16) & 0xFF,
        (version >> 8) & 0xFF,
        version & 0xFF,
    )


def parse_nif(data: bytes) -> NifFile:
    """Parse the header, tables and block table of a Skyrim NIF."""
    if len(data) < 40:
        raise NifError("file too short for a NIF header")
    newline = data.find(b"\n", 0, 64)
    if newline <= 0:
        raise NifError("missing NIF header string")
    header_string = data[: newline + 1]
    try:
        header_text = header_string.decode("ascii").rstrip("\n\r")
    except UnicodeDecodeError as exc:
        raise NifError("header string is not ASCII") from exc
    if "Gamebryo File Format" not in header_text and "NetImmerse File Format" not in header_text:
        raise NifError("not a NIF header: %r" % header_string[:64])

    pos = newline + 1
    version, = _unpack("<I", data, pos)
    pos += 4
    endian = data[pos]
    pos += 1
    if endian != 1:
        raise NifError("big-endian NIFs are not supported (endian=%d)" % endian)
    user_version, = _unpack("<I", data, pos)
    pos += 4
    num_blocks, bs_version = _unpack("<II", data, pos)
    pos += 8
    author = _read_bstring(data, pos)
    pos += author[1]
    process = _read_bstring(data, pos)
    pos += process[1]
    export = _read_bstring(data, pos)
    pos += export[1]

    num_types, = _unpack("<H", data, pos)
    pos += 2
    if num_types > MAX_ARRAY:
        raise NifError("implausible block-type count %d" % num_types)
    block_types: List[str] = []
    for i in range(num_types):
        length, = _unpack("<I", data, pos)
        pos += 4
        if length > 4096 or pos + length > len(data):
            raise NifError("bad block-type string %d length %d" % (i, length))
        try:
            block_types.append(data[pos : pos + length].decode("ascii"))
        except UnicodeDecodeError as exc:
            raise NifError("block-type string %d is not ASCII" % i) from exc
        pos += length

    if num_blocks > MAX_ARRAY:
        raise NifError("implausible block count %d" % num_blocks)
    raw_indices = data[pos : pos + 2 * num_blocks]
    if len(raw_indices) != 2 * num_blocks:
        raise NifError("truncated block-type index table")
    type_indices = list(struct.unpack("<%dH" % num_blocks, raw_indices))
    pos += 2 * num_blocks
    raw_sizes = data[pos : pos + 4 * num_blocks]
    if len(raw_sizes) != 4 * num_blocks:
        raise NifError("truncated block-size table")
    block_sizes = list(struct.unpack("<%dI" % num_blocks, raw_sizes))
    pos += 4 * num_blocks

    num_strings, max_string_len = _unpack("<II", data, pos)
    pos += 8
    if num_strings > MAX_ARRAY:
        raise NifError("implausible string count %d" % num_strings)
    strings: List[str] = []
    for i in range(num_strings):
        length, = _unpack("<I", data, pos)
        pos += 4
        if length > max(65536, max_string_len + 1) or pos + length > len(data):
            raise NifError("bad string-table entry %d length %d" % (i, length))
        try:
            strings.append(data[pos : pos + length].decode("cp1252"))
        except UnicodeDecodeError as exc:
            raise NifError("string-table entry %d is not decodable" % i) from exc
        pos += length

    reserved, = _unpack("<I", data, pos)
    pos += 4
    blocks_start = pos
    total_block_bytes = sum(block_sizes)
    blocks_end = blocks_start + total_block_bytes
    if blocks_end + TRAILER_SIZE > len(data):
        raise NifError(
            "block data runs past EOF: %d + %d > %d"
            % (blocks_start, total_block_bytes, len(data))
        )
    trailer = data[blocks_end : blocks_end + TRAILER_SIZE]
    if len(trailer) != TRAILER_SIZE:
        raise NifError("missing 8-byte NIF trailer")
    extra = len(data) - (blocks_end + TRAILER_SIZE)
    if extra != 0:
        raise NifError("unexpected %d trailing bytes after the NIF trailer" % extra)

    blocks: List[NifBlock] = []
    offset = blocks_start
    for i in range(num_blocks):
        if type_indices[i] >= len(block_types):
            raise NifError("block %d has type index %d out of range" % (i, type_indices[i]))
        size = block_sizes[i]
        if size < 0 or offset + size > blocks_end:
            raise NifError("block %d has an invalid size %d" % (i, size))
        blocks.append(NifBlock(i, block_types[type_indices[i]], offset, size))
        offset += size

    return NifFile(
        version=version,
        user_version=user_version,
        bs_version=bs_version,
        endian=endian,
        header_string=header_text,
        author=author[0],
        process_script=process[0],
        export_script=export[0],
        block_types=block_types,
        strings=strings,
        blocks=blocks,
        data_size=len(data),
        header_size=blocks_start,
        trailer=trailer,
        raw=data,
    )


def parse_nif_file(path: str) -> NifFile:
    with open(path, "rb") as handle:
        return parse_nif(handle.read())


# ---------------------------------------------------------------------------
# Small readers
# ---------------------------------------------------------------------------

def _unpack(fmt: str, data: bytes, pos: int):
    size = struct.calcsize(fmt)
    if pos + size > len(data):
        raise NifError("truncated read at %d (%s)" % (pos, fmt))
    return struct.unpack_from(fmt, data, pos)


def _read_bstring(data: bytes, pos: int) -> Tuple[str, int]:
    if pos >= len(data):
        raise NifError("truncated bstring")
    length = data[pos]
    if length == 0 or pos + 1 + length > len(data):
        raise NifError("bad bstring at %d (length %d)" % (pos, length))
    raw = data[pos + 1 : pos + 1 + length]
    if raw[-1:] == b"\x00":
        raw = raw[:-1]
    try:
        text = raw.decode("cp1252")
    except UnicodeDecodeError as exc:
        raise NifError("bstring at %d is not decodable" % pos) from exc
    return text, 1 + length


def _read_i32(data: bytes, pos: int) -> int:
    return _unpack("<i", data, pos)[0]


def _read_u32(data: bytes, pos: int) -> int:
    return _unpack("<I", data, pos)[0]


def _parse_shape_data_ref(block: bytes, size: int) -> Optional[int]:
    """Return the geometry data block ref of a NiTriShape/NiTriStrips."""
    pos = 0
    name = _read_i32(block, pos)
    pos += 4
    extra_count = _read_u32(block, pos)
    pos += 4
    if extra_count > 4096:
        raise NifError("implausible extra-data count %d" % extra_count)
    pos += 4 * extra_count
    controller = _read_i32(block, pos)
    pos += 4
    flags = _read_u32(block, pos)
    pos += 4
    # MatTransform: translation (3f), rotation (9f), scale (1f)
    pos += 13 * 4
    collision = _read_i32(block, pos)
    pos += 4
    if pos + 4 > len(block):
        raise NifError("shape block is too short for its fields")
    data_ref = _read_i32(block, pos)
    return data_ref


def parse_nitrishapedata(block: bytes, size: int) -> dict:
    """Decode the scalar counts of a Skyrim ``NiTriShapeData`` block.

    The full vertex/index payload is deliberately not materialised; the walk
    skips it using the counts and flags, and the block must be consumed almost
    exactly (only the triangle/match-group tail remains).
    """
    pos = 0
    group_id, = _unpack("<I", block, pos)
    pos += 4
    num_vertices, = _unpack("<H", block, pos)
    pos += 2
    if pos + 3 > len(block):
        raise NifError("truncated NiGeometryData flags")
    keep_flags = block[pos]
    compress_flags = block[pos + 1]
    has_vertices = block[pos + 2]
    pos += 3
    if has_vertices:
        pos += num_vertices * 12
    data_flags, = _unpack("<H", block, pos)
    pos += 2
    material_crc, = _unpack("<I", block, pos)
    pos += 4
    has_normals = block[pos]
    pos += 1
    if has_normals:
        pos += num_vertices * 12
        if data_flags & 0xF000:
            pos += num_vertices * 24
    pos += 16  # BoundingSphere
    has_colors = block[pos]
    pos += 1
    if has_colors:
        pos += num_vertices * 16
    num_uv_sets = data_flags & 1  # stream version >= 34
    if num_uv_sets:
        pos += num_uv_sets * num_vertices * 8
    pos += 2  # consistencyFlags (uint16)
    additional_ref, = _unpack("<i", block, pos)
    pos += 4
    num_triangles, = _unpack("<H", block, pos)
    pos += 2
    if pos > len(block):
        raise NifError("NiTriShapeData header overruns the block")
    # Remaining tail: numTrianglePoints u32, hasTriangles u8, indices, groups.
    num_triangle_points, = _unpack("<I", block, pos)
    pos += 4
    has_triangles = block[pos]
    pos += 1
    if has_triangles:
        pos += num_triangles * 6
    num_match_groups, = _unpack("<H", block, pos)
    pos += 2
    for _ in range(num_match_groups):
        count, = _unpack("<H", block, pos)
        pos += 2 + count * 2
    if pos != len(block):
        raise NifError(
            "NiTriShapeData consumed %d of %d bytes (flags=0x%04x)"
            % (pos, len(block), data_flags)
        )
    return {
        "num_vertices": num_vertices,
        "num_triangles": num_triangles,
        "num_triangle_points": num_triangle_points,
        "group_id": group_id,
        "keep_flags": keep_flags,
        "compress_flags": compress_flags,
        "data_flags": data_flags,
        "has_vertices": bool(has_vertices),
        "has_normals": bool(has_normals),
        "has_colors": bool(has_colors),
        "material_crc": material_crc,
        "additional_data_ref": additional_ref,
    }


# ---------------------------------------------------------------------------
# Texture path scan
# ---------------------------------------------------------------------------

_TEXTURE_RE = re.compile(rb"(?i:textures[\\/][ -~]{1,200}?\.(?:dds|tga))")


def scan_texture_paths(data: bytes) -> List[str]:
    """Return unique texture paths mentioned anywhere in the NIF bytes.

    Skyrim stores them as ASCII strings inside ``BSShaderTextureSet`` blocks.
    We do not decode the block layout; the printable-run scan is stable across
    the shipped meshes and is explicitly a scan, not a material parser.
    """
    found: Dict[str, str] = {}
    for match in _TEXTURE_RE.finditer(data):
        raw = match.group(0)
        if any(byte < 0x20 or byte > 0x7E for byte in raw):
            continue
        text = raw.decode("ascii").replace("/", "\\")
        found.setdefault(text.lower(), text)
    return sorted(found.values())
