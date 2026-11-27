#!/usr/bin/env python3
"""Read-only CPU rasterizer for d9_capture v3 draw snapshots.

The binary capture is authoritative; a sibling .txt file is not required.  No
GPU, Pillow, NumPy, or other third-party package is used.  PNG output is written
with the standard library only.
"""

from __future__ import annotations

import argparse
from array import array
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
import math
import os
from pathlib import Path
import struct
import sys
import time
from typing import Sequence
import zlib


# Values confirmed by source/d3d9_core.h and source/d3d9_vertex.h.
TRIANGLE_LIST = 4
TRIANGLE_STRIP = 5
TRIANGLE_FAN = 6
A8R8G8B8 = 21
X8R8G8B8 = 22
A8 = 28
DXT1 = 0x31545844  # BC1
DXT5 = 0x35545844  # BC3
D9_VERTEX_STRIDE = 20       # D9Vertex: five little-endian float32 values.
D9_COLOR_VERTEX_STRIDE = 40 # D9ColorVertex: xyz, uv, rgba, w (all float32).

SECTION_DRAW = 1
SECTION_VIEWPORT = 2
SECTION_DRAWSTATE = 3
SECTION_WORLD = 4
SECTION_VIEW = 5
SECTION_PROJECTION = 6
SECTION_VS = 7
SECTION_PS = 8
SECTION_DECLARATION = 9
SECTION_STREAM = 10
SECTION_VERTICES = 11
SECTION_INDICES = 12
SECTION_TEXTURE = 13
SECTION_FRAME = 14
SECTION_END = 15
SECTION_TARGET = 16

USAGE_POSITION = 0
USAGE_POSITIONT = 9

TRUNCATION_NAMES = (
    (1 << 0, "vertices"),
    (1 << 1, "indices"),
    (1 << 2, "vs"),
    (1 << 3, "ps"),
    (1 << 4, "texture"),
    (1 << 5, "texture_total"),
    (1 << 6, "streams"),
    (1 << 7, "textures"),
    (1 << 8, "declaration"),
)

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3


class CaptureError(ValueError):
    """A named malformed/unsupported capture condition."""


@dataclass(frozen=True)
class DrawInfo:
    type: int
    start: int
    primitives: int
    indexed: int
    base_vertex: int
    min_vertex: int
    vertex_count: int
    start_index: int


@dataclass(frozen=True)
class Viewport:
    x: int
    y: int
    width: int
    height: int
    minz: float
    maxz: float


@dataclass(frozen=True)
class DrawState:
    z_enable: int = 0
    z_write: int = 0
    z_func: int = 4
    cull: int = 1
    alpha_test: int = 0
    alpha_func: int = 8
    alpha_ref: int = 0
    blend: int = 0
    src_blend: int = 2
    dst_blend: int = 1
    blend_op: int = 1
    color_write: int = 15
    address_u: int = 3
    address_v: int = 3
    mag_filter: int = 2
    min_filter: int = 2
    mip_filter: int = 2
    max_mip: int = 0
    resident_mips: int = 0
    pixel_shader: int = 0
    address_w: int = 1
    lod_bias: float = 0.0
    max_anisotropy: int = 1
    srgb: int = 0
    border_color: int = 0


@dataclass(frozen=True)
class VertexElement:
    stream: int
    offset: int
    type: int
    method: int
    usage: int
    usage_index: int


@dataclass(frozen=True)
class CapturedTexture:
    stage: int
    width: int
    height: int
    format: int
    level: int
    row_pitch: int
    pixels: bytes


@dataclass
class Capture:
    path: Path
    version: int
    signature: int
    truncation: int
    frame_index: int
    draw_index: int
    draw: DrawInfo
    viewport: Viewport
    state: DrawState
    world: tuple[tuple[float, ...], ...]
    view: tuple[tuple[float, ...], ...]
    projection: tuple[tuple[float, ...], ...]
    vs_code: bytes
    ps_code: bytes
    declaration_active: int
    declaration_position: int
    declaration_uv: int
    elements: tuple[VertexElement, ...]
    vertex_stride: int
    vertices: bytes
    index_size: int
    indices: bytes
    textures: tuple[CapturedTexture, ...]
    clear_valid: int
    clear_color: int
    clear_flags: int
    target_width: int
    target_height: int
    target_format: int
    warnings: list[str] = field(default_factory=list)

    @property
    def width(self) -> int:
        return self.target_width or self.viewport.width

    @property
    def height(self) -> int:
        return self.target_height or self.viewport.height


@dataclass(frozen=True)
class ShaderVertex:
    x: float
    y: float
    z: float
    u: float
    v: float
    r: float
    g: float
    b: float
    a: float
    inv_w: float


@dataclass
class RenderStats:
    triangles: int = 0
    culled: int = 0
    clipped: int = 0
    fragments: int = 0


