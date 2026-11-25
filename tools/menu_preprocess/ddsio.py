"""Bounded DDS inspection/decoding shared by the menu preprocessors."""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple

try:  # PYTHONPATH=tools or direct script use
    from skyrim import dds as dds_mod
except ImportError:  # repository root, tools.menu_preprocess.*
    from tools.skyrim import dds as dds_mod  # type: ignore

from .common import (
    BudgetError,
    DataError,
    Limits,
    MemoryMeter,
    display_path,
    read_bounded,
)


@dataclass
class DdsSource:
    name: str
    data: bytes
    info: dds_mod.DdsInfo
    trailing_bytes: int

    def describe(self) -> dict:
        out = self.info.describe()
        out.update({
            "file_bytes": len(self.data),
            "trailing_bytes": self.trailing_bytes,
            "dxgi_format": self.info.dxgi_format,
        })
        return out


def load_dds_source(source: bytes | os.PathLike[str] | str, limits: Limits,
                    label: str = "DDS") -> Tuple[DdsSource, MemoryMeter]:
    limits.validate()
    meter = MemoryMeter(limit=limits.max_memory_bytes)
    if isinstance(source, (bytes, bytearray, memoryview)):
        if len(source) > limits.max_input_bytes:
            raise BudgetError(
                "%s is %d bytes, exceeds max_input_bytes %d"
                % (label, len(source), limits.max_input_bytes)
            )
        data = bytes(source)
        name = "<memory>"
    else:
        path = Path(source)
        name = display_path(path)
        data = read_bounded(path, limits.max_input_bytes, label)
    meter.acquire("source_dds", len(data), "load source")
    try:
        info = inspect_dds(data, limits, label)
    except Exception:
        meter.release("source_dds")
        raise
    return DdsSource(name, data, info,
                     max(0, len(data) - info.data_offset - info.all_mips_size)), meter


def inspect_dds(data: bytes, limits: Limits, label: str = "DDS",
                *, file_size: Optional[int] = None,
                linear_only: bool = True) -> dds_mod.DdsInfo:
    """Parse a header and reject truncation/unsupported encodings up front."""

    actual_size = len(data) if file_size is None else file_size

    if len(data) < 128:
        raise DataError("%s is shorter than a DDS header" % label)
    try:
        info = dds_mod.parse_dds(data[:148], require_payload=False)
    except dds_mod.DdsError as exc:
        raise DataError("%s: %s" % (label, exc)) from exc

    flags = struct.unpack_from("<I", data, 8)[0]
    declared_mips = struct.unpack_from("<I", data, 28)[0]
    if flags & dds_mod.DDSD_MIPMAPCOUNT:
        if declared_mips == 0 or declared_mips > 16:
            raise DataError("%s has invalid declared mip count %d"
                            % (label, declared_mips))
        if declared_mips != info.mip_count:
            raise DataError(
                "%s declares %d mips but only %d fit its dimensions"
                % (label, declared_mips, info.mip_count)
            )
    elif declared_mips not in (0, 1):
        raise DataError("%s has mip count without DDSD_MIPMAPCOUNT" % label)

    pixels = info.width * info.height
    if pixels > limits.max_pixels:
        raise BudgetError(
            "%s is %dx%d (%d pixels), exceeds max_pixels %d"
            % (label, info.width, info.height, pixels, limits.max_pixels)
        )
    format_upper = info.format_name.upper()
    if data[84:88] in (b"DXT2", b"DXT4"):
        raise DataError(
            "%s uses premultiplied %s alpha; straight-alpha menu contracts "
            "require DXT3/BC2 or DXT5/BC3 UNORM"
            % (label, data[84:88].decode("ascii"))
        )
    if "SNORM" in format_upper:
        raise DataError(
            "%s is SNORM; menu preprocessing requires UNORM source samples"
            % label
        )
    if "SRGB" in format_upper and linear_only:
        raise DataError(
            "%s is sRGB; menu normal/glyph contracts require linear LDR samples"
            % label
        )
    required = info.data_offset + info.all_mips_size
    if actual_size < required:
        raise DataError(
            "%s is truncated: needs %d bytes through mip %d, has %d"
            % (label, required, info.mip_count - 1, actual_size)
        )
    if info.width * info.height * 4 > limits.max_memory_bytes:
        raise BudgetError(
            "%s top-level RGBA decode alone needs %d bytes, exceeding "
            "max_memory_bytes %d"
            % (label, info.width * info.height * 4, limits.max_memory_bytes)
        )
    return info


