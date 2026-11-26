"""Offline preparation of light mods for the New 3DS port.

``textures`` downscales DDS surfaces to 256/128 px, optionally drops mips and
encodes ETC1/ETC1A4 (SKTX) or BC1 (DDS).  ``pack`` writes a v104 BSA that this
port's reader indexes.  ``mesh-plan`` only *reports* LOD/decimation candidates:
geometry rewriting is deliberately not automatic, because a bad decimation
silently changes collision/visuals and there is no runtime fallback yet.

Original game files are only read.  Output goes to a caller-supplied directory,
never back into ``Data``; the pack step is explicit.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import struct
import sys
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

try:
    from . import bsa_write, dds, etc1
except ImportError:  # direct script execution
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from skyrim import bsa_write, dds, etc1

# ETC1 modifier table, same order as etc1._ETC1_MODIFIERS.
_MODS = (2, 8, -2, -8, 5, 17, -5, -17, 9, 29, -9, -29,
         13, 42, -13, -42, 18, 60, -18, -60, 24, 80, -24, -80,
         33, 106, -33, -106, 47, 183, -47, -183)

OUTPUT_FORMATS = ("etc1", "etc1a4", "bc1", "dds")


class OptimizeError(Exception):
    pass


def _clamp(value: int, low: int = 0, high: int = 255) -> int:
    return low if value < low else high if value > high else value


def _target_size(width: int, height: int, max_dim: int) -> Tuple[int, int]:
    """Scale so the longer side is <= max_dim, keep aspect, multiples of 4."""
    if max_dim < 4:
        raise OptimizeError("max_dim must be >= 4")
    scale = min(1.0, max_dim / float(max(width, height)))
    def snap(value: float) -> int:
        return max(4, int(round(value / 4.0)) * 4)
    return snap(width * scale), snap(height * scale)


def resize_rgba(rgba: bytes, width: int, height: int,
                new_width: int, new_height: int) -> bytes:
    """Box-filter resize (area average), byte-exact for integer ratios."""
    if (width, height) == (new_width, new_height):
        return bytes(rgba)
    out = bytearray(new_width * new_height * 4)
    x_ratio = width / float(new_width)
    y_ratio = height / float(new_height)
    for y in range(new_height):
        y0 = int(y * y_ratio)
        y1 = max(y0 + 1, int((y + 1) * y_ratio))
        for x in range(new_width):
            x0 = int(x * x_ratio)
            x1 = max(x0 + 1, int((x + 1) * x_ratio))
            r = g = b = a = count = 0
            for sy in range(y0, min(y1, height)):
                base = (sy * width + x0) * 4
                for sx in range(x0, min(x1, width)):
                    r += rgba[base]
                    g += rgba[base + 1]
                    b += rgba[base + 2]
                    a += rgba[base + 3]
                    base += 4
                    count += 1
            o = (y * new_width + x) * 4
            if count:
                out[o] = r // count
                out[o + 1] = g // count
                out[o + 2] = b // count
                out[o + 3] = a // count
            else:
                out[o + 3] = 255
    return bytes(out)


def _mip_chain(rgba: bytes, width: int, height: int,
               min_dim: int = 8) -> List[Tuple[int, int, bytes]]:
    levels = [(width, height, rgba)]
    w, h = width, height
    while w >= min_dim and h >= min_dim and min(w, h) > min_dim:
        nw, nh = max(1, w // 2), max(1, h // 2)
        if nw < min_dim or nh < min_dim:
            break
        rgba = resize_rgba(rgba, w, h, nw, nh)
        levels.append((nw, nh, rgba))
        w, h = nw, nh
    return levels


# ---------------------------------------------------------------------------
# BC1
# ---------------------------------------------------------------------------


def _rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def _from565(value: int) -> Tuple[int, int, int]:
    r = (value >> 11) & 0x1F
    g = (value >> 5) & 0x3F
    b = value & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def encode_bc1_block(pixels: Sequence[Tuple[int, int, int, int]]) -> bytes:
    """4x4 RGB block -> 8 bytes; 4-colour BC1, opaque."""
    if len(pixels) != 16:
        raise OptimizeError("BC1 block needs 16 pixels")
    lo = [255, 255, 255]
    hi = [0, 0, 0]
    for r, g, b, _a in pixels:
        for channel, value in enumerate((r, g, b)):
            lo[channel] = min(lo[channel], value)
            hi[channel] = max(hi[channel], value)
    c0 = _rgb565(*hi)
    c1 = _rgb565(*lo)
    if c0 < c1:
        c0, c1 = c1, c0
    p0 = _from565(c0)
    p1 = _from565(c1)
    if c0 == c1:
        palette = [p0, p1, p0, p1]
    else:
        palette = [
            p0,
            p1,
            tuple((2 * p0[i] + p1[i]) // 3 for i in range(3)),
            tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3)),
        ]
    bits = 0
    for index, (r, g, b, _a) in enumerate(pixels):
        best = 0
        best_error = 1 << 30
        for slot, (pr, pg, pb) in enumerate(palette):
            error = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
            if error < best_error:
                best_error = error
                best = slot
        bits |= best << (2 * index)
    return struct.pack("<HHI", c0, c1, bits)


def encode_bc1(rgba: bytes, width: int, height: int) -> bytes:
    out = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            pixels = []
            for y in range(4):
                for x in range(4):
                    sx = min(bx + x, width - 1)
                    sy = min(by + y, height - 1)
                    o = (sy * width + sx) * 4
                    pixels.append((rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]))
            out += encode_bc1_block(pixels)
    return bytes(out)


def _dds_header_bc1(width: int, height: int, levels: int) -> bytes:
    flags = 0x00081007  # CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE|MIPMAPCOUNT
    linear = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 8
    caps = 0x00401008  # COMPLEX|TEXTURE|MIPMAP
    header = struct.pack("<4s7I", b"DDS ", 124, flags, height, width,
                         linear, 0, levels)
    header += b"\x00" * 44  # reserved
    header += struct.pack("<II4s5I", 32, 0x4, b"DXT1", 0, 0, 0, 0, 0)
    header += struct.pack("<5I", caps, 0, 0, 0, 0)
    return header


def write_bc1_dds(path: str, levels: Sequence[Tuple[int, int, bytes]]) -> int:
    width, height = levels[0][0], levels[0][1]
    blob = bytearray(_dds_header_bc1(width, height, len(levels)))
    for w, h, rgba in levels:
        blob += encode_bc1(rgba, w, h)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(blob)
    return len(blob)


# ---------------------------------------------------------------------------
# ETC1
# ---------------------------------------------------------------------------


def _etc1_subblock(pixels: Sequence[Tuple[int, int, int]]) -> Tuple[int, int, int, int]:
    """Return (r4, g4, b4, 16 selection bits) for 8 pixels in index order."""
    avg = [sum(p[i] for p in pixels) / 8.0 for i in range(3)]
    base = [min(15, max(0, int(round(a / 17.0)))) for a in avg]
    base8 = [value * 17 for value in base]
    best = None
    for cw in range(8):
        total = 0
        sels = []
        for p in pixels:
            best_error = 1 << 30
            best_sel = 0
            for sel in range(4):
                delta = _MODS[cw * 4 + sel]
                error = sum((_clamp(base8[i] + delta) - p[i]) ** 2 for i in range(3))
                if error < best_error:
                    best_error = error
                    best_sel = sel
            total += best_error
            sels.append(best_sel)
        if best is None or total < best[0]:
            bits = 0
            for index, sel in enumerate(sels):
                bits |= (sel & 1) << index
                bits |= ((sel >> 1) & 1) << (index + 8)
            best = (total, cw, bits)
    return base[0], base[1], base[2], best[1], best[2]


def encode_etc1_block(pixels: Sequence[Tuple[int, int, int, int]]) -> bytes:
    """Encode one 4x4 RGBA block (rows top-down) as 8 ETC1 bytes."""
    if len(pixels) != 16:
        raise OptimizeError("ETC1 block needs 16 pixels")
    rgb = [(p[0], p[1], p[2]) for p in pixels]
    left = [rgb[(index >> 2) * 4 + (index & 3)] for index in range(8)]
    right = [rgb[((index >> 2) + 2) * 4 + (index & 3)] for index in range(8)]
    r1, g1, b1, cw1, sel1 = _etc1_subblock(left)
    r2, g2, b2, cw2, sel2 = _etc1_subblock(right)
    high = (0) | (0 << 1) | (cw2 << 2) | (cw1 << 5)
    high |= (b2 & 0xF) << 8
    high |= (b1 & 0xF) << 12
    high |= (g2 & 0xF) << 16
    high |= (g1 & 0xF) << 20
    high |= (r2 & 0xF) << 24
    high |= (r1 & 0xF) << 28
    low = 0
    # Selection bits: subblock 0 uses bits 0..7 and 15..22; subblock 1 uses
    # bits 8..15 and 23..30 (matches etc1.decode_etc1_block).
    for index in range(8):
        bit = (sel1 >> index) & 1
        low |= bit << index
        low |= ((sel1 >> (index + 8)) & 1) << (index + 15)
        low |= ((sel2 >> index) & 1) << (index + 8)
        low |= ((sel2 >> (index + 8)) & 1) << (index + 23)
    value = (high << 32) | low
    return value.to_bytes(8, "little")


def _alpha_nibbles(pixels: Sequence[Tuple[int, int, int, int]]) -> bytes:
    out = bytearray(8)
    for index, pixel in enumerate(pixels):
        nibble = min(15, max(0, int(round(pixel[3] / 17.0))))
        if index & 1:
            out[index >> 1] |= nibble << 4
        else:
            out[index >> 1] |= nibble
    return bytes(out)


def encode_etc1_tiled(rgba: bytes, width: int, height: int,
                      alpha: bool = False) -> bytes:
    """PICA 8x8-tiled ETC1/ETC1A4 level (quadrant order, alpha first)."""
    tiles_x = (width + 7) // 8
    tiles_y = (height + 7) // 8
    out = bytearray()
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            for quadrant in range(4):
                bx = tx * 2 + (quadrant & 1)
                by = ty * 2 + (quadrant >> 1)
                pixels = []
                for y in range(4):
                    for x in range(4):
                        sx = bx * 4 + x
                        sy = by * 4 + y
                        if sx >= width or sy >= height:
                            pixels.append((0, 0, 0, 255))
                        else:
                            o = (sy * width + sx) * 4
                            pixels.append((rgba[o], rgba[o + 1], rgba[o + 2],
                                           rgba[o + 3]))
                if alpha:
                    out += _alpha_nibbles(pixels)
                out += encode_etc1_block(pixels)
    return bytes(out)


# ---------------------------------------------------------------------------
# Texture optimisation
# ---------------------------------------------------------------------------


@dataclass
class TexturePolicy:
    max_dim: int = 256
    fmt: str = "etc1"
    mips: bool = True
    keep_alpha: bool = True

    def validate(self) -> None:
        if self.fmt not in ("etc1", "etc1a4", "bc1"):
            raise OptimizeError("unknown output format %r" % self.fmt)
        if self.max_dim < 8 or self.max_dim & (self.max_dim - 1):
            raise OptimizeError("max_dim must be a power of two >= 8")
        if self.fmt == "etc1a4":
            self.keep_alpha = True


def optimize_texture(data: bytes, name: str, out_dir: str,
                     policy: Optional[TexturePolicy] = None) -> dict:
    policy = policy or TexturePolicy()
    policy.validate()
    info = dds.parse_dds(data)
    width, height, rgba = dds.decode_dds(data, 0)
    tw, th = _target_size(width, height, policy.max_dim)
    rgba = resize_rgba(rgba, width, height, tw, th)
    levels = _mip_chain(rgba, tw, th) if policy.mips else [(tw, th, rgba)]

    rel = name.replace("/", os.sep).replace("\\", os.sep).lstrip(os.sep)
    if policy.fmt == "bc1":
        out_path = os.path.join(out_dir, rel[:-4] + ".dds" if rel.lower().endswith(".dds") else rel + ".dds")
        size = write_bc1_dds(out_path, levels)
        container = "dds-bc1"
    else:
        fmt = "etc1a4" if policy.fmt == "etc1a4" else "etc1"
        payload = [(w, h, encode_etc1_tiled(rgba_level, w, h, fmt == "etc1a4"))
                   for w, h, rgba_level in levels]
        out_path = os.path.join(out_dir, (rel[:-4] if rel.lower().endswith(".dds") else rel)
                                + ".sktx")
        size = etc1.write_sktx(out_path, tw, th, fmt, payload)
        container = "sktx-" + fmt
    with open(out_path, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    return {
        "name": name,
        "source": {"width": width, "height": height,
                   "format": info.format_name, "mips": info.mip_count,
                   "bytes": len(data)},
        "target": {"width": tw, "height": th, "levels": len(levels),
                   "container": container, "bytes": size},
        "downscaled": (tw, th) != (width, height),
        "output": os.path.abspath(out_path),
        "sha256": digest,
    }


def optimize_tree(src: str, out: str, policy: Optional[TexturePolicy] = None,
                  *, extensions: Sequence[str] = (".dds",)) -> dict:
    policy = policy or TexturePolicy()
    policy.validate()
    rows = []
    errors = []
    for base, dirs, files in os.walk(src):
        dirs.sort(key=str.lower)
        for name in sorted(files, key=str.lower):
            if not name.lower().endswith(tuple(extensions)):
                continue
            full = os.path.join(base, name)
            rel = os.path.relpath(full, src)
            try:
                with open(full, "rb") as handle:
                    data = handle.read()
                rows.append(optimize_texture(data, rel, out, policy))
            except (OSError, ValueError, etc1.TextureError, dds.DdsError,
                    OptimizeError) as exc:
                errors.append({"path": rel, "error": str(exc)})
    before = sum(row["source"]["bytes"] for row in rows)
    after = sum(row["target"]["bytes"] for row in rows)
    return {"source_root": os.path.abspath(src), "out_root": os.path.abspath(out),
            "textures": rows, "errors": errors,
            "bytes_before": before, "bytes_after": after,
            "ratio": (after / before) if before else 0.0}


def pack_bsa(output: str, files: Iterable[str], *,
             version: int = 104, compress: bool = True) -> dict:
    pairs = []
    root = None
    for path in files:
        with open(path, "rb") as handle:
            payload = handle.read()
        rel = os.path.basename(path)
        if root is None:
            root = os.path.dirname(os.path.abspath(path))
        try:
            rel = os.path.relpath(path, root)
        except ValueError:
            rel = os.path.basename(path)
        pairs.append((rel, payload))
    return bsa_write.write_bsa(output, pairs, version=version,
                               compressed=compress)


def mesh_plan(root: str, *, max_triangles: int = 60000) -> dict:
    """Report LOD/decimation candidates; never rewrites a mesh."""
    try:
        from . import nif
    except ImportError:
        from skyrim import nif
    rows = []
    for base, dirs, files in os.walk(root):
        dirs.sort(key=str.lower)
        for name in sorted(files, key=str.lower):
            if not name.lower().endswith(".nif"):
                continue
            full = os.path.join(base, name)
            try:
                parsed = nif.parse_nif_file(full)
                stats = parsed.geometry_stats()
                triangles = stats["totals"]["triangles"]
                rows.append({
                    "path": os.path.relpath(full, root),
                    "bytes": os.path.getsize(full),
                    "triangles": triangles,
                    "lod_triangles": stats["lod_totals"]["triangles"],
                    "candidate": triangles > max_triangles
                                 or stats["lod_totals"]["triangles"] > 0,
                    "action": ("strip LOD copy" if stats["lod_totals"]["triangles"]
                               else "decimate offline" if triangles > max_triangles
                               else "keep"),
                })
            except Exception as exc:
                rows.append({"path": os.path.relpath(full, root),
                             "error": str(exc)})
    return {
        "root": os.path.abspath(root),
        "meshes": rows,
        "note": "No mesh is modified. A decimation pass needs a validated "
                "vertex/index rewrite and a visual check; it is not automatic.",
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    tex = sub.add_parser("textures", help="downscale/encode a DDS tree")
    tex.add_argument("source")
    tex.add_argument("output")
    tex.add_argument("--max-dim", type=int, default=256)
    tex.add_argument("--format", choices=("etc1", "etc1a4", "bc1"),
                     default="etc1")
    tex.add_argument("--no-mips", action="store_true")
    tex.add_argument("--json", default=None)

    one = sub.add_parser("texture", help="one DDS file")
    one.add_argument("source")
    one.add_argument("output_dir")
    one.add_argument("--max-dim", type=int, default=256)
    one.add_argument("--format", choices=("etc1", "etc1a4", "bc1"),
                     default="etc1")
    one.add_argument("--no-mips", action="store_true")

    pack = sub.add_parser("pack", help="write a v104 BSA")
    pack.add_argument("output")
    pack.add_argument("files", nargs="+")
    pack.add_argument("--no-compress", action="store_true")

    mesh = sub.add_parser("mesh-plan", help="report mesh LOD candidates")
    mesh.add_argument("source")
    mesh.add_argument("--max-triangles", type=int, default=60000)

    args = parser.parse_args(argv)
    if args.command in ("textures", "texture", "mesh-plan"):
        try:
            if args.command == "textures":
                policy = TexturePolicy(args.max_dim, args.format, not args.no_mips)
                report = optimize_tree(args.source, args.output, policy=policy)
            elif args.command == "texture":
                policy = TexturePolicy(args.max_dim, args.format, not args.no_mips)
                with open(args.source, "rb") as handle:
                    data = handle.read()
                report = optimize_texture(data, os.path.basename(args.source),
                                          args.output_dir, policy)
            else:
                report = mesh_plan(args.source, max_triangles=args.max_triangles)
        except (OSError, ValueError, OptimizeError, etc1.TextureError) as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 2
        text = json.dumps(report, indent=2, sort_keys=True)
        if getattr(args, "json", None):
            with open(args.json, "w", encoding="utf-8") as handle:
                handle.write(text + "\n")
        print(text)
        return 0
    if args.command == "pack":
        try:
            report = pack_bsa(args.output, args.files,
                              compress=not args.no_compress)
        except (OSError, bsa_write.BsaWriteError) as exc:
            print("error: %s" % exc, file=sys.stderr)
            return 2
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
