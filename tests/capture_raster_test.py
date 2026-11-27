#!/usr/bin/env python3
"""Standard-library tests for tools/capture_raster.py (no GPU/Pillow)."""

from __future__ import annotations

from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import capture_raster as cr  # noqa: E402


def section(section_id: int, payload: bytes) -> bytes:
    return struct.pack("<II", section_id, len(payload)) + payload


def matrix_identity() -> bytes:
    values = (1.0, 0.0, 0.0, 0.0,
              0.0, 1.0, 0.0, 0.0,
              0.0, 0.0, 1.0, 0.0,
              0.0, 0.0, 0.0, 1.0)
    return struct.pack("<16f", *values)


def shader(code: bytes = b"") -> bytes:
    return struct.pack("<IIQQII", 0, 0, cr.fnv1a(code), len(code), len(code), 0) + code


def vertices_payload(vertices: bytes, stride: int, count: int) -> bytes:
    return struct.pack("<4I2Q2I", stride, count, count, len(vertices),
                       len(vertices), cr.fnv1a(vertices), 0, 0) + vertices


def indices_payload(indices: bytes, size: int, count: int) -> bytes:
    return struct.pack("<4I2Q2I", size, count, count, len(indices),
                       len(indices), cr.fnv1a(indices), 0, 0) + indices


def texture_payload(pixels: bytes, width: int, height: int, format_id: int,
                    pitch: int | None = None) -> bytes:
    pitch = pitch if pitch is not None else width * (1 if format_id == cr.A8 else 4)
    return struct.pack("<11I4Q", 0, 0x50000000, width, height, format_id,
                       1, 0, 0, 0, 0, 0, pitch, len(pixels), len(pixels),
                       cr.fnv1a(pixels)) + pixels


def make_capture(
    vertices: list[tuple[float, ...]],
    *,
    stride: int = cr.D9_VERTEX_STRIDE,
    primitive: int = cr.TRIANGLE_LIST,
    primitives: int = 1,
    start: int = 0,
    indexed: bool = False,
    indices: list[int] | None = None,
    index_size: int = 2,
    base_vertex: int = 0,
    min_vertex: int = 0,
    vertex_count: int | None = None,
    viewport: tuple[int, int, int, int] = (0, 0, 16, 16),
    target: tuple[int, int] | None = None,
    state: dict | None = None,
    texture: tuple[int, int, int, bytes] | None = None,
    clear_color: int = 0xFF000000,
    frame: int = 1,
    declaration_active: bool = False,
) -> bytes:
    if stride == cr.D9_VERTEX_STRIDE:
        vertex_bytes = b"".join(struct.pack("<5f", *row[:5]) for row in vertices)
    elif stride == cr.D9_COLOR_VERTEX_STRIDE:
        vertex_bytes = b"".join(
            struct.pack("<10f", *(tuple(row[:9]) + (0.0,))) for row in vertices
        )
    else:
        # Malformed-stride fixture: valid bounded bytes, unsupported layout.
        vertex_bytes = b"".join(struct.pack("<5f", *row[:5]) + bytes(stride - 20)
                                for row in vertices)
    if indices is None:
        indices = []
    if index_size == 2:
        index_bytes = struct.pack(f"<{len(indices)}H", *indices) if indices else b""
    else:
        index_bytes = struct.pack(f"<{len(indices)}I", *indices) if indices else b""
    state_values = [
        0, 0, 4, 1, 0, 8, 0, 0, 2, 1, 1, 15,
        3, 3, 2, 2, 2, 0, 0, 0, 1, 0, 1, 0, 0,
    ]
    for key, value in (state or {}).items():
        names = list(cr.DrawState.__dataclass_fields__)
        state_values[names.index(key)] = value
    draw = struct.pack("<8I", primitive, start, primitives, int(indexed),
                       base_vertex & 0xFFFFFFFF, min_vertex,
                       len(vertices) if vertex_count is None else vertex_count, 0)
    vp = struct.pack("<4I2f", *viewport, 0.0, 1.0)
    if declaration_active:
        declaration = struct.pack("<21I", *([1, 1, 0, 0xFFFFFFFF, 1, 20] + [0] * 15))
        declaration += struct.pack("<HHBBBB", 0, 0, 2, 0, cr.USAGE_POSITION, 0)
    else:
        declaration = struct.pack("<21I", *([0, 0, 0, 0, 0] + [0] * 16))
    frame_payload = struct.pack("<9I", frame, 1, 1, clear_color, 1, 0, 0, 0, 0)
    tw, th = target or (viewport[2], viewport[3])
    target_payload = struct.pack("<6I", 0, 1, tw, th, cr.X8R8G8B8, 1)
    sections = [
        section(cr.SECTION_DRAW, draw),
        section(cr.SECTION_VIEWPORT, vp),
        section(cr.SECTION_DRAWSTATE, struct.pack("<25I", *state_values)),
        section(cr.SECTION_WORLD, matrix_identity()),
        section(cr.SECTION_VIEW, matrix_identity()),
        section(cr.SECTION_PROJECTION, matrix_identity()),
        section(cr.SECTION_VS, shader()),
        section(cr.SECTION_PS, shader()),
        section(cr.SECTION_DECLARATION, declaration),
        section(cr.SECTION_VERTICES, vertices_payload(vertex_bytes, stride, len(vertices))),
        section(cr.SECTION_INDICES, indices_payload(index_bytes, index_size if indices else 0,
                                                    len(indices))),
    ]
    if texture is not None:
        width, height, format_id, pixels = texture
        sections.append(section(cr.SECTION_TEXTURE,
                                texture_payload(pixels, width, height, format_id)))
    sections.extend((section(cr.SECTION_FRAME, frame_payload),
                     section(cr.SECTION_TARGET, target_payload),
                     section(cr.SECTION_END, b"")))
    header = struct.pack("<8sIIQII", b"D9CAPBIN", 3, 32, 0x123456789ABCDEF0,
                         0, len(sections))
    return header + b"".join(sections)


