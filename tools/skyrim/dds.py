"""DDS parsing and BC1/BC2/BC3/BC4/BC5 decoding to RGBA8.

Only the formats Skyrim 2011 actually ships are needed, but the legacy RGB
masks are handled too because a few UI textures are uncompressed.  The decoder
is host-side reference code: it is not intended to run on the 3DS and it does
not encode anything.  ETC1 encoding is delegated to devkitPro's ``tex3ds``
(see :mod:`skyrim.convert`).

BC1/BC4 store 8 bytes per 4x4 block (4 bpp); BC2/BC3/BC5 store 16 bytes per
block (8 bpp).  ETC1 also stores 8 bytes per 4x4 block, so an ETC1 surface is
exactly the BC1 size and exactly half the BC2/BC3/BC5 size -- with no mip
overhead difference.  ETC1A4 (needed for alpha) is 16 bytes per block.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

DDS_MAGIC = b"DDS "
DDS_HEADER_SIZE = 124
DDS_DATA_OFFSET = 128

DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PITCH = 0x8
DDSD_PIXELFORMAT = 0x1000
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000
DDSD_DEPTH = 0x800000

DDPF_ALPHAPIXELS = 0x1
DDPF_ALPHA = 0x2
DDPF_FOURCC = 0x4
DDPF_RGB = 0x40
DDPF_YUV = 0x200
DDPF_LUMINANCE = 0x20000

DDSCAPS_COMPLEX = 0x8
DDSCAPS_TEXTURE = 0x1000
DDSCAPS_MIPMAP = 0x400000

FOURCC_DXT1 = b"DXT1"
FOURCC_DXT2 = b"DXT2"
FOURCC_DXT3 = b"DXT3"
FOURCC_DXT4 = b"DXT4"
FOURCC_DXT5 = b"DXT5"
FOURCC_ATI1 = b"ATI1"
FOURCC_BC4U = b"BC4U"
FOURCC_ATI2 = b"ATI2"
FOURCC_BC5U = b"BC5U"
FOURCC_DX10 = b"DX10"

DXGI_FORMATS = {
    28: ("R8G8B8A8_UNORM", "rgba8"),
    29: ("R8G8B8A8_UNORM_SRGB", "rgba8"),
    49: ("R8G8_UNORM", "rg8"),
    61: ("R8_UNORM", "r8"),
    65: ("A8_UNORM", "a8"),
    71: ("BC1_UNORM", "bc1"),
    72: ("BC1_UNORM_SRGB", "bc1"),
    74: ("BC2_UNORM", "bc2"),
    75: ("BC2_UNORM_SRGB", "bc2"),
    77: ("BC3_UNORM", "bc3"),
    78: ("BC3_UNORM_SRGB", "bc3"),
    80: ("BC4_UNORM", "bc4"),
    81: ("BC4_SNORM", "bc4"),
    83: ("BC5_UNORM", "bc5"),
    84: ("BC5_SNORM", "bc5"),
    85: ("B5G6R5_UNORM", "b5g6r5"),
    86: ("B5G5R5A1_UNORM", "b5g5r5a1"),
    87: ("B8G8R8A8_UNORM", "bgra8"),
    88: ("B8G8R8X8_UNORM", "bgrx8"),
    90: ("B8G8R8A8_UNORM_SRGB", "bgra8"),
    91: ("B8G8R8X8_UNORM_SRGB", "bgrx8"),
}

BLOCK_BYTES = {"bc1": 8, "bc2": 16, "bc3": 16, "bc4": 8, "bc5": 16}
BPP = {"bc1": 4.0, "bc2": 8.0, "bc3": 8.0, "bc4": 4.0, "bc5": 8.0,
       "rgba8": 32.0, "bgra8": 32.0, "bgrx8": 32.0, "rgba8": 32.0,
       "r8": 8.0, "a8": 8.0, "rg8": 16.0, "b5g6r5": 16.0, "b5g5r5a1": 16.0,
       "rgb24": 24.0, "bgr24": 24.0, "rgba16": 16.0}


class DdsError(Exception):
    """Malformed or unsupported DDS data."""


@dataclass(frozen=True)
class DdsLevel:
    width: int
    height: int
    offset: int
    size: int


@dataclass
class DdsInfo:
    width: int
    height: int
    flags: int
    mip_count: int
    format_name: str
    kind: str
    fourcc: bytes
    dxgi_format: Optional[int]
    rgb_bit_count: int
    red_mask: int
    green_mask: int
    blue_mask: int
    alpha_mask: int
    data_offset: int
    levels: List[DdsLevel]
    payload_size: int

    @property
    def is_block_compressed(self) -> bool:
        return self.kind in BLOCK_BYTES

    @property
    def bits_per_pixel(self) -> float:
        return BPP.get(self.kind, float(self.rgb_bit_count or 0))

    @property
    def all_mips_size(self) -> int:
        return sum(level.size for level in self.levels)

    def describe(self) -> dict:
        return {
            "width": self.width,
            "height": self.height,
            "mip_count": self.mip_count,
            "format": self.format_name,
            "kind": self.kind,
            "bits_per_pixel": self.bits_per_pixel,
            "all_mips_size": self.all_mips_size,
            "payload_size": self.payload_size,
        }


def _log2_ceil(value: int) -> int:
    n = 0
    while (1 << n) < value:
        n += 1
    return n


def _mip_dimension(base: int, level: int) -> int:
    return max(1, base >> level)


def _level_size(width: int, height: int, kind: str) -> int:
    if kind in BLOCK_BYTES:
        return ((width + 3) // 4) * ((height + 3) // 4) * BLOCK_BYTES[kind]
    bpp = BPP.get(kind)
    if bpp is None:
        return 0
    if kind in ("rgb24", "bgr24"):
        return width * height * 3
    return (width * height * int(bpp)) // 8


def _legacy_kind(flags: int, bit_count: int, r: int, g: int, b: int, a: int) -> str:
    if flags & DDPF_LUMINANCE and bit_count == 8:
        return "r8"
    if flags & DDPF_ALPHA and bit_count == 8 and not (r or g or b):
        return "a8"
    if bit_count == 32 and a and r == 0x00FF0000 and g == 0x0000FF00 and b == 0x000000FF:
        return "bgra8"
    if bit_count == 32 and a == 0 and r == 0x00FF0000 and g == 0x0000FF00 and b == 0x000000FF:
        return "bgrx8"
    if bit_count == 32 and r == 0x000000FF and g == 0x0000FF00 and b == 0x00FF0000:
        return "rgba8"
    if bit_count == 24:
        if r == 0x00FF0000 and g == 0x0000FF00 and b == 0x000000FF:
            return "bgr24"
        if r == 0x000000FF and g == 0x0000FF00 and b == 0x00FF0000:
            return "rgb24"
    if bit_count == 16 and r == 0xF800 and g == 0x07E0 and b == 0x001F:
        return "b5g6r5"
    if bit_count == 16 and r == 0x7C00 and g == 0x03E0 and b == 0x001F:
        return "b5g5r5a1"
    raise DdsError(
        "unsupported uncompressed pixel format (flags 0x%x, %d bpp, "
        "masks r=0x%08x g=0x%08x b=0x%08x a=0x%08x)"
        % (flags, bit_count, r, g, b, a)
    )


def parse_dds(data: bytes, *, require_payload: bool = True) -> DdsInfo:
    """Parse the DDS header and enumerate mip levels.  No pixels are decoded.

    With ``require_payload=False`` only the header bytes are needed; the
    declared mip chain is kept even if the caller supplied a short prefix.
    """
    if len(data) < DDS_DATA_OFFSET:
        raise DdsError("file is shorter than a DDS header (%d bytes)" % len(data))
    if data[:4] != DDS_MAGIC:
        raise DdsError("not a DDS file (magic %r)" % data[:4])
    (header_size, flags, height, width, pitch_or_linear, depth, mip_count) = struct.unpack_from(
        "<7I", data, 4
    )
    if header_size != DDS_HEADER_SIZE:
        raise DdsError("unexpected DDS header size %d" % header_size)
    if width == 0 or height == 0:
        raise DdsError("zero-sized DDS surface %dx%d" % (width, height))
    if width > 32768 or height > 32768:
        raise DdsError("implausible DDS dimensions %dx%d" % (width, height))
    pf_size, pf_flags, fourcc_raw, bit_count, r_mask, g_mask, b_mask, a_mask = (
        struct.unpack_from("<8I", data, 76)
    )
    if pf_size != 32:
        raise DdsError("unexpected DDS pixel format size %d" % pf_size)
    fourcc = struct.pack("<I", fourcc_raw)
    dxgi_format: Optional[int] = None
    data_offset = DDS_DATA_OFFSET
    if fourcc == FOURCC_DX10:
        if len(data) < DDS_DATA_OFFSET + 20:
            raise DdsError("DX10 header is truncated")
        dxgi_format, resource_dim, misc, array_size, misc2 = struct.unpack_from(
            "<5I", data, DDS_DATA_OFFSET
        )
        if resource_dim != 3:  # DDS_DIMENSION_TEXTURE2D
            raise DdsError("DX10 resource dimension %d is not a 2D texture" % resource_dim)
        if array_size > 1:
            raise DdsError("DX10 texture arrays (%d slices) are not supported" % array_size)
        if dxgi_format not in DXGI_FORMATS:
            raise DdsError("unsupported DXGI format %d" % dxgi_format)
        format_name, kind = DXGI_FORMATS[dxgi_format]
        data_offset += 20
    elif fourcc == FOURCC_DXT1 or fourcc == FOURCC_DXT2:
        format_name, kind = ("DXT1", "bc1")
    elif fourcc == FOURCC_DXT3 or fourcc == FOURCC_DXT4:
        format_name, kind = ("DXT3", "bc2")
    elif fourcc == FOURCC_DXT5:
        format_name, kind = ("DXT5", "bc3")
    elif fourcc in (FOURCC_ATI1, FOURCC_BC4U):
        format_name, kind = ("ATI1/BC4", "bc4")
    elif fourcc in (FOURCC_ATI2, FOURCC_BC5U):
        format_name, kind = ("ATI2/BC5", "bc5")
    elif pf_flags & DDPF_FOURCC:
        raise DdsError("unsupported FourCC %r" % fourcc)
    else:
        kind = _legacy_kind(pf_flags, bit_count, r_mask, g_mask, b_mask, a_mask)
        format_name = kind
        fourcc = b"\x00\x00\x00\x00"

    if mip_count == 0 or not (flags & DDSD_MIPMAPCOUNT):
        mip_count = 1
    max_mips = max(_log2_ceil(width), _log2_ceil(height)) + 1
    mip_count = max(1, min(mip_count, max_mips))

    levels: List[DdsLevel] = []
    offset = data_offset
    for level in range(mip_count):
        w = _mip_dimension(width, level)
        h = _mip_dimension(height, level)
        size = _level_size(w, h, kind)
        if size == 0:
            raise DdsError("cannot size %s mip %d" % (kind, level))
        levels.append(DdsLevel(w, h, offset, size))
        offset += size
    payload = len(data) - data_offset
    if require_payload:
        if payload < levels[0].size:
            raise DdsError(
                "truncated DDS: %d payload bytes, top level needs %d"
                % (payload, levels[0].size)
            )
        # Real files occasionally declare more mips than they store.  Keep only
        # levels that are fully present rather than rejecting the whole texture.
        present: List[DdsLevel] = []
        for level in levels:
            if level.offset + level.size <= len(data):
                present.append(level)
            else:
                break
        levels = present
    return DdsInfo(
        width=width,
        height=height,
        flags=flags,
        mip_count=len(levels),
        format_name=format_name,
        kind=kind,
        fourcc=fourcc,
        dxgi_format=dxgi_format,
        rgb_bit_count=bit_count,
        red_mask=r_mask,
        green_mask=g_mask,
        blue_mask=b_mask,
        alpha_mask=a_mask,
        data_offset=data_offset,
        levels=levels,
        payload_size=payload,
    )


# ---------------------------------------------------------------------------
# Block decoders
# ---------------------------------------------------------------------------

def _expand565(c: int) -> Tuple[int, int, int]:
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    return r, g, b


def _color_table(c0: int, c1: int, allow_three: bool) -> List[Tuple[int, int, int, int]]:
    r0, g0, b0 = _expand565(c0)
    r1, g1, b1 = _expand565(c1)
    if not allow_three or c0 > c1:
        return [
            (r0, g0, b0, 255),
            (r1, g1, b1, 255),
            ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255),
            ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255),
        ]
    return [
        (r0, g0, b0, 255),
        (r1, g1, b1, 255),
        ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255),
        (0, 0, 0, 0),
    ]


def _decode_color_block(block: bytes, allow_three: bool) -> List[Tuple[int, int, int, int]]:
    c0, c1, bits = struct.unpack_from("<HHI", block, 0)
    table = _color_table(c0, c1, allow_three)
    out = []
    for i in range(16):
        out.append(table[(bits >> (2 * i)) & 3])
    return out


def _decode_bc1_block(block: bytes) -> List[Tuple[int, int, int, int]]:
    return _decode_color_block(block, allow_three=True)


def _decode_bc2_block(block: bytes) -> List[Tuple[int, int, int, int]]:
    alphas = struct.unpack_from("<Q", block, 0)[0]
    colors = _decode_color_block(block[8:16], allow_three=False)
    out = []
    for i in range(16):
        a = (alphas >> (4 * i)) & 0xF
        r, g, b, _ = colors[i]
        out.append((r, g, b, a * 17))
    return out


def _decode_alpha_values(a0: int, a1: int) -> List[int]:
    if a0 > a1:
        return [a0, a1] + [
            ((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)
        ]
    return [a0, a1] + [
        ((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)
    ] + [0, 255]


def _decode_alpha_block(block: bytes) -> List[int]:
    a0 = block[0]
    a1 = block[1]
    values = _decode_alpha_values(a0, a1)
    bits = block[2] | (block[3] << 8) | (block[4] << 16) | (block[5] << 24) | (
        block[6] << 32
    ) | (block[7] << 40)
    return [values[(bits >> (3 * i)) & 7] for i in range(16)]


def _decode_bc3_block(block: bytes) -> List[Tuple[int, int, int, int]]:
    alphas = _decode_alpha_block(block[:8])
    colors = _decode_color_block(block[8:16], allow_three=False)
    return [(c[0], c[1], c[2], a) for c, a in zip(colors, alphas)]


def _decode_bc4_block(block: bytes) -> List[Tuple[int, int, int, int]]:
    alphas = _decode_alpha_block(block[:8])
    return [(a, a, a, 255) for a in alphas]


def _decode_bc5_block(block: bytes) -> List[Tuple[int, int, int, int]]:
    r = _decode_alpha_block(block[:8])
    g = _decode_alpha_block(block[8:16])
    return [(rv, gv, 0, 255) for rv, gv in zip(r, g)]


_BLOCK_DECODERS = {
    "bc1": _decode_bc1_block,
    "bc2": _decode_bc2_block,
    "bc3": _decode_bc3_block,
    "bc4": _decode_bc4_block,
    "bc5": _decode_bc5_block,
}


def _mask_channel(value: int, mask: int) -> int:
    if not mask:
        return 255
    shift = (mask & -mask).bit_length() - 1
    maxv = mask >> shift
    v = (value & mask) >> shift
    if maxv == 0:
        return 255
    return (v * 255 + maxv // 2) // maxv


def _decode_uncompressed_level(
    data: bytes, info: DdsInfo, level: DdsLevel
) -> bytearray:
    w, h = level.width, level.height
    out = bytearray(w * h * 4)
    kind = info.kind
    payload = data[level.offset : level.offset + level.size]
    if kind in ("rgba8", "bgra8", "bgrx8"):
        bpp = 4
        for y in range(h):
            src = y * w * 4
            dst = y * w * 4
            for x in range(w):
                b0, b1, b2, b3 = payload[src : src + 4]
                if kind == "rgba8":
                    rgba = (b0, b1, b2, b3)
                else:
                    rgba = (b2, b1, b0, 255 if kind == "bgrx8" else b3)
                out[dst : dst + 4] = bytes(rgba)
                src += 4
                dst += 4
        return out
    if kind in ("rgb24", "bgr24"):
        for y in range(h):
            src = y * w * 3
            dst = y * w * 4
            for x in range(w):
                if kind == "rgb24":
                    r, g, b = payload[src : src + 3]
                else:
                    b, g, r = payload[src : src + 3]
                out[dst : dst + 4] = bytes((r, g, b, 255))
                src += 3
                dst += 4
        return out
    if kind == "r8":
        for i in range(w * h):
            v = payload[i]
            out[i * 4 : i * 4 + 4] = bytes((v, v, v, 255))
        return out
    if kind == "a8":
        for i in range(w * h):
            v = payload[i]
            out[i * 4 : i * 4 + 4] = bytes((v, v, v, v))
        return out
    if kind == "rg8":
        for i in range(w * h):
            r, g = payload[i * 2], payload[i * 2 + 1]
            out[i * 4 : i * 4 + 4] = bytes((r, g, 0, 255))
        return out
    if kind == "b5g6r5":
        for i in range(w * h):
            v = payload[i * 2] | (payload[i * 2 + 1] << 8)
            r, g, b = _expand565(v)
            out[i * 4 : i * 4 + 4] = bytes((r, g, b, 255))
        return out
    if kind == "b5g5r5a1":
        for i in range(w * h):
            v = payload[i * 2] | (payload[i * 2 + 1] << 8)
            r = (v >> 10) & 0x1F
            g = (v >> 5) & 0x1F
            b = v & 0x1F
            a = 255 if (v & 0x8000) else 0
            out[i * 4 : i * 4 + 4] = bytes(
                ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), a)
            )
        return out
    raise DdsError("decoder missing for %s" % kind)


def decode_level(data: bytes, info: DdsInfo, level: int = 0) -> Tuple[int, int, bytes]:
    """Decode one mip level to tightly packed RGBA8 (row-major, top-down)."""
    if level < 0 or level >= len(info.levels):
        raise DdsError("mip level %d is not stored (have %d)" % (level, len(info.levels)))
    lv = info.levels[level]
    if info.kind not in BLOCK_BYTES:
        return lv.width, lv.height, bytes(_decode_uncompressed_level(data, info, lv))
    w, h = lv.width, lv.height
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    block_size = BLOCK_BYTES[info.kind]
    needed = bw * bh * block_size
    payload = data[lv.offset : lv.offset + lv.size]
    if len(payload) < needed:
        raise DdsError("mip %d is truncated (%d of %d bytes)" % (level, len(payload), needed))
    decoder = _BLOCK_DECODERS[info.kind]
    out = bytearray(w * h * 4)
    pos = 0
    for by in range(bh):
        for bx in range(bw):
            pixels = decoder(payload[pos : pos + block_size])
            pos += block_size
            for py in range(4):
                y = by * 4 + py
                if y >= h:
                    continue
                row = (y * w + bx * 4) * 4
                for px in range(4):
                    x = bx * 4 + px
                    if x >= w:
                        continue
                    r, g, b, a = pixels[py * 4 + px]
                    out[row + px * 4 : row + px * 4 + 4] = bytes((r, g, b, a))
    return w, h, bytes(out)


def decode_dds(data: bytes, level: int = 0) -> Tuple[int, int, bytes]:
    info = parse_dds(data)
    return decode_level(data, info, level)


def load_dds(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


# ---------------------------------------------------------------------------
# Size models
# ---------------------------------------------------------------------------

def etc1_mipped_size(width: int, height: int, mips: int = 1) -> int:
    """Bytes for an ETC1 surface (8 bytes per 4x4 block, all mips)."""
    total = 0
    for level in range(max(1, mips)):
        w = max(1, width >> level)
        h = max(1, height >> level)
        total += ((w + 3) // 4) * ((h + 3) // 4) * 8
    return total


def etc1a4_mipped_size(width: int, height: int, mips: int = 1) -> int:
    """Bytes for an ETC1A4 surface (16 bytes per 4x4 block, all mips)."""
    return etc1_mipped_size(width, height, mips) * 2


def bc_mipped_size(width: int, height: int, kind: str, mips: int = 1) -> int:
    total = 0
    for level in range(max(1, mips)):
        total += _level_size(max(1, width >> level), max(1, height >> level), kind)
    return total


def summarize_dds(data: bytes) -> dict:
    info = parse_dds(data)
    out = info.describe()
    out["bc_size"] = info.all_mips_size
    out["etc1_size"] = etc1_mipped_size(info.width, info.height, info.mip_count)
    out["etc1a4_size"] = etc1a4_mipped_size(info.width, info.height, info.mip_count)
    return out