class Reader:
    def __init__(self, data: bytes, context: str):
        self.data = data
        self.offset = 0
        self.context = context

    def take(self, size: int) -> bytes:
        if size < 0 or self.offset + size > len(self.data):
            raise CaptureError(
                f"{self.context}: payload is short at offset {self.offset} "
                f"(need {size}, have {len(self.data) - self.offset})"
            )
        out = self.data[self.offset:self.offset + size]
        self.offset += size
        return out

    def finish(self) -> None:
        if self.offset != len(self.data):
            raise CaptureError(
                f"{self.context}: {len(self.data) - self.offset} unexpected payload bytes"
            )

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def i32(self) -> int:
        return struct.unpack("<i", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def f32(self) -> float:
        return struct.unpack("<f", self.take(4))[0]


def fnv1a(data: bytes) -> int:
    value = FNV64_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def _require_size(reader: Reader, expected: int, name: str) -> None:
    if len(reader.data) != expected:
        raise CaptureError(
            f"{reader.context}: {name} payload is {len(reader.data)} bytes, expected {expected}"
        )


def _parse_matrix(payload: bytes, name: str) -> tuple[tuple[float, ...], ...]:
    _require_size(Reader(payload, name), 64, name)
    values = struct.unpack("<16f", payload)
    matrix = tuple(tuple(values[row * 4:row * 4 + 4]) for row in range(4))
    if not all(math.isfinite(v) for row in matrix for v in row):
        raise CaptureError(f"{name}: matrix contains a non-finite value")
    return matrix


def _parse_shader(payload: bytes, name: str) -> bytes:
    reader = Reader(payload, name)
    if len(payload) < 32:
        raise CaptureError(f"{name}: shader metadata is shorter than 32 bytes")
    reader.u32()  # object
    reader.u32()  # reserved
    expected_hash = reader.u64()
    reader.u64()  # declared bytes
    stored = reader.u32()
    truncated = reader.u32()
    code = reader.take(stored)
    reader.finish()
    if fnv1a(code) != expected_hash:
        raise CaptureError(f"{name}: stored shader bytes fail their FNV-1a hash")
    if truncated and not code:
        raise CaptureError(f"{name}: truncated shader has no stored bytecode")
    return code


def _parse_vertices(payload: bytes) -> tuple[int, bytes]:
    reader = Reader(payload, "VERTICES")
    stride = reader.u32()
    declared_count = reader.u32()
    captured_count = reader.u32()
    stored = reader.u32()
    declared_bytes = reader.u64()
    expected_hash = reader.u64()
    truncated = reader.u32()
    reader.u32()  # reserved
    data = reader.take(stored)
    reader.finish()
    if not stride or captured_count * stride != stored:
        raise CaptureError(
            f"VERTICES: stride {stride}, captured {captured_count}, stored {stored} are inconsistent"
        )
    if stored > declared_bytes or truncated != int(stored < declared_bytes):
        raise CaptureError("VERTICES: declared/stored/truncated metadata is inconsistent")
    if declared_count < captured_count:
        raise CaptureError("VERTICES: declared count is smaller than captured count")
    if fnv1a(data) != expected_hash:
        raise CaptureError("VERTICES: stored vertex bytes fail their FNV-1a hash")
    return stride, data


def _parse_indices(payload: bytes) -> tuple[int, bytes]:
    reader = Reader(payload, "INDICES")
    size = reader.u32()
    declared_count = reader.u32()
    captured_count = reader.u32()
    stored = reader.u32()
    declared_bytes = reader.u64()
    expected_hash = reader.u64()
    truncated = reader.u32()
    reader.u32()  # reserved
    data = reader.take(stored)
    reader.finish()
    if size == 0:
        if declared_count or captured_count or stored or declared_bytes or truncated:
            raise CaptureError("INDICES: zero index size has nonzero count/size metadata")
        if fnv1a(data) != expected_hash:
            raise CaptureError("INDICES: empty index payload fails its FNV-1a hash")
        return 0, b""
    if size not in (2, 4) or captured_count * size != stored:
        raise CaptureError(
            f"INDICES: size {size}, captured {captured_count}, stored {stored} are inconsistent"
        )
    if stored > declared_bytes or truncated != int(stored < declared_bytes):
        raise CaptureError("INDICES: declared/stored/truncated metadata is inconsistent")
    if declared_count < captured_count:
        raise CaptureError("INDICES: declared count is smaller than captured count")
    if fnv1a(data) != expected_hash:
        raise CaptureError("INDICES: stored index bytes fail their FNV-1a hash")
    return size, data


def _parse_texture(payload: bytes, index: int) -> CapturedTexture:
    name = f"TEXTURE[{index}]"
    reader = Reader(payload, name)
    stage = reader.u32()
    reader.u32()  # handle
    width = reader.u32()
    height = reader.u32()
    format_id = reader.u32()
    reader.u32()  # levels
    reader.u32()  # pool
    reader.u32()  # usage
    level = reader.u32()
    truncated = reader.u32()
    reader.u32()  # reserved
    row_pitch = reader.u64()
    declared_bytes = reader.u64()
    stored = reader.u64()
    expected_hash = reader.u64()
    pixels = reader.take(stored)
    reader.finish()
    if not width or not height:
        raise CaptureError(f"{name}: zero texture dimension")
    if stored > declared_bytes or truncated != int(stored < declared_bytes):
        raise CaptureError(f"{name}: declared/stored/truncated metadata is inconsistent")
    if fnv1a(pixels) != expected_hash:
        raise CaptureError(f"{name}: stored pixels fail their FNV-1a hash")
    if row_pitch > 0xFFFFFFFFFFFFFFFF:
        raise CaptureError(f"{name}: impossible row pitch")
    return CapturedTexture(stage, width, height, format_id, level, row_pitch, pixels)


def parse_capture(data: bytes, path: Path | str = "<memory>") -> Capture:
    path = Path(path)
    if len(data) < 32:
        raise CaptureError(f"{path}: file is shorter than the 32-byte header")
    if data[:8] == b"D9CAPPRE":
        raise CaptureError(f"{path}: Present event has no draw/pixel payload")
    if data[:8] != b"D9CAPBIN":
        raise CaptureError(f"{path}: bad magic {data[:8]!r}; expected D9CAPBIN")
    version, header_bytes, signature, truncation, section_count = struct.unpack_from(
        "<IIQII", data, 8
    )
    if not 1 <= version <= 3:
        raise CaptureError(f"{path}: unsupported d9_capture version {version}")
    if header_bytes != 32:
        raise CaptureError(f"{path}: header bytes is {header_bytes}, expected 32")

    sections: list[tuple[int, bytes]] = []
    offset = 32
    for number in range(section_count):
        if offset + 8 > len(data):
            raise CaptureError(f"{path}: missing section header {number}")
        section_id, size = struct.unpack_from("<II", data, offset)
        offset += 8
        if offset + size > len(data):
            raise CaptureError(f"{path}: section {section_id} extends past end of file")
        sections.append((section_id, data[offset:offset + size]))
        offset += size
    if offset != len(data):
        raise CaptureError(f"{path}: {len(data) - offset} trailing bytes after sections")
    if not sections or sections[-1] != (SECTION_END, b""):
        raise CaptureError(f"{path}: final section is not an empty END section")

    by_id: dict[int, list[bytes]] = {}
    for section_id, payload in sections:
        by_id.setdefault(section_id, []).append(payload)

    def one(section_id: int, name: str, size: int | None = None) -> bytes:
        values = by_id.get(section_id)
        if not values:
            raise CaptureError(f"{path}: missing {name} section")
        if len(values) != 1:
            raise CaptureError(f"{path}: duplicate {name} section")
        if size is not None and len(values[0]) != size:
            raise CaptureError(
                f"{path}: {name} payload is {len(values[0])} bytes, expected {size}"
            )
        return values[0]

    draw_payload = one(SECTION_DRAW, "DRAW", 32)
    values = struct.unpack("<8I", draw_payload)
    draw = DrawInfo(values[0], values[1], values[2], values[3],
                    struct.unpack("<i", struct.pack("<I", values[4]))[0],
                    values[5], values[6], values[7])

    vp_payload = one(SECTION_VIEWPORT, "VIEWPORT", 24)
    vx, vy, vw, vh = struct.unpack_from("<4I", vp_payload, 0)
    minz, maxz = struct.unpack_from("<2f", vp_payload, 16)
    viewport = Viewport(vx, vy, vw, vh, minz, maxz)
    if not vw or not vh:
        raise CaptureError(f"{path}: viewport has a zero dimension")
    if not math.isfinite(minz) or not math.isfinite(maxz) or maxz <= minz:
        raise CaptureError(f"{path}: invalid viewport depth range")

    state_size = 100 if version >= 3 else 80
    state_payload = one(SECTION_DRAWSTATE, "DRAWSTATE", state_size)
    state_values = struct.unpack("<25I", state_payload)
    lod_bits = state_values[21] if version >= 3 else 0
    lod_bias = struct.unpack("<f", struct.pack("<I", lod_bits))[0]
    if not math.isfinite(lod_bias):
        raise CaptureError(f"{path}: non-finite sampler LOD bias")
    state = DrawState(
        *state_values[:20],
        address_w=state_values[20] if version >= 3 else 1,
        lod_bias=lod_bias,
        max_anisotropy=state_values[22] if version >= 3 else 1,
        srgb=state_values[23] if version >= 3 else 0,
        border_color=state_values[24] if version >= 3 else 0,
    )

    world = _parse_matrix(one(SECTION_WORLD, "WORLD"), f"{path}: WORLD")
    view = _parse_matrix(one(SECTION_VIEW, "VIEW"), f"{path}: VIEW")
    projection = _parse_matrix(one(SECTION_PROJECTION, "PROJECTION"), f"{path}: PROJECTION")
    vs_code = _parse_shader(one(SECTION_VS, "VS"), f"{path}: VS")
    ps_code = _parse_shader(one(SECTION_PS, "PS"), f"{path}: PS")

    decl_payload = one(SECTION_DECLARATION, "DECLARATION")
    decl = Reader(decl_payload, f"{path}: DECLARATION")
    if len(decl_payload) < 84:
        raise CaptureError(f"{path}: DECLARATION payload is shorter than 84 bytes")
    declaration_active = decl.u32()
    declaration_count = decl.u32()
    declaration_position = decl.u32()
    declaration_uv = decl.u32()
    decl.u32()  # stream mask
    for _ in range(16):
        decl.u32()  # required stream bytes
    elements: list[VertexElement] = []
    while decl.offset < len(decl_payload):
        if decl.offset + 8 > len(decl_payload):
            raise CaptureError(f"{path}: DECLARATION ends with a partial element")
        elements.append(VertexElement(decl.u16(), decl.u16(), decl.u8(), decl.u8(),
                                      decl.u8(), decl.u8()))
    if len(elements) > declaration_count:
        raise CaptureError(f"{path}: DECLARATION has more captured elements than its count")
    if declaration_active and declaration_count and not elements:
        raise CaptureError(f"{path}: active DECLARATION has no captured elements")

    vertex_stride, vertices = _parse_vertices(one(SECTION_VERTICES, "VERTICES"))
    if vertex_stride not in (D9_VERTEX_STRIDE, D9_COLOR_VERTEX_STRIDE):
        raise CaptureError(
            f"{path}: unsupported decoded vertex stride {vertex_stride}; "
            f"expected D9Vertex={D9_VERTEX_STRIDE} or D9ColorVertex={D9_COLOR_VERTEX_STRIDE}"
        )

    index_size = 0
    indices = b""
    if SECTION_INDICES in by_id:
        index_size, indices = _parse_indices(one(SECTION_INDICES, "INDICES"))
    if draw.indexed and not indices:
        raise CaptureError(f"{path}: indexed draw is missing its INDICES payload")

    textures = tuple(
        _parse_texture(payload, index)
        for index, payload in enumerate(by_id.get(SECTION_TEXTURE, []))
    )

    frame_payload = one(SECTION_FRAME, "FRAME", 36)
    frame_values = struct.unpack("<9I", frame_payload)
    frame_index, draw_index = frame_values[:2]
    clear_valid, clear_color, clear_flags = frame_values[2:5]

    target_width = target_height = target_format = 0
    if SECTION_TARGET in by_id:
        target = one(SECTION_TARGET, "TARGET", 24)
        _handle, _implicit, target_width, target_height, target_format, _usage = \
            struct.unpack("<6I", target)
    elif version >= 2:
        raise CaptureError(f"{path}: version {version} draw is missing TARGET section")
    width = target_width or viewport.width
    height = target_height or viewport.height
    if not width or not height:
        raise CaptureError(f"{path}: target and viewport both have a zero dimension")
    if width > 16384 or height > 16384:
        raise CaptureError(f"{path}: refusing unreasonable target size {width}x{height}")

    warnings: list[str] = []
    if truncation:
        names = [name for bit, name in TRUNCATION_NAMES if truncation & bit]
        unknown = truncation & ~sum(bit for bit, _ in TRUNCATION_NAMES)
        if unknown:
            names.append(f"unknown-0x{unknown:x}")
        warnings.append("capture is truncated: " + ", ".join(names))
    if vertex_stride == D9_VERTEX_STRIDE:
        warnings.append(
            "D9Vertex payload has XYZ+UV only; vertex COLOR0 is absent and defaults to white"
        )
    if vs_code or ps_code or state.pixel_shader:
        warnings.append(
            "shader bytecode/presence is recorded but not executed; using fixed colour/UV approximation"
        )
    if state.srgb:
        warnings.append("state.srgb=1 is not applied by this diagnostic rasterizer")
    if state.mip_filter != 0 or state.resident_mips > 1:
        warnings.append("mip selection/filtering is approximated with the captured level only")

    return Capture(
        path=path, version=version, signature=signature, truncation=truncation,
        frame_index=frame_index, draw_index=draw_index, draw=draw, viewport=viewport,
        state=state, world=world, view=view, projection=projection,
        vs_code=vs_code, ps_code=ps_code,
        declaration_active=declaration_active,
        declaration_position=declaration_position, declaration_uv=declaration_uv,
        elements=tuple(elements), vertex_stride=vertex_stride, vertices=vertices,
        index_size=index_size, indices=indices, textures=textures,
        clear_valid=clear_valid, clear_color=clear_color, clear_flags=clear_flags,
        target_width=target_width, target_height=target_height,
        target_format=target_format, warnings=warnings,
    )


def load_capture(path: Path | str) -> Capture:
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise CaptureError(f"{path}: cannot read capture: {exc}") from exc
    return parse_capture(data, path)


def load_captures(paths: Sequence[Path], jobs: int) -> list[Capture]:
    if jobs <= 1 or len(paths) <= 1:
        return [load_capture(path) for path in paths]
    # Captures are independent for parsing/hash validation.  Rasterization stays
    # ordered because later draws may blend into earlier draws.
    with ProcessPoolExecutor(max_workers=min(jobs, len(paths))) as pool:
        return list(pool.map(load_capture, paths))


def argb_bytes(value: int) -> tuple[int, int, int, int]:
    return ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF, (value >> 24) & 0xFF)