def read_png(path: Path) -> tuple[int, int, list[bytes]]:
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    offset = 8
    idat = bytearray()
    width = height = 0
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        crc = struct.unpack_from(">I", data, offset + 8 + size)[0]
        assert crc == zlib.crc32(kind + payload) & 0xFFFFFFFF
        if kind == b"IHDR":
            width, height, depth, colour, comp, filt, interlace = struct.unpack(">IIBBBBB", payload)
            assert (depth, colour, comp, filt, interlace) == (8, 6, 0, 0, 0)
        elif kind == b"IDAT":
            idat.extend(payload)
        offset += 12 + size
    raw = zlib.decompress(bytes(idat))
    stride = 1 + width * 4
    assert len(raw) == height * stride
    rows = []
    for y in range(height):
        assert raw[y * stride] == 0
        rows.append(raw[y * stride + 1:(y + 1) * stride])
    return width, height, rows


def pixel(rows: list[bytes], width: int, x: int, y: int) -> bytes:
    return rows[y][x * 4:x * 4 + 4]


class CaptureRasterTests(unittest.TestCase):
    def render(self, capture_bytes: bytes, position_space: str = "screen") -> tuple[dict, list[bytes], int]:
        capture = cr.parse_capture(capture_bytes, "synthetic.bin")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "frame.png"
            result = cr.render_captures([capture], output, position_space)
            width, height, rows = read_png(output)
        self.assertEqual((width, height), (capture.width, capture.height))
        return result, rows, width

    def test_list_strip_and_fan(self):
        square = [(1.0, 1.0, 0.0, 0.0, 0.0),
                  (12.0, 1.0, 0.0, 1.0, 0.0),
                  (1.0, 12.0, 0.0, 0.0, 1.0),
                  (12.0, 12.0, 0.0, 1.0, 1.0)]
        cases = [
            (cr.TRIANGLE_LIST, 2, square[:3] + square[1:]),
            (cr.TRIANGLE_STRIP, 2, square),
            (cr.TRIANGLE_FAN, 2, [square[0], square[1], square[3], square[2]]),
        ]
        for primitive, primitives, vertices in cases:
            with self.subTest(primitive=primitive):
                result, rows, width = self.render(make_capture(
                    vertices, primitive=primitive, primitives=primitives))
                self.assertEqual(result["captures"][0]["triangles"], 2)
                self.assertGreater(result["fragments"], 50)
                self.assertEqual(pixel(rows, width, 5, 5), b"\xff\xff\xff\xff")

    def test_expanded_strip_scratch_is_not_reinterpreted_as_strip(self):
        # Current COM d->scratch contains two already-expanded triangles even
        # though the original draw type remains TRIANGLESTRIP.
        vertices = [
            (1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0),
            (5.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0),
            (1.0, 5.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0),
            (9.0, 9.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0),
            (13.0, 9.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0),
            (9.0, 13.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0),
        ]
        result, rows, width = self.render(make_capture(
            vertices, stride=cr.D9_COLOR_VERTEX_STRIDE,
            primitive=cr.TRIANGLE_STRIP, primitives=2))
        self.assertEqual(result["captures"][0]["triangles"], 2)
        self.assertEqual(pixel(rows, width, 2, 2), b"\xff\x00\x00\xff")
        self.assertEqual(pixel(rows, width, 10, 10), b"\x00\xff\x00\xff")

    def test_indexed_16_and_32_use_colour_vertices(self):
        vertices = [
            (1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0),
            (12.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0),
            (1.0, 12.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0),
            (12.0, 12.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 1.0),
        ]
        for index_size in (2, 4):
            with self.subTest(index_size=index_size):
                result, rows, width = self.render(make_capture(
                    vertices, stride=cr.D9_COLOR_VERTEX_STRIDE, indexed=True,
                    indices=[0, 1, 2], index_size=index_size, vertex_count=4))
                self.assertEqual(result["captures"][0]["indices"], 3)
                rgba = pixel(rows, width, 3, 3)
                self.assertGreater(rgba[0], rgba[1])
                self.assertGreater(rgba[0], rgba[2])

    def test_texture_bilinear_blend_and_alpha_test(self):
        # One red X8R8G8B8 texel in B,G,R,A memory order.
        texture = (1, 1, cr.X8R8G8B8, b"\x00\x00\xff\xff")
        vertices = [
            (1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.5),
            (12.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 0.5),
            (1.0, 12.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.5),
        ]
        result, rows, width = self.render(make_capture(
            vertices, stride=cr.D9_COLOR_VERTEX_STRIDE, texture=texture,
            state={"blend": 1, "src_blend": 5, "dst_blend": 6,
                   "mag_filter": 1, "min_filter": 1},
            clear_color=0xFF0000FF))
        rgba = pixel(rows, width, 3, 3)
        self.assertGreater(rgba[0], 100)
        self.assertGreater(rgba[2], 100)
        self.assertLess(rgba[1], 5)
        self.assertGreater(result["fragments"], 0)

        result, rows, width = self.render(make_capture(
            vertices, stride=cr.D9_COLOR_VERTEX_STRIDE,
            state={"alpha_test": 1, "alpha_func": 5, "alpha_ref": 200}))
        self.assertEqual(result["fragments"], 0)
        self.assertEqual(pixel(rows, width, 3, 3), b"\x00\x00\x00\xff")

    def test_viewport_offset_and_target_dimensions(self):
        vertices = [(0.0, 0.0, 0.0, 0.0, 0.0),
                    (4.0, 0.0, 0.0, 1.0, 0.0),
                    (0.0, 4.0, 0.0, 0.0, 1.0)]
        result, rows, width = self.render(make_capture(
            vertices, viewport=(3, 2, 8, 8), target=(16, 16)))
        self.assertEqual((result["width"], result["height"]), (16, 16))
        self.assertEqual(pixel(rows, width, 3, 2), b"\xff\xff\xff\xff")
        self.assertEqual(pixel(rows, width, 2, 2), b"\x00\x00\x00\xff")
        self.assertEqual(pixel(rows, width, 12, 2), b"\x00\x00\x00\xff")

    def test_hash_and_stride_failures_are_named(self):
        data = bytearray(make_capture([(0.25, 0.25, 0.0, 0.0, 0.0)] * 3))
        # This float bit pattern occurs only in the vertex payload, not identity matrices.
        marker = b"\x00\x00\x80\x3e\x00\x00\x80\x3e"
        offset = data.find(marker)
        self.assertGreater(offset, 0)
        data[offset] ^= 1
        with self.assertRaisesRegex(cr.CaptureError, "VERTICES.*FNV-1a"):
            cr.parse_capture(bytes(data), "bad-hash.bin")
        with self.assertRaisesRegex(cr.CaptureError, "unsupported decoded vertex stride"):
            cr.parse_capture(make_capture([(0.0,) * 9], stride=24), "bad-stride.bin")

    def test_refuses_to_overwrite_input_capture(self):
        data = make_capture([(0.0, 0.0, 0.0, 0.0, 0.0)] * 3)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.bin"
            path.write_bytes(data)
            capture = cr.load_capture(path)
            with self.assertRaisesRegex(cr.CaptureError, "refusing to overwrite input capture"):
                cr.render_captures([capture], path, "screen")
            self.assertEqual(path.read_bytes(), data)

    def test_fixed_function_object_positions_use_matrices(self):
        # POSITION declaration + identity WVP maps clip (-1,-1)..(1,1) to viewport.
        data = make_capture([
            (-0.5, -0.5, 0.5, 0.0, 0.0),
            (0.5, -0.5, 0.5, 1.0, 0.0),
            (-0.5, 0.5, 0.5, 0.0, 1.0),
        ], viewport=(0, 0, 16, 16), declaration_active=True)
        capture = cr.parse_capture(data, "object.bin")
        self.assertEqual(cr._resolve_position_space(capture, "auto"), "object")
        with tempfile.TemporaryDirectory() as directory:
            result = cr.render_captures([capture], Path(directory) / "object.png", "auto")
            width, _height, rows = read_png(Path(directory) / "object.png")
        self.assertGreater(result["fragments"], 0)
        self.assertEqual(pixel(rows, width, 4, 8), b"\xff\xff\xff\xff")


if __name__ == "__main__":
    unittest.main(verbosity=2)
