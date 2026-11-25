"""DDS -> PNG decoding and the ETC1 hand-off to devkitPro ``tex3ds``.

We do **not** implement an ETC1 encoder.  The supported path is:

1. decode a DDS level to RGBA8 with :mod:`skyrim.dds` (reference decoder),
2. write a PNG (lossless, no colour management),
3. run devkitPro's ``tex3ds`` with ``-f etc1`` (or ``etc1a4``/``auto-etc1``)
   to produce a ``.t3x`` for the 3DS build.

``tex3ds`` lives at ``$DEVKITPRO/tools/bin/tex3ds`` (on this machine
``C:\\devkitPro\\tools\\bin\\tex3ds.exe``).  The tool is a build-time dependency;
nothing derived from the game is committed to the repository.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import zlib
from dataclasses import dataclass
from typing import Iterable, List, Optional, Sequence

from . import dds as dds_mod


class ConvertError(Exception):
    """Conversion failed or a required external tool is missing."""


# ---------------------------------------------------------------------------
# PNG writer
# ---------------------------------------------------------------------------

def _png_chunk(kind: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(kind)
    crc = zlib.crc32(payload, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


def write_png(path: str, width: int, height: int, rgba: bytes, *, compress: int = 9) -> int:
    """Write tightly packed top-down RGBA8 to an 8-bit RGBA PNG."""
    if len(rgba) != width * height * 4:
        raise ConvertError(
            "RGBA buffer is %d bytes, expected %d for %dx%d"
            % (len(rgba), width * height * 4, width, height)
        )
    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type: none
        raw += rgba[y * stride : (y + 1) * stride]
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png += _png_chunk(b"IHDR", header)
    png += _png_chunk(b"IDAT", zlib.compress(bytes(raw), compress))
    png += _png_chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(png)
    return len(png)


def dds_to_png(dds_data: bytes, out_path: str, level: int = 0) -> dict:
    """Decode one DDS mip to RGBA8 and write it as a PNG."""
    info = dds_mod.parse_dds(dds_data)
    width, height, rgba = dds_mod.decode_level(dds_data, info, level)
    size = write_png(out_path, width, height, rgba)
    return {
        "path": out_path,
        "width": width,
        "height": height,
        "png_bytes": size,
        "source_bytes": len(dds_data),
        "source_format": info.format_name,
        "bc_mip_bytes": info.levels[level].size,
        "etc1_mip_bytes": ((width + 3) // 4) * ((height + 3) // 4) * 8,
    }


# ---------------------------------------------------------------------------
# tex3ds
# ---------------------------------------------------------------------------

_TEX3DS_CANDIDATES = (
    r"C:\devkitPro\tools\bin\tex3ds.exe",
    r"C:\devkitPro\tools\bin\tex3ds",
    "/opt/devkitpro/tools/bin/tex3ds",
    "/usr/local/bin/tex3ds",
)


def find_tex3ds(explicit: Optional[str] = None) -> str:
    if explicit:
        if os.path.isfile(explicit):
            return explicit
        raise ConvertError("tex3ds not found at %s" % explicit)
    for env in ("TEX3DS", "SKYRIM_TEX3DS"):
        value = os.environ.get(env)
        if value:
            if os.path.isfile(value):
                return value
            raise ConvertError("%s points at %s, which is not a file" % (env, value))
    found = shutil.which("tex3ds") or shutil.which("tex3ds.exe")
    if found:
        return found
    for candidate in _TEX3DS_CANDIDATES:
        if os.path.isfile(candidate):
            return candidate
    raise ConvertError(
        "tex3ds not found; install devkitPro tools or set TEX3DS to its path"
    )


def tex3ds_version(tex3ds: Optional[str] = None) -> str:
    exe = find_tex3ds(tex3ds)
    out = subprocess.run([exe, "--version"], capture_output=True, text=True, check=False)
    text = (out.stdout or "") + (out.stderr or "")
    return text.strip().splitlines()[0] if text.strip() else "unknown"


def tex3ds_convert(
    png_path: str,
    out_path: str,
    *,
    fmt: str = "etc1",
    mipmaps: Optional[str] = None,
    compress: str = "none",
    quality: Optional[str] = None,
    tex3ds: Optional[str] = None,
    extra: Sequence[str] = (),
    timeout: int = 120,
) -> dict:
    """Run tex3ds on one PNG and return the measured output size."""
    exe = find_tex3ds(tex3ds)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    args: List[str] = [exe, "-f", fmt, "-z", compress, "-o", out_path]
    if mipmaps:
        args += ["-m", mipmaps]
    if quality:
        args += ["-q", quality]
    args += list(extra)
    args.append(png_path)
    result = subprocess.run(
        args, capture_output=True, text=True, check=False, timeout=timeout
    )
    if result.returncode != 0:
        raise ConvertError(
            "tex3ds failed (%d): %s"
            % (result.returncode, (result.stderr or result.stdout).strip())
        )
    size = os.path.getsize(out_path)
    return {
        "t3x_path": out_path,
        "format": fmt,
        "mipmaps": mipmaps,
        "compress": compress,
        "t3x_bytes": size,
        "png_bytes": os.path.getsize(png_path),
    }


@dataclass
class ConversionResult:
    name: str
    png_path: str
    t3x_path: str
    width: int
    height: int
    source_format: str
    bc_bytes: int
    etc1_bytes: int
    t3x_bytes: int
    png_bytes: int

    def ratio_vs_bc(self) -> float:
        return self.t3x_bytes / self.bc_bytes if self.bc_bytes else 0.0


def convert_dds_file(
    dds_data: bytes,
    name: str,
    out_dir: str,
    *,
    fmt: str = "etc1",
    tex3ds: Optional[str] = None,
) -> ConversionResult:
    """Decode the top mip, write PNG, and hand it to tex3ds for ETC1."""
    info = dds_mod.parse_dds(dds_data)
    width, height, rgba = dds_mod.decode_level(dds_data, info, 0)
    safe = name.replace("\\", "_").replace("/", "_")
    png_path = os.path.join(out_dir, safe + ".png")
    t3x_path = os.path.join(out_dir, safe + "." + fmt + ".t3x")
    png_bytes = write_png(png_path, width, height, rgba)
    out = tex3ds_convert(png_path, t3x_path, fmt=fmt, tex3ds=tex3ds)
    etc1 = ((width + 3) // 4) * ((height + 3) // 4) * 8
    return ConversionResult(
        name=name,
        png_path=png_path,
        t3x_path=t3x_path,
        width=width,
        height=height,
        source_format=info.format_name,
        bc_bytes=info.levels[0].size,
        etc1_bytes=etc1,
        t3x_bytes=out["t3x_bytes"],
        png_bytes=png_bytes,
    )