def clamp01(value: float) -> float:
    if value <= 0.0:
        return 0.0
    if value >= 1.0:
        return 1.0
    return value


def _decode_vertices(capture: Capture) -> list[tuple[float, ...]]:
    count = len(capture.vertices) // capture.vertex_stride
    if capture.vertex_stride == D9_VERTEX_STRIDE:
        values = [struct.unpack_from("<5f", capture.vertices, i * capture.vertex_stride)
                  for i in range(count)]
        return [(x, y, z, u, v, 1.0, 1.0, 1.0, 1.0) for x, y, z, u, v in values]
    values = [struct.unpack_from("<10f", capture.vertices, i * capture.vertex_stride)
              for i in range(count)]
    return [row[:9] for row in values]  # trailing float is TEXCOORD0.w, unused for 2D blit.


def _resolve_position_space(capture: Capture, requested: str) -> str:
    if requested != "auto":
        return requested
    if capture.declaration_active and capture.declaration_position < len(capture.elements):
        usage = capture.elements[capture.declaration_position].usage
        if usage == USAGE_POSITIONT:
            return "screen"
        if usage == USAGE_POSITION:
            if capture.vs_code:
                raise CaptureError(
                    f"{capture.path}: POSITION with VS bytecode needs an explicit "
                    "--position-space; VS constants are not captured"
                )
            return "object"
        raise CaptureError(
            f"{capture.path}: declaration position element has unsupported usage {usage}"
        )
    if capture.declaration_active:
        raise CaptureError(f"{capture.path}: active declaration has no captured position element")
    if capture.vs_code:
        raise CaptureError(
            f"{capture.path}: inactive declaration plus VS bytecode is ambiguous; "
            "choose --position-space screen|clip|object explicitly"
        )
    capture.warnings.append(
        "declaration is inactive and raw FVF is not captured; assuming pretransformed "
        "screen-space positions (override with --position-space)"
    )
    return "screen"