def decode_level_accounted(data: bytes, info: dds_mod.DdsInfo, level: int,
                           meter: MemoryMeter, label: str) -> Tuple[int, int, bytes]:
    """Decode one mip and account for the decoder's temporary RGBA copy."""

    lv = info.levels[level]
    rgba_bytes = lv.width * lv.height * 4
    # skyrim.dds builds a bytearray and returns bytes, so both exist briefly.
    meter.acquire("decoder_workspace", rgba_bytes, "decode %s mip %d" % (label, level))
    try:
        width, height, rgba = dds_mod.decode_level(data, info, level)
    except dds_mod.DdsError as exc:
        raise DataError("%s mip %d: %s" % (label, level, exc)) from exc
    finally:
        meter.release("decoder_workspace")
    if len(rgba) != rgba_bytes or (width, height) != (lv.width, lv.height):
        raise DataError("%s mip %d decoder returned inconsistent dimensions"
                        % (label, level))
    meter.acquire("decoded_level", len(rgba), "decoded %s mip %d" % (label, level))
    return width, height, rgba


def inspect_dds_file(path: os.PathLike[str] | str, limits: Limits,
                     label: str = "DDS", *, linear_only: bool = True) -> Tuple[dds_mod.DdsInfo, int]:
    """Inspect a DDS using only its header plus the file size."""

    p = Path(path)
    try:
        size = p.stat().st_size
        with p.open("rb") as handle:
            prefix = handle.read(148)
    except OSError as exc:
        raise DataError("%s: cannot read %s: %s" % (label, p, exc)) from exc
    if size > limits.max_input_bytes:
        raise BudgetError("%s exceeds max_input_bytes %d" % (label,
                                                             limits.max_input_bytes))
    return inspect_dds(prefix, limits, label, file_size=size,
                       linear_only=linear_only), size


def rgba8_dds_header(width: int, height: int, mips: int) -> bytes:
    """Build a legacy 128-byte DDS header for linear RGBA8, top-down rows."""

    if width <= 0 or height <= 0 or not 1 <= mips <= 16:
        raise DataError("invalid RGBA8 DDS dimensions/mip count")
    flags = (dds_mod.DDSD_CAPS | dds_mod.DDSD_HEIGHT | dds_mod.DDSD_WIDTH
             | dds_mod.DDSD_PITCH | dds_mod.DDSD_PIXELFORMAT)
    caps = dds_mod.DDSCAPS_TEXTURE
    if mips > 1:
        flags |= dds_mod.DDSD_MIPMAPCOUNT
        caps |= dds_mod.DDSCAPS_COMPLEX | dds_mod.DDSCAPS_MIPMAP
    header = bytearray(128)
    header[0:4] = dds_mod.DDS_MAGIC
    struct.pack_into("<7I", header, 4, dds_mod.DDS_HEADER_SIZE, flags,
                     height, width, width * 4, 0, mips)
    struct.pack_into("<8I", header, 76, 32,
                     dds_mod.DDPF_RGB | dds_mod.DDPF_ALPHAPIXELS,
                     0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000,
                     0xFF000000)
    struct.pack_into("<5I", header, 108, caps, 0, 0, 0, 0)
    return bytes(header)


def rgba8_output_size(info: dds_mod.DdsInfo) -> int:
    return 128 + sum(level.width * level.height * 4 for level in info.levels)


__all__ = [
    "DdsSource", "load_dds_source", "inspect_dds", "inspect_dds_file",
    "decode_level_accounted", "rgba8_dds_header", "rgba8_output_size",
]
