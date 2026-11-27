#!/usr/bin/env python3
"""CI test for tools/menu_swf_probe.py without retail bytes.

The test builds a tiny FWS movie in memory (one opaque solid DefineShape
rectangle), runs the Python tool end to end when a built host probe is
available, and checks that the emitted PNG is a valid, non-empty RGBA8 image.
The PNG encoder and summary formatter are also exercised with a synthetic
summary so the Python half is covered even without a probe binary.
"""

import json
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import menu_swf_probe as probe_tool  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, what):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("FAIL: %s" % what)


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.n = 0

    def bits(self, value, count):
        for i in range(count - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.buf.append(self.acc & 0xFF)
                self.acc = 0
                self.n = 0

    def sbits(self, value, count):
        self.bits(value & ((1 << count) - 1), count)

    def align(self):
        if self.n:
            self.buf.append((self.acc << (8 - self.n)) & 0xFF)
            self.acc = 0
            self.n = 0

    def u8(self, v):
        self.buf.append(v & 0xFF)

    def u16(self, v):
        self.buf += struct.pack("<H", v & 0xFFFF)


def tag(code, payload):
    if len(payload) < 0x3F:
        return struct.pack("<H", (code << 6) | len(payload)) + payload
    return (struct.pack("<H", (code << 6) | 0x3F) +
            struct.pack("<I", len(payload)) + payload)


def build_synthetic_swf(size=2000):
    body = BitWriter()
    body.bits(15, 5)
    for v in (0, size, 0, size):
        body.sbits(v, 15)
    body.align()
    body.u16(0x1800)  # 24 fps, 8.8
    body.u16(1)       # one frame

    shape = BitWriter()
    shape.u16(1)
    shape.bits(15, 5)
    for v in (0, size, 0, size):
        shape.sbits(v, 15)
    shape.align()
    shape.u8(1)               # one fill style
    shape.u8(0x00)            # solid
    shape.u8(0xD0)            # RGB red
    shape.u8(0x20)
    shape.u8(0x20)
    shape.u8(0)               # no line styles
    shape.bits(4, 4)          # fill bits
    shape.bits(0, 4)          # line bits
    shape.bits(0, 1)          # style change
    shape.bits(0, 1)          # new styles
    shape.bits(0, 1)          # line style
    shape.bits(1, 1)          # fill style 1
    shape.bits(0, 1)          # fill style 0
    shape.bits(1, 1)          # move to
    shape.bits(15, 5)
    shape.sbits(0, 15)
    shape.sbits(0, 15)
    shape.bits(1, 4)          # fill 1
    for dx, dy in ((size, 0), (0, size), (-size, 0), (0, -size)):
        shape.bits(1, 1)      # edge
        shape.bits(1, 1)      # straight
        shape.bits(12, 4)     # stored delta width is 12 + 2
        shape.bits(1, 1)      # general line
        shape.sbits(dx, 14)
        shape.sbits(dy, 14)
    shape.bits(0, 1)
    shape.bits(0, 1)          # end shape
    shape.align()
    body.buf += tag(2, bytes(shape.buf))
    body.buf += tag(26, b"\x02\x01\x00\x01\x00")  # PlaceObject2 depth 1
    body.buf += tag(1, b"")                       # ShowFrame
    body.buf += tag(0, b"")                       # End
    total = 8 + len(body.buf)
    return (b"FWS\x06" + struct.pack("<I", total) + bytes(body.buf))


def decode_png(data):
    check(data[:8] == b"\x89PNG\r\n\x1a\n", "png signature")
    pos = 8
    width = height = None
    idat = b""
    while pos < len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack_from(">I", data, pos + 8 + length)
        check(crc == zlib.crc32(kind + payload) & 0xFFFFFFFF,
              "png chunk crc %s" % kind)
        if kind == b"IHDR":
            width, height, depth, ctype = struct.unpack(">IIBB", payload[:10])
            check(depth == 8 and ctype == 6, "png is RGBA8")
        elif kind == b"IDAT":
            idat += payload
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    check(len(raw) == (stride + 1) * height, "png row bytes")
    opaque = sum(1 for y in range(height)
                 for x in range(width)
                 if raw[y * (stride + 1) + 1 + x * 4 + 3])
    return width, height, opaque


def sample_stats():
    return {
        "swf_bytes": 1000, "tags": 4, "top_level_tags": 4, "show_frame": 1,
        "scene_twips": [2000, 2000], "define_shape": 1, "define_font2": 0,
        "define_font3": 0, "define_sprite": 0, "define_bits_lossless": 0,
        "define_bits_jpeg": 0, "records": 1, "sprite_records": 0,
        "mask_records": 0, "shape_records": 1, "text_records": 0,
        "unsupported_records": 0,
        "fill_styles": {"solid": 1, "gradient": 0, "focal": 0, "bitmap": 0},
        "shape_triangles": 2, "stroke_segments": 0, "text_definitions": 0,
        "text_nonempty": 0, "text_resolved": 0, "text_refused": 0,
        "text_glyphs": 0, "text_missing_glyphs": 0, "text_quads": 0,
        "atlas": {"size": 512, "placed_glyphs": 1, "used_height": 1,
                  "occupied_pixels": 1},
        "rgba": {"width": 100, "height": 100, "opaque_pixels": 10000,
                 "bbox": [0, 0, 100, 100]},
        "render_triangles": 2, "missing": "",
    }


def test_png_encoder():
    rgba = bytes([255, 0, 0, 255]) * (4 * 4)
    png = probe_tool.rgba_to_png(rgba, 4, 4)
    w, h, opaque = decode_png(png)
    check((w, h, opaque) == (4, 4, 16), "RGBA8 PNG round-trip")
    text = probe_tool.format_summary(sample_stats(), None)
    check("opaque_pixels=10000" in text and "NOT an engine frame" in text,
          "summary names the diagnostic render and reports opaque pixels")


def test_synthetic_render(tmp):
    swf = tmp / "synthetic.swf"
    swf.write_bytes(build_synthetic_swf())
    probe = probe_tool.find_probe(None)
    if not probe:
        print("SKIP: menu_swf_frame_probe binary not built; C self-test covers "
              "the synthetic SWF render")
        return
    out = tmp / "synthetic.png"
    rc = probe_tool.main(["--swf", str(swf), "--probe", str(probe),
                          "--out", str(out)])
    check(rc == 0, "tool exit status")
    w, h, opaque = decode_png(out.read_bytes())
    check((w, h) == (100, 100), "synthetic scene size")
    check(opaque == 100 * 100, "synthetic frame fully opaque (%d px)" % opaque)


def main():
    test_png_encoder()
    with tempfile.TemporaryDirectory(prefix="menu_swf_probe_test_") as d:
        test_synthetic_render(Path(d))
    print("menu swf probe test: %d checks, %d failures" % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