def _row_vector(matrix: tuple[tuple[float, ...], ...], vector: Sequence[float]) -> list[float]:
    return [sum(vector[i] * matrix[i][j] for i in range(4)) for j in range(4)]


def _to_shader_vertex(
    raw: Sequence[float], capture: Capture, space: str,
    raw_bounds: tuple[float, float, float, float, float, float] | None = None,
) -> ShaderVertex | None:
    x, y, z, u, v, r, g, b, a = raw
    vp = capture.viewport
    if space == "raw":
        if raw_bounds is None:
            raise CaptureError("raw position space requires captured XYZ bounds")
        min_x, max_x, min_y, max_y, min_z, max_z = raw_bounds
        span_x, span_y, span_z = max_x - min_x, max_y - min_y, max_z - min_z
        sx = vp.x + ((x - min_x) / span_x if span_x > 0.0 else 0.5) * vp.width
        sy = vp.y + ((max_y - y) / span_y if span_y > 0.0 else 0.5) * vp.height
        sz = vp.minz + (clamp01((z - min_z) / span_z) if span_z > 0.0 else 0.0) * (vp.maxz - vp.minz)
        inv_w = 1.0
    elif space == "screen":
        sx = vp.x + x
        sy = vp.y + y
        sz = vp.minz + clamp01(z) * (vp.maxz - vp.minz)
        inv_w = 1.0
    else:
        if space == "object":
            vector = _row_vector(capture.world, (x, y, z, 1.0))
            vector = _row_vector(capture.view, vector)
            vector = _row_vector(capture.projection, vector)
        elif space == "clip":
            vector = (x, y, z, 1.0)
        else:
            raise CaptureError(f"unknown position space {space}")
        cx, cy, cz, cw = vector
        if not all(math.isfinite(value) for value in vector) or cw <= 1.0e-12:
            return None
        nx, ny, nz = cx / cw, cy / cw, cz / cw
        if nz < 0.0 or nz > 1.0:
            return None
        sx = vp.x + (nx + 1.0) * 0.5 * vp.width
        sy = vp.y + (1.0 - ny) * 0.5 * vp.height
        sz = vp.minz + nz * (vp.maxz - vp.minz)
        inv_w = 1.0 / cw
    if not all(math.isfinite(value) for value in (sx, sy, sz, u, v, r, g, b, a)):
        return None
    return ShaderVertex(sx, sy, sz, u, v, r, g, b, a, inv_w)


def _primitive_indices(
    draw: DrawInfo, source_indices: Sequence[int], expanded_scratch: bool = False
) -> list[tuple[int, int, int]]:
    primitives = draw.primitives
    if primitives == 0:
        return []
    primitive = TRIANGLE_LIST if expanded_scratch else draw.type
    if primitive == TRIANGLE_LIST:
        needed = primitives * 3
        if len(source_indices) < needed:
            raise CaptureError(f"triangle list needs {needed} vertices, have {len(source_indices)}")
        return [(source_indices[i], source_indices[i + 1], source_indices[i + 2])
                for i in range(0, needed, 3)]
    if primitive == TRIANGLE_STRIP:
        needed = primitives + 2
        if len(source_indices) < needed:
            raise CaptureError(f"triangle strip needs {needed} vertices, have {len(source_indices)}")
        triangles = []
        for triangle in range(primitives):
            i = triangle
            if triangle & 1:
                triangles.append((source_indices[i + 1], source_indices[i], source_indices[i + 2]))
            else:
                triangles.append((source_indices[i], source_indices[i + 1], source_indices[i + 2]))
        return triangles
    if primitive == TRIANGLE_FAN:
        needed = primitives + 2
        if len(source_indices) < needed:
            raise CaptureError(f"triangle fan needs {needed} vertices, have {len(source_indices)}")
        return [(source_indices[0], source_indices[i + 1], source_indices[i + 2])
                for i in range(primitives)]
    raise CaptureError(
        f"unsupported primitive type {draw.type}; supported triangle types are "
        f"list={TRIANGLE_LIST}, strip={TRIANGLE_STRIP}, fan={TRIANGLE_FAN}"
    )


def build_triangles(
    capture: Capture, position_space: str = "auto"
) -> tuple[list[tuple[ShaderVertex, ShaderVertex, ShaderVertex]], list[str]]:
    warnings: list[str] = []
    space = _resolve_position_space(capture, position_space)
    raw_vertices = _decode_vertices(capture)
    shader_vertices = [_to_shader_vertex(raw, capture, space) for raw in raw_vertices]
    draw = capture.draw
    expanded_count = draw.primitives * 3
    source_indices: list[int]
    expanded_scratch = False

    if not draw.indexed:
        compact_count = expanded_count if draw.type == TRIANGLE_LIST else draw.primitives + 2
        if len(raw_vertices) == expanded_count:
            # Current COM hook stores d->scratch, already expanded and already
            # offset by draw.start.  This also handles strip/fan snapshots.
            source_indices = list(range(expanded_count))
            expanded_scratch = True
        elif len(raw_vertices) >= draw.start + compact_count:
            # Also accept a conventional compact non-indexed vertex range.
            source_indices = list(range(draw.start, draw.start + compact_count))
        else:
            raise CaptureError(
                f"{capture.path}: non-indexed {draw.type} draw has {len(raw_vertices)} "
                f"vertices; need expanded={expanded_count} or compact={compact_count}"
            )
    else:
        needed = expanded_count if draw.type == TRIANGLE_LIST else draw.primitives + 2
        if len(capture.indices) < needed * capture.index_size:
            raise CaptureError(
                f"{capture.path}: indexed draw needs {needed} indices, "
                f"capture stores {len(capture.indices) // capture.index_size}"
            )
        if capture.index_size == 2:
            raw_indices = list(struct.unpack_from(f"<{needed}H", capture.indices, 0))
        else:
            raw_indices = list(struct.unpack_from(f"<{needed}I", capture.indices, 0))
        if len(raw_vertices) == expanded_count:
            # d->scratch is already in primitive order.  Re-indexing it with the
            # original index buffer would apply the indices twice.
            source_indices = list(range(expanded_count))
            expanded_scratch = True
            warnings.append(
                "indexed vertices are the expanded D9Vertex scratch stream; raw indices "
                "were parsed/hash-checked but are not applied a second time"
            )
        else:
            full_indices = [draw.base_vertex + index for index in raw_indices]
            if full_indices and min(full_indices) >= 0 and max(full_indices) < len(raw_vertices):
                source_indices = full_indices
            else:
                relative = [index - draw.min_vertex for index in raw_indices]
                if (draw.min_vertex and min(relative) >= 0 and
                        max(relative) < len(raw_vertices)):
                    source_indices = relative
                    warnings.append("indexed vertex payload is a MinIndex-relative compact range")
                else:
                    raise CaptureError(
                        f"{capture.path}: index range {min(raw_indices)}..{max(raw_indices)} "
                        f"does not address the {len(raw_vertices)} captured vertices"
                    )

    triangles: list[tuple[ShaderVertex, ShaderVertex, ShaderVertex]] = []
    for indices in _primitive_indices(draw, source_indices, expanded_scratch):
        triangle = tuple(shader_vertices[index] for index in indices)
        if any(vertex is None for vertex in triangle):
            warnings.append("a triangle was skipped because a vertex failed transform/depth validation")
            continue
        triangles.append(triangle)  # type: ignore[arg-type]
    return triangles, warnings


def _rgb565(value: int) -> tuple[int, int, int]:
    r, g, b = value >> 11, (value >> 5) & 0x3F, value & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _bc_color_palette(block: bytes, offset: int, force_four_color: bool) -> list[tuple[int, int, int, int]]:
    c0 = struct.unpack_from("<H", block, offset)[0]
    c1 = struct.unpack_from("<H", block, offset + 2)[0]
    p0 = (*_rgb565(c0), 255)
    p1 = (*_rgb565(c1), 255)
    if force_four_color or c0 > c1:
        p2 = tuple((2 * p0[i] + p1[i]) // 3 for i in range(3)) + (255,)
        p3 = tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3)) + (255,)
    else:
        p2 = tuple((p0[i] + p1[i]) // 2 for i in range(3)) + (255,)
        p3 = (0, 0, 0, 0)
    return [p0, p1, p2, p3]


def _bc_color_block(block: bytes, offset: int = 0, force_four_color: bool = False) -> bytes:
    if len(block) < offset + 8:
        raise CaptureError("BC color block is shorter than 8 bytes")
    palette = _bc_color_palette(block, offset, force_four_color)
    bits = struct.unpack_from("<I", block, offset + 4)[0]
    rgba = bytearray(64)
    for i in range(16):
        rgba[i * 4:i * 4 + 4] = bytes(palette[(bits >> (2 * i)) & 3])
    return bytes(rgba)


def _bc_alpha_palette(a0: int, a1: int) -> list[int]:
    palette = [a0, a1] + [0] * 6
    if a0 > a1:
        for i in range(1, 7):
            palette[i + 1] = ((7 - i) * a0 + i * a1) // 7
    else:
        for i in range(1, 5):
            palette[i + 1] = ((5 - i) * a0 + i * a1) // 5
        palette[6], palette[7] = 0, 255
    return palette


def decode_bc1_block(block: bytes) -> bytes:
    """Decode one little-endian DXT1/BC1 4x4 block to row-major RGBA8."""
    return _bc_color_block(block)


def decode_bc3_block(block: bytes) -> bytes:
    """Decode one little-endian DXT5/BC3 4x4 block to row-major RGBA8."""
    if len(block) < 16:
        raise CaptureError("BC3 block is shorter than 16 bytes")
    palette = _bc_alpha_palette(block[0], block[1])
    alpha_bits = int.from_bytes(block[2:8], "little")
    color = _bc_color_block(block, 8, True)
    rgba = bytearray(color)
    for i in range(16):
        rgba[i * 4 + 3] = palette[(alpha_bits >> (3 * i)) & 7]
    return bytes(rgba)


def decode_bc_block(format_id: int, block: bytes) -> bytes:
    if format_id == DXT1:
        return decode_bc1_block(block)
    if format_id == DXT5:
        return decode_bc3_block(block)
    raise CaptureError(f"unsupported BC texture format 0x{format_id:08x}")


class DecodedTexture:
    def __init__(self, texture: CapturedTexture, state: DrawState):
        self.width = texture.width
        self.height = texture.height
        self.state = state
        self.border = tuple(value / 255.0 for value in argb_bytes(state.border_color))
        compressed = texture.format in (DXT1, DXT5)
        if texture.format in (A8R8G8B8, X8R8G8B8):
            bytes_per_pixel = 4
        elif texture.format == A8:
            bytes_per_pixel = 1
        elif compressed:
            bytes_per_pixel = 0
        else:
            raise CaptureError(
                f"texture stage {texture.stage}: unsupported format 0x{texture.format:08x}; "
                f"supported A8R8G8B8={A8R8G8B8}, X8R8G8B8={X8R8G8B8}, "
                f"A8={A8}, DXT1={DXT1}, DXT5={DXT5}"
            )
        if compressed:
            block_bytes = 8 if texture.format == DXT1 else 16
            blocks_wide = (self.width + 3) // 4
            blocks_high = (self.height + 3) // 4
            row_bytes = blocks_wide * block_bytes  # DXT pitch counts block bytes.
        else:
            row_bytes = self.width * bytes_per_pixel
        pitch = texture.row_pitch or row_bytes
        if pitch < row_bytes:
            raise CaptureError(
                f"texture stage {texture.stage}: row pitch {pitch} is smaller than {row_bytes}"
            )
        required = ((blocks_high - 1) * pitch + row_bytes
                    if compressed else (self.height - 1) * pitch + row_bytes)
        if len(texture.pixels) < required:
            raise CaptureError(
                f"texture stage {texture.stage}: stored pixels are truncated "
                f"({len(texture.pixels)} of {required} required bytes)"
            )
        rgba = bytearray(self.width * self.height * 4)
        if compressed:
            for by in range(blocks_high):
                for bx in range(blocks_wide):
                    offset = by * pitch + bx * block_bytes
                    decoded = decode_bc_block(texture.format, texture.pixels[offset:offset + block_bytes])
                    for y in range(4):
                        py = by * 4 + y
                        if py >= self.height:
                            break
                        copied = min(4, self.width - bx * 4)
                        source = (y * 4) * 4
                        dest = (py * self.width + bx * 4) * 4
                        rgba[dest:dest + copied * 4] = decoded[source:source + copied * 4]
        else:
            row_rgba_bytes = self.width * 4
            for y in range(self.height):
                source = texture.pixels[y * pitch:y * pitch + row_bytes]
                dest = y * row_rgba_bytes
                if texture.format == A8:
                    rgba[dest:dest + row_rgba_bytes:4] = b"\xff" * self.width
                    rgba[dest + 1:dest + row_rgba_bytes:4] = b"\xff" * self.width
                    rgba[dest + 2:dest + row_rgba_bytes:4] = b"\xff" * self.width
                    rgba[dest + 3:dest + row_rgba_bytes:4] = source
                else:
                    # D3D9 A8R8G8B8/X8R8G8B8 is B,G,R,A in little-endian memory.
                    rgba[dest:dest + row_rgba_bytes:4] = source[2::4]
                    rgba[dest + 1:dest + row_rgba_bytes:4] = source[1::4]
                    rgba[dest + 2:dest + row_rgba_bytes:4] = source[0::4]
                    if texture.format == X8R8G8B8:
                        rgba[dest + 3:dest + row_rgba_bytes:4] = b"\xff" * self.width
                    else:
                        rgba[dest + 3:dest + row_rgba_bytes:4] = source[3::4]
        self.rgba = bytes(rgba)

    @staticmethod
    def _wrap(index: int, size: int, mode: int) -> int | None:
        if mode in (1,):  # WRAP
            return index % size
        if mode == 2:    # MIRROR
            period = size * 2
            folded = index % period
            return folded if folded < size else period - 1 - folded
        if mode == 3:    # CLAMP
            return min(size - 1, max(0, index))
        if mode == 5:    # MIRRORONCE: mirror once across [-1,1], then clamp.
            if index < 0:
                index = -index
            if index >= size * 2:
                return size - 1
            return index if index < size else size * 2 - 1 - index
        if mode == 4:    # BORDER
            return index if 0 <= index < size else None
        raise CaptureError(f"unsupported texture address mode {mode}")

    def _tap(self, x: int, y: int) -> tuple[float, float, float, float]:
        tx = self._wrap(x, self.width, self.state.address_u)
        ty = self._wrap(y, self.height, self.state.address_v)
        if tx is None or ty is None:
            return self.border
        offset = (ty * self.width + tx) * 4
        return tuple(value / 255.0 for value in self.rgba[offset:offset + 4])  # type: ignore[return-value]

    def sample(self, u: float, v: float, linear: bool) -> tuple[float, float, float, float]:
        if not linear:
            # For point sampling, normalized 1.0 is the final texel for clamp,
            # and wraps to zero for wrap/mirror.  Other modes receive the raw
            # integer tap so BORDER and MIRRORONCE can reject/fold negatives.
            fx = (min(self.width, max(0.0, u * self.width))
                  if self.state.address_u == 3 else u * self.width)
            fy = (min(self.height, max(0.0, v * self.height))
                  if self.state.address_v == 3 else v * self.height)
            return self._tap(int(math.floor(fx)), int(math.floor(fy)))
        if self.state.address_u == 4 and not 0.0 <= u <= 1.0:
            return self.border
        if self.state.address_v == 4 and not 0.0 <= v <= 1.0:
            return self.border
        fx = u * self.width - 0.5
        fy = v * self.height - 0.5
        x0, y0 = math.floor(fx), math.floor(fy)
        tx, ty = fx - x0, fy - y0
        c00 = self._tap(x0, y0)
        c10 = self._tap(x0 + 1, y0)
        c01 = self._tap(x0, y0 + 1)
        c11 = self._tap(x0 + 1, y0 + 1)
        return tuple(
            (c00[c] * (1.0 - tx) + c10[c] * tx) * (1.0 - ty) +
            (c01[c] * (1.0 - tx) + c11[c] * tx) * ty
            for c in range(4)
        )  # type: ignore[return-value]


def _compare(func: int, value: float, reference: float) -> bool:
    if func == 1:
        return False
    if func == 2:
        return value < reference
    if func == 3:
        return value == reference
    if func == 4:
        return value <= reference
    if func == 5:
        return value > reference
    if func == 6:
        return value != reference
    if func == 7:
        return value >= reference
    if func == 8:
        return True
    raise CaptureError(f"unsupported D3DCMP function {func}")


def _blend_factor(factor: int, src: Sequence[float], dst: Sequence[float]) -> tuple[float, ...]:
    if factor == 1:
        return (0.0, 0.0, 0.0, 0.0)
    if factor == 2:
        return (1.0, 1.0, 1.0, 1.0)
    if factor == 3:
        return tuple(src)
    if factor == 4:
        return tuple(1.0 - value for value in src)
    if factor == 5:
        return (src[3],) * 4
    if factor == 6:
        return (1.0 - src[3],) * 4
    if factor == 7:
        return (dst[3],) * 4
    if factor == 8:
        return (1.0 - dst[3],) * 4
    if factor == 9:
        return tuple(dst)
    if factor == 10:
        return tuple(1.0 - value for value in dst)
    if factor == 11:
        saturated = min(src[3], 1.0 - dst[3])
        return (saturated, saturated, saturated, 1.0)
    raise CaptureError(
        f"unsupported blend factor {factor}; blend-factor/separate-alpha state is not captured"
    )


def _blend_op(op: int, src: Sequence[float], dst: Sequence[float]) -> tuple[float, float, float, float]:
    if op == 1:
        return tuple(src[i] + dst[i] for i in range(4))  # type: ignore[return-value]
    if op == 2:
        return tuple(src[i] - dst[i] for i in range(4))  # type: ignore[return-value]
    if op == 3:
        return tuple(dst[i] - src[i] for i in range(4))  # type: ignore[return-value]
    if op == 4:
        return tuple(min(src[i], dst[i]) for i in range(4))  # type: ignore[return-value]
    if op == 5:
        return tuple(max(src[i], dst[i]) for i in range(4))  # type: ignore[return-value]
    raise CaptureError(f"unsupported blend operation {op}")


def _validate_state(state: DrawState) -> None:
    if state.cull not in (1, 2, 3):
        raise CaptureError(f"unsupported cull mode {state.cull}")
    if state.color_write & ~0xF:
        raise CaptureError(f"invalid color write mask {state.color_write}")
    if state.alpha_test and not 1 <= state.alpha_func <= 8:
        raise CaptureError(f"unsupported alpha-test function {state.alpha_func}")
    if state.z_enable and not 1 <= state.z_func <= 8:
        raise CaptureError(f"unsupported depth-test function {state.z_func}")
    for mode in (state.address_u, state.address_v):
        if mode not in (1, 2, 3, 4, 5):
            raise CaptureError(f"unsupported texture address mode {mode}")
    for name, filter_value in (("mag", state.mag_filter), ("min", state.min_filter)):
        if filter_value not in (0, 1, 2, 3):
            raise CaptureError(f"unsupported {name} texture filter {filter_value}")
    if state.mip_filter not in (0, 1, 2):
        raise CaptureError(f"unsupported mip texture filter {state.mip_filter}")
    if state.blend:
        _blend_factor(state.src_blend, (1.0, 1.0, 1.0, 1.0), (0.0, 0.0, 0.0, 0.0))
        _blend_factor(state.dst_blend, (1.0, 1.0, 1.0, 1.0), (0.0, 0.0, 0.0, 0.0))
        _blend_op(state.blend_op, (0.0,) * 4, (0.0,) * 4)


def _edge(a: ShaderVertex, b: ShaderVertex, px: float, py: float) -> float:
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)


def _top_left(a: ShaderVertex, b: ShaderVertex) -> bool:
    # Window coordinates have +Y down.  Include the top/left edge only.
    return a.y > b.y or (a.y == b.y and a.x < b.x)


class Framebuffer:
    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self.pixels = bytearray(width * height * 4)
        self.depth = array("f", [1.0]) * (width * height)
        self.touched = bytearray(width * height)
        self.stats = RenderStats()

    def clear(self, color: int = 0, clear_depth: bool = True) -> None:
        rgba = argb_bytes(color)
        row = bytes(rgba) * self.width
        for y in range(self.height):
            start = y * self.width * 4
            self.pixels[start:start + len(row)] = row
        self.touched = bytearray(self.width * self.height)
        if clear_depth:
            self.depth = array("f", [1.0]) * (self.width * self.height)

    def draw_triangle(
        self,
        triangle: tuple[ShaderVertex, ShaderVertex, ShaderVertex],
        capture: Capture,
        texture: DecodedTexture | None,
    ) -> None:
        self.stats.triangles += 1
        original = triangle
        area = _edge(triangle[0], triangle[1], triangle[2].x, triangle[2].y)
        if abs(area) < 1.0e-9:
            return
        if (capture.state.cull == 2 and area > 0.0) or (
            capture.state.cull == 3 and area < 0.0
        ):
            self.stats.culled += 1
            return
        if area < 0.0:
            triangle = (triangle[0], triangle[2], triangle[1])
            area = -area
        v0, v1, v2 = triangle
        vp = capture.viewport
        x0 = max(vp.x, int(math.floor(min(v0.x, v1.x, v2.x))))
        y0 = max(vp.y, int(math.floor(min(v0.y, v1.y, v2.y))))
        x1 = min(vp.x + vp.width, int(math.ceil(max(v0.x, v1.x, v2.x))))
        y1 = min(vp.y + vp.height, int(math.ceil(max(v0.y, v1.y, v2.y))))
        x0 = min(self.width, max(0, x0))
        y0 = min(self.height, max(0, y0))
        x1 = min(self.width, max(0, x1))
        y1 = min(self.height, max(0, y1))
        if x0 >= x1 or y0 >= y1:
            self.stats.clipped += 1
            return

        linear = False
        if texture is not None:
            uv_span = max(max(v.u for v in original) - min(v.u for v in original),
                          max(v.v for v in original) - min(v.v for v in original))
            footprint = max(uv_span * texture.width / max(1, x1 - x0),
                            uv_span * texture.height / max(1, y1 - y0))
            selected = capture.state.min_filter if footprint > 1.0 else capture.state.mag_filter
            linear = selected in (2, 3)

        state = capture.state
        inv_area = 1.0 / area
        for y in range(y0, y1):
            py = y + 0.5
            for x in range(x0, x1):
                px = x + 0.5
                w0 = _edge(v1, v2, px, py) * inv_area
                w1 = _edge(v2, v0, px, py) * inv_area
                w2 = _edge(v0, v1, px, py) * inv_area
                covered = (
                    (w0 > 0.0 or (w0 >= -1.0e-9 and _top_left(v1, v2))) and
                    (w1 > 0.0 or (w1 >= -1.0e-9 and _top_left(v2, v0))) and
                    (w2 > 0.0 or (w2 >= -1.0e-9 and _top_left(v0, v1)))
                )
                if not covered:
                    continue
                inv_w = w0 * v0.inv_w + w1 * v1.inv_w + w2 * v2.inv_w
                if inv_w <= 0.0:
                    continue
                p0, p1, p2 = w0 * v0.inv_w, w1 * v1.inv_w, w2 * v2.inv_w
                u = (p0 * v0.u + p1 * v1.u + p2 * v2.u) / inv_w
                v = (p0 * v0.v + p1 * v1.v + p2 * v2.v) / inv_w
                r = clamp01((p0 * v0.r + p1 * v1.r + p2 * v2.r) / inv_w)
                g = clamp01((p0 * v0.g + p1 * v1.g + p2 * v2.g) / inv_w)
                b = clamp01((p0 * v0.b + p1 * v1.b + p2 * v2.b) / inv_w)
                a = clamp01((p0 * v0.a + p1 * v1.a + p2 * v2.a) / inv_w)
                if texture is not None:
                    tr, tg, tb, ta = texture.sample(u, v, linear)
                    r, g, b, a = r * tr, g * tg, b * tb, a * ta
                if state.alpha_test and not _compare(state.alpha_func, a, state.alpha_ref / 255.0):
                    continue
                depth = w0 * v0.z + w1 * v1.z + w2 * v2.z
                pixel = y * self.width + x
                if state.z_enable and not _compare(state.z_func, depth, self.depth[pixel]):
                    continue
                offset = pixel * 4
                dst = tuple(value / 255.0 for value in self.pixels[offset:offset + 4])
                src = (r, g, b, a)
                if state.blend:
                    src_factor = _blend_factor(state.src_blend, src, dst)
                    dst_factor = _blend_factor(state.dst_blend, src, dst)
                    blended_src = tuple(src[i] * src_factor[i] for i in range(4))
                    blended_dst = tuple(dst[i] * dst_factor[i] for i in range(4))
                    shaded = _blend_op(state.blend_op, blended_src, blended_dst)
                else:
                    shaded = src
                out = tuple(int(clamp01(shaded[i]) * 255.0 + 0.5) for i in range(4))
                mask = state.color_write
                self.pixels[offset] = out[0] if mask & 1 else self.pixels[offset]
                self.pixels[offset + 1] = out[1] if mask & 2 else self.pixels[offset + 1]
                self.pixels[offset + 2] = out[2] if mask & 4 else self.pixels[offset + 2]
                self.pixels[offset + 3] = out[3] if mask & 8 else self.pixels[offset + 3]
                if state.z_enable and state.z_write:
                    self.depth[pixel] = depth
                self.touched[pixel] = 1
                self.stats.fragments += 1

    def nonzero_pixels(self) -> int:
        return sum(1 for offset in range(0, len(self.pixels), 4)
                   if self.pixels[offset:offset + 4] != b"\x00\x00\x00\x00")

    def nonzero_bbox(self) -> list[int] | None:
        min_x, min_y, max_x, max_y = self.width, self.height, -1, -1
        for y in range(self.height):
            row = y * self.width * 4
            for x in range(self.width):
                offset = row + x * 4
                if self.pixels[offset:offset + 4] != b"\x00\x00\x00\x00":
                    min_x, min_y = min(min_x, x), min(min_y, y)
                    max_x, max_y = max(max_x, x), max(max_y, y)
        return None if max_x < 0 else [min_x, min_y, max_x, max_y]


def _png_chunk(kind: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def write_png(path: Path | str, width: int, height: int, rgba: bytes) -> None:
    if len(rgba) != width * height * 4:
        raise CaptureError("PNG framebuffer byte count does not match its dimensions")
    stride = width * 4
    scanlines = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride]
                         for y in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr) +
           _png_chunk(b"IDAT", zlib.compress(scanlines, 9)) +
           _png_chunk(b"IEND", b""))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def render_captures(
    captures: Sequence[Capture], output: Path | str, position_space: str = "auto"
) -> dict:
    if not captures:
        raise CaptureError("no draw captures were supplied")
    first = captures[0]
    output_path = Path(output).resolve()
    for capture in captures:
        protected = (capture.path.resolve(), capture.path.with_suffix(".txt").resolve())
        if output_path in protected:
            raise CaptureError(f"refusing to overwrite input capture {output_path}")
    width, height = first.width, first.height
    for capture in captures[1:]:
        if (capture.width, capture.height) != (width, height):
            raise CaptureError(
                f"{capture.path}: target size {capture.width}x{capture.height} does not "
                f"match first target {width}x{height}"
            )
    framebuffer = Framebuffer(width, height)
    warnings: list[str] = []
    previous_frame: int | None = None
    rendered: list[dict] = []
    for capture in captures:
        warnings.extend(f"{capture.path}: {warning}" for warning in capture.warnings)
        _validate_state(capture.state)
        if previous_frame is None:
            if capture.clear_valid and capture.clear_flags & 1:
                framebuffer.clear(capture.clear_color, bool(capture.clear_flags & 2))
        elif capture.frame_index != previous_frame and capture.clear_valid and capture.clear_flags & 1:
            framebuffer.clear(capture.clear_color, bool(capture.clear_flags & 2))
        previous_frame = capture.frame_index
        triangles, geometry_warnings = build_triangles(capture, position_space)
        warnings.extend(f"{capture.path}: {warning}" for warning in geometry_warnings)
        texture: DecodedTexture | None = None
        stage0 = [item for item in capture.textures if item.stage == 0]
        if stage0:
            texture = DecodedTexture(stage0[0], capture.state)
            if len(stage0) > 1:
                warnings.append(f"{capture.path}: multiple stage-0 textures; using the first")
            if any(item.stage != 0 for item in capture.textures):
                warnings.append(f"{capture.path}: non-stage-0 textures are not combined")
        for triangle in triangles:
            framebuffer.draw_triangle(triangle, capture, texture)
        rendered.append({
            "path": str(capture.path),
            "signature": f"0x{capture.signature:016x}",
            "frame": capture.frame_index,
            "draw": capture.draw_index,
            "primitive": capture.draw.type,
            "indexed": bool(capture.draw.indexed),
            "vertices": len(capture.vertices) // capture.vertex_stride,
            "indices": len(capture.indices) // capture.index_size if capture.index_size else 0,
            "triangles": len(triangles),
            "texture_stages": [item.stage for item in capture.textures],
        })
    write_png(output, width, height, bytes(framebuffer.pixels))
    return {
        "png": str(output),
        "width": width,
        "height": height,
        "nonzero_pixels": framebuffer.nonzero_pixels(),
        "nonzero_bbox": framebuffer.nonzero_bbox(),
        "covered_pixels": sum(framebuffer.touched),
        "fragments": framebuffer.stats.fragments,
        "culled_triangles": framebuffer.stats.culled,
        "captures": rendered,
        "warnings": warnings,
    }


def _input_path(value: str) -> Path:
    path = Path(value)
    if path.suffix.lower() == ".txt":
        path = path.with_suffix(".bin")
    if path.suffix.lower() != ".bin":
        raise CaptureError(f"{path}: input must be a .bin capture or its .txt companion")
    return path


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="d9_capture .bin files (or .txt companions), in draw order")
    parser.add_argument("-o", "--output", required=True, help="destination PNG (never written beside inputs implicitly)")
    parser.add_argument(
        "--position-space", choices=("auto", "screen", "clip", "object"), default="auto",
        help="vertex position semantics; auto uses POSITIONT/POSITION, otherwise assumes screen for FVF",
    )
    parser.add_argument(
        "--jobs", type=int, default=min(16, os.cpu_count() or 1),
        help="parallel capture parsers, 1..16 (default: up to 16; rasterization stays ordered)",
    )
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 16:
        parser.error("--jobs must be between 1 and 16")
    started = time.perf_counter()
    try:
        paths = [_input_path(value) for value in args.inputs]
        captures = load_captures(paths, args.jobs)
        result = render_captures(captures, Path(args.output), args.position_space)
    except (CaptureError, OSError) as exc:
        print(f"capture-raster: error: {exc}", file=sys.stderr)
        return 2
    elapsed = time.perf_counter() - started
    for capture in result["captures"]:
        print(
            f"{capture['path']}: frame={capture['frame']} draw={capture['draw']} "
            f"type={capture['primitive']} indexed={int(capture['indexed'])} "
            f"vertices={capture['vertices']} indices={capture['indices']} "
            f"triangles={capture['triangles']} textures={capture['texture_stages']} "
            f"signature={capture['signature']}"
        )
    print(
        f"PNG {result['png']} {result['width']}x{result['height']} "
        f"covered_pixels={result['covered_pixels']} nonzero_pixels={result['nonzero_pixels']} "
        f"nonzero_bbox={result['nonzero_bbox']} "
        f"fragments={result['fragments']} "
        f"culled_triangles={result['culled_triangles']} wall={elapsed:.3f}s"
    )
    for warning in result["warnings"]:
        print(f"warning: {warning}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
