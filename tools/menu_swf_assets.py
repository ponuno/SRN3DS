#!/usr/bin/env python3
"""Read-only external-asset closure for ``Interface/StartMenu.swf``.

The tool reads the movie out of ``Skyrim - Interface.bsa`` (read-only, through
``tools/skyrim/bsa.py`` -- the Python twin of the bounded C ``bsa_reader``),
walks every tag that can name or embed an asset, and resolves the ones that
point outside the movie through a ``Data`` overlay:

* ``ImportAssets`` (57) / ``ImportAssets2`` (71): URL + ``id -> symbol`` list.
  ``gfxfontlib.swf`` is a *virtual* GFx font library (no file in any archive);
  its symbols are resolved through ``Interface/fontconfig.txt``
  (``map "$Alias" = "Face" Style``) to a real ``DefineFont3`` in a ``fontlib``
  movie, with the same exact face + Bold/Italic predicate the runtime uses.
* ``DefineExternalImage`` (1000): character id + file name; resolved as a
  ``Data\\Interface\\...`` path through the overlay.
* ``DefineBits``/``DefineBitsJPEG2/3/4``/``DefineBitsLossless/2``
  (6/21/35/36/90/20): the image bytes are embedded in the tag.  They are
  reported with their dimensions and payload bytes as *inline* assets; there
  is no external path to resolve.  JPEG SOF dimensions are read without
  decoding.
* ``DefineFont``/``DefineFont2``/``DefineFont3`` (10/48/75): embedded glyphs.
* ``DefineSound`` (14): embedded audio; never an external file.

Overlay order matches ``menu_bsa_runtime``: a loose file under ``Data`` wins,
then archives are searched last-to-first so a later archive (``Update.bsa``)
overrides an earlier one (``Skyrim - Interface.bsa``).  Nothing is extracted
to disk and no game bytes are printed; only names, sizes and parsed metadata.

Usage::

    python tools/menu_swf_assets.py                     # measured install
    python tools/menu_swf_assets.py --data E:\\Skyrim\\Data
    python tools/menu_swf_assets.py --bsa "Skyrim - Interface.bsa" --json

Exit status is 0 when every asset required for drawing resolves, 1 otherwise.
Sound tracks are reported as deferred, never as missing.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

_HERE = Path(__file__).resolve().parent
ROOT = _HERE.parent if (_HERE.parent / "tools" / "skyrim").is_dir() else _HERE
sys.path.insert(0, str(ROOT / "tools"))

from skyrim import bsa  # noqa: E402

DEFAULT_DATA = Path(os.environ.get("SKYRIM_N3DS_DATA") or r"F:\Skyrim\Data")
DEFAULT_ARCHIVE = "Skyrim - Interface.bsa"
MOVIE = "interface/startmenu.swf"
FONTCONFIG = "interface/fontconfig.txt"
UPDATE_ARCHIVE = "Update.bsa"

# ------------------------------------------------------------------- tags ----
TAG_NAMES = {
    0: "End", 1: "ShowFrame", 2: "DefineShape", 4: "PlaceObject",
    5: "RemoveObject", 6: "DefineBits", 8: "JPEGTables", 9: "SetBackgroundColor",
    10: "DefineFont", 12: "DoAction", 14: "DefineSound", 15: "StartSound",
    18: "SoundStreamHead", 20: "DefineBitsLossless", 21: "DefineBitsJPEG2",
    22: "DefineShape2", 26: "PlaceObject2", 32: "DefineShape3",
    35: "DefineBitsJPEG3", 36: "DefineBitsLossless2", 37: "DefineEditText",
    39: "DefineSprite", 43: "FrameLabel", 45: "SoundStreamBlock",
    46: "DefineMorphShape", 48: "DefineFont2", 56: "ExportAssets",
    57: "ImportAssets", 59: "DoInitAction", 69: "FileAttributes",
    70: "PlaceObject3", 71: "ImportAssets2", 73: "DefineFontAlignZones",
    74: "CSMTextSettings", 75: "DefineFont3", 76: "SymbolClass",
    77: "Metadata", 78: "DefineScalingGrid", 83: "DefineShape4",
    88: "DefineFontName", 90: "DefineBitsJPEG4", 1000: "DefineExternalImage",
}

SPRITE_TAG = 39
BITMAP_TAGS = {6: "DefineBits", 20: "DefineBitsLossless", 21: "DefineBitsJPEG2",
               35: "DefineBitsJPEG3", 36: "DefineBitsLossless2",
               90: "DefineBitsJPEG4"}
JPEG_TAGS = {6, 21, 35, 90}
LOSSLESS_TAGS = {20, 36}
FONT_TAGS = {10: "DefineFont", 48: "DefineFont2", 75: "DefineFont3"}
SOUND_TAGS = {14: "DefineSound"}
EXTERNAL_IMAGE_TAGS = {1000: "DefineExternalImage"}
IMPORT_TAGS = {57: "ImportAssets", 71: "ImportAssets2"}

SOUND_RATES = {0: 5512, 1: 11025, 2: 22050, 3: 44100}


class SwfError(Exception):
    """Named refusal while parsing the SWF body."""


# -------------------------------------------------------------- container ----
@dataclass
class SwfTag:
    code: int
    name: str
    offset: int          # payload offset inside the decompressed body
    length: int
    payload: bytes
    depth: int = 0
    sprite_id: int | None = None


def swf_body(data: bytes) -> bytes:
    """Return the decompressed body (rect + frame header + tag stream)."""
    if len(data) < 8:
        raise SwfError("SWF is shorter than the 8-byte file header")
    sig = data[:3]
    if sig == b"FWS":
        return data[8:]
    if sig == b"CWS":
        try:
            body = zlib.decompress(data[8:])
        except zlib.error as exc:
            raise SwfError("CWS zlib stream: %s" % exc) from None
        declared = struct.unpack_from("<I", data, 4)[0]
        if declared != len(body) + 8:
            raise SwfError("CWS declares %d bytes, inflated %d"
                           % (declared, len(body) + 8))
        return body
    if sig == b"ZWS":
        raise SwfError("ZWS (LZMA) container is not supported by this tool")
    raise SwfError("unknown SWF signature %r" % sig)


def frame_header(body: bytes) -> dict:
    """Decode the RECT/frame-rate/frame-count header and the tag offset."""
    if not body:
        raise SwfError("empty SWF body")
    nbits = (body[0] >> 3) & 0x1F
    if nbits == 0:
        raise SwfError("RECT declares zero bits")
    bits = 5 + 4 * nbits
    byte_len = (bits + 7) // 8
    if len(body) < byte_len + 4:
        raise SwfError("truncated SWF frame header")
    acc = 0
    for byte in body[:byte_len]:
        acc = (acc << 8) | byte
    total = byte_len * 8
    shift = total - 5
    values = []
    for _ in range(4):
        shift -= nbits
        raw = (acc >> shift) & ((1 << nbits) - 1)
        if raw & (1 << (nbits - 1)):
            raw -= 1 << nbits
        values.append(raw)
    rate = struct.unpack_from("<H", body, byte_len)[0]
    count = struct.unpack_from("<H", body, byte_len + 2)[0]
    return {
        "rect_twips": values,
        "width_twips": values[1] - values[0],
        "height_twips": values[3] - values[2],
        "frame_rate": rate / 256.0,
        "frame_count": count,
        "tag_offset": byte_len + 4,
    }


def iter_tags(body: bytes, start: int, end: int | None = None, *,
              depth: int = 0, sprite_id: int | None = None):
    """Yield every tag, descending into DefineSprite tag streams."""
    end = len(body) if end is None else end
    pos = start
    while pos + 2 <= end:
        header = struct.unpack_from("<H", body, pos)[0]
        pos += 2
        code = header >> 6
        length = header & 0x3F
        if length == 0x3F:
            if pos + 4 > end:
                raise SwfError("truncated long tag header at %d" % (pos - 2))
            length = struct.unpack_from("<I", body, pos)[0]
            pos += 4
        if pos + length > end:
            raise SwfError("tag %d length %d runs past body at %d"
                           % (code, length, pos - 2))
        payload = body[pos:pos + length]
        yield SwfTag(code, TAG_NAMES.get(code, "Tag%d" % code), pos, length,
                     payload, depth, sprite_id)
        if code == SPRITE_TAG:
            if length < 4:
                raise SwfError("DefineSprite shorter than 4 bytes")
            child = struct.unpack_from("<H", payload, 0)[0]
            yield from iter_tags(body, pos + 4, pos + length,
                                 depth=depth + 1, sprite_id=child)
        pos += length
        if code == 0 and depth == 0:
            break


# ---------------------------------------------------------------- parsers ----
def _u16(payload: bytes, off: int) -> int:
    if off + 2 > len(payload):
        raise SwfError("truncated u16 at %d" % off)
    return struct.unpack_from("<H", payload, off)[0]


def _u32(payload: bytes, off: int) -> int:
    if off + 4 > len(payload):
        raise SwfError("truncated u32 at %d" % off)
    return struct.unpack_from("<I", payload, off)[0]


def _cstring(payload: bytes, off: int) -> tuple[str, int]:
    end = payload.find(b"\x00", off)
    if end < 0:
        raise SwfError("unterminated string at %d" % off)
    try:
        text = payload[off:end].decode("latin-1")
    except UnicodeDecodeError:  # pragma: no cover - latin-1 is total
        text = payload[off:end].decode("ascii", "replace")
    return text, end + 1


def parse_import_assets(tag: SwfTag) -> dict:
    payload = tag.payload
    url, off = _cstring(payload, 0)
    if tag.code == 71:
        if off + 2 > len(payload):
            raise SwfError("ImportAssets2 missing reserved bytes")
        off += 2
    count = _u16(payload, off)
    off += 2
    symbols = []
    for _ in range(count):
        cid = _u16(payload, off)
        off += 2
        name, off = _cstring(payload, off)
        symbols.append({"id": cid, "name": name})
    if off != len(payload):
        raise SwfError("ImportAssets has %d trailing bytes"
                       % (len(payload) - off))
    return {"code": tag.code, "kind": tag.name, "url": url,
            "symbols": symbols, "offset": tag.offset}


def parse_lossless(tag: SwfTag) -> dict:
    payload = tag.payload
    cid = _u16(payload, 0)
    if len(payload) < 7:
        raise SwfError("DefineBitsLossless payload shorter than 7 bytes")
    fmt = payload[2]
    width = _u16(payload, 3)
    height = _u16(payload, 5)
    return {"kind": tag.name, "id": cid, "format": fmt,
            "width": width, "height": height, "bytes": tag.length}


def jpeg_dimensions(data: bytes) -> tuple[int, int] | None:
    """Read SOF dimensions from a JPEG bitstream without decoding it."""
    if data[:2] != b"\xff\xd8":
        return None
    pos = 2
    while pos + 4 <= len(data):
        if data[pos] != 0xFF:
            pos += 1
            continue
        marker = data[pos + 1]
        pos += 2
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7 or marker == 0x01:
            continue
        seglen = struct.unpack_from(">H", data, pos)[0]
        if seglen < 2:
            return None
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if pos + 7 > len(data):
                return None
            height = struct.unpack_from(">H", data, pos + 3)[0]
            width = struct.unpack_from(">H", data, pos + 5)[0]
            return width, height
        if marker == 0xDA:
            return None
        pos += seglen
    return None


def parse_jpeg(tag: SwfTag) -> dict:
    payload = tag.payload
    cid = _u16(payload, 0)
    data_off = 2
    alpha_bytes = 0
    if tag.code in (35, 90):
        alpha_bytes = _u32(payload, 2)
        data_off = 6
    dims = jpeg_dimensions(payload[data_off:])
    return {"kind": tag.name, "id": cid,
            "width": dims[0] if dims else 0,
            "height": dims[1] if dims else 0,
            "alpha_bytes": alpha_bytes, "bytes": tag.length}


def parse_bitmap(tag: SwfTag) -> dict:
    return parse_lossless(tag) if tag.code in LOSSLESS_TAGS else parse_jpeg(tag)


def parse_font(tag: SwfTag) -> dict:
    payload = tag.payload
    cid = _u16(payload, 0)
    if tag.code == 10:
        # DefineFont: UI16 id then a UI16 offset table; no name or flag byte.
        return {"kind": tag.name, "id": cid, "name": "", "flags": 0,
                "bold": False, "italic": False, "glyphs": 0,
                "bytes": tag.length}
    if len(payload) < 5:
        raise SwfError("DefineFont2/3 payload shorter than 5 bytes")
    flags = payload[2]
    name_len = payload[4]
    if 5 + name_len + 2 > len(payload):
        raise SwfError("DefineFont2/3 name runs past the payload")
    # The name field is NUL-terminated and its stored length includes the NUL.
    name = payload[5:5 + name_len].decode("latin-1").rstrip("\x00")
    glyphs = _u16(payload, 5 + name_len)
    return {"kind": tag.name, "id": cid, "name": name, "flags": flags,
            "bold": bool(flags & 0x01), "italic": bool(flags & 0x02),
            "glyphs": glyphs, "bytes": tag.length}


def parse_sound(tag: SwfTag) -> dict:
    payload = tag.payload
    cid = _u16(payload, 0)
    if len(payload) < 7:
        raise SwfError("DefineSound payload shorter than 7 bytes")
    flags = payload[2]
    fmt = flags >> 4
    rate = SOUND_RATES[(flags >> 2) & 0x03]
    stereo = bool(flags & 0x01)
    samples = _u32(payload, 3)
    seconds = samples / float(rate) if rate else 0.0
    return {"kind": tag.name, "id": cid, "format": fmt, "rate": rate,
            "stereo": stereo, "samples": samples, "seconds": round(seconds, 3),
            "bytes": tag.length}


def parse_external_image(tag: SwfTag) -> dict:
    payload = tag.payload
    cid = _u16(payload, 0)
    name, off = _cstring(payload, 2)
    width = height = 0
    if off + 8 <= len(payload):
        width = _u32(payload, off)
        height = _u32(payload, off + 4)
    return {"kind": tag.name, "id": cid, "file": name,
            "width": width, "height": height, "bytes": tag.length}


# -------------------------------------------------------------- fontconfig ----
def parse_fontconfig(text: str) -> dict:
    """Parse ``fontlib`` and ``map`` lines (mirrors menu_swf_font.c)."""
    libs = []
    maps = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        key = line.split(None, 1)[0].lower()
        rest = line[len(key):].strip()
        if key == "fontlib":
            if len(rest) >= 2 and rest[0] == '"' and rest[-1] == '"':
                libs.append(rest[1:-1].replace("\\", "/"))
        elif key == "map":
            alias, _, tail = rest.partition("=")
            alias = alias.strip()
            tail = tail.strip()
            if not (alias.startswith('"') and alias.endswith('"')):
                continue
            alias = alias[1:-1]
            face = ""
            style = ""
            if tail.startswith('"'):
                end = tail.find('"', 1)
                if end > 0:
                    face = tail[1:end]
                    style = tail[end + 1:].strip()
            if not face:
                continue
            tokens = [t.lower() for t in style.replace(",", " ").split()]
            maps.append({"alias": alias, "face": face, "style": style,
                         "bold": "bold" in tokens,
                         "italic": "italic" in tokens})
    return {"fontlibs": libs, "maps": maps}


def _name_equal(a: str, b: str) -> bool:
    return a.rstrip(" \t\x00").lower() == b.rstrip(" \t\x00").lower()


# ----------------------------------------------------------------- overlay ----
@dataclass
class ResolvedAsset:
    path: str
    source: str            # "loose" or the winning archive name
    size: int
    stored: int = 0

    def as_dict(self) -> dict:
        return {"path": self.path, "source": self.source,
                "size": self.size, "stored": self.stored}


class DataOverlay:
    """Read-only ``Data`` overlay: loose files, then archives last-to-first."""

    def __init__(self, data_dir: Path | str,
                 archive_names: tuple[str, ...] = (DEFAULT_ARCHIVE,
                                                   UPDATE_ARCHIVE)):
        self.data_dir = Path(data_dir)
        self.archives = []
        self.errors = []
        for name in archive_names:
            path = self.data_dir / name
            if not path.exists():
                continue
            try:
                self.archives.append((name, bsa.open_archive(str(path))))
            except Exception as exc:  # named, not silently skipped for lookup
                self.errors.append("%s: %s" % (name, exc))

    def close(self) -> None:
        for _, archive in self.archives:
            archive.close()
        self.archives.clear()

    def __enter__(self) -> "DataOverlay":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def resolve(self, virtual_path: str) -> ResolvedAsset | None:
        key = virtual_path.replace("/", "\\").strip("\\")
        loose = self.data_dir / key.replace("\\", "/")
        if loose.is_file():
            return ResolvedAsset(key, "loose", loose.stat().st_size)
        for name, archive in reversed(self.archives):
            entry = archive.find(key)
            if entry is None:
                continue
            try:
                size = archive.unpacked_size(key)
            except Exception:
                size = entry.size
            return ResolvedAsset(key, name, size, entry.size)
        return None


# ------------------------------------------------------------------ report ----
@dataclass
class AssetReport:
    movie: dict = field(default_factory=dict)
    bitmaps: list = field(default_factory=list)
    fonts: list = field(default_factory=list)
    sounds: list = field(default_factory=list)
    imports: list = field(default_factory=list)
    external_images: list = field(default_factory=list)
    counts: dict = field(default_factory=dict)
    tags: int = 0
    nested_tags: int = 0
    missing: list = field(default_factory=list)
    deferred: list = field(default_factory=list)
    resolved: int = 0
    notes: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.missing

    def as_dict(self) -> dict:
        return {
            "movie": self.movie, "tags": self.tags,
            "nested_tags": self.nested_tags, "counts": self.counts,
            "bitmaps": self.bitmaps, "fonts": self.fonts,
            "sounds": self.sounds, "imports": self.imports,
            "external_images": self.external_images,
            "resolved": self.resolved, "missing": self.missing,
            "deferred": self.deferred, "notes": self.notes, "ok": self.ok,
        }


def collect_assets(movie_bytes: bytes, movie_path: str = MOVIE) -> AssetReport:
    body = swf_body(movie_bytes)
    header = frame_header(body)
    report = AssetReport()
    report.movie = {
        "path": movie_path,
        "bytes": len(movie_bytes),
        "stored_bytes": len(movie_bytes),
        "signature": movie_bytes[:3].decode("latin-1"),
        "version": movie_bytes[3],
        "header_bytes": len(body) + 8,
        "sha256": hashlib.sha256(movie_bytes).hexdigest(),
        **header,
    }
    for tag in iter_tags(body, header["tag_offset"]):
        report.tags += 1
        if tag.depth:
            report.nested_tags += 1
            continue  # assets are top-level; imported ids only there
        try:
            if tag.code in BITMAP_TAGS:
                report.bitmaps.append({**parse_bitmap(tag), "inline": True})
            elif tag.code in FONT_TAGS:
                report.fonts.append({**parse_font(tag), "inline": True})
            elif tag.code in SOUND_TAGS:
                report.sounds.append({**parse_sound(tag), "inline": True})
            elif tag.code in IMPORT_TAGS:
                report.imports.append(parse_import_assets(tag))
            elif tag.code in EXTERNAL_IMAGE_TAGS:
                report.external_images.append(parse_external_image(tag))
        except SwfError as exc:
            raise SwfError("%s tag at body %d: %s"
                           % (tag.name, tag.offset, exc)) from None
    report.counts = {
        "bitmaps": len(report.bitmaps), "fonts": len(report.fonts),
        "sounds": len(report.sounds), "imports": len(report.imports),
        "external_images": len(report.external_images),
    }
    return report


def _symbol_face(fontconfig: dict, name: str) -> dict | None:
    for entry in fontconfig["maps"]:
        if _name_equal(entry["alias"], name):
            return entry
    return None


def _match_face(fonts: list, face: str, bold: bool,
                italic: bool) -> tuple[dict | None, str | None]:
    hits = [f for f in fonts
            if _name_equal(f.get("name", ""), face)
            and bool(f.get("bold")) == bold
            and bool(f.get("italic")) == italic]
    if not hits:
        return None, "face '%s' %s%s is absent" % (
            face, "Bold" if bold else "Normal", " Italic" if italic else "")
    if len(hits) > 1:
        return None, "face '%s' %s%s is ambiguous (%d candidates)" % (
            face, "Bold" if bold else "Normal", " Italic" if italic else "",
            len(hits))
    return hits[0], None


def close_assets(report: AssetReport, overlay: DataOverlay) -> AssetReport:
    """Resolve every external reference through ``overlay``."""
    movie_dir = report.movie["path"].rsplit("/", 1)[0]

    # Embedded definitions have no external dependency.
    for item in report.bitmaps + report.fonts:
        report.resolved += 1
    for sound in report.sounds:
        report.deferred.append({
            "id": sound["id"], "kind": sound["kind"], "bytes": sound["bytes"],
            "reason": "embedded audio; not required for menu drawing"})
        report.resolved += 1

    fontconfig = {"fontlibs": [], "maps": []}
    config_asset = overlay.resolve(FONTCONFIG)
    if config_asset is not None:
        archive = next((a for name, a in overlay.archives
                        if name == config_asset.source), None)
        if config_asset.source == "loose":
            text = (overlay.data_dir / FONTCONFIG.replace("/", "\\")
                    ).read_bytes().decode("latin-1")
        else:
            text = archive.read_file(FONTCONFIG).decode("latin-1")
        fontconfig = parse_fontconfig(text)

    candidate_cache: dict[str, list] = {}
    for record in report.imports:
        url = record["url"]
        record["path"] = "%s/%s" % (movie_dir, url) if movie_dir else url
        direct = overlay.resolve(record["path"])
        record["symbols_resolved"] = []
        record["missing"] = []
        if direct is not None:
            record["external"] = direct.as_dict()
            record["resolution"] = "external movie in overlay"
            for sym in record["symbols"]:
                record["symbols_resolved"].append({**sym, "how": "external"})
            report.resolved += 1 + len(record["symbols"])
            continue
        # Not a file: gfxfontlib.swf is the virtual GFx font library.
        for sym in record["symbols"]:
            entry = _symbol_face(fontconfig, sym["name"])
            resolved = {"id": sym["id"], "name": sym["name"], "how": None}
            if entry is None:
                reason = "no fontconfig map for '%s'" % sym["name"]
                record["missing"].append({"symbol": sym["name"],
                                          "reason": reason})
                report.missing.append({
                    "kind": "import", "url": url, "symbol": sym["name"],
                    "reason": reason})
                record["symbols_resolved"].append({**resolved,
                                                   "reason": reason})
                continue
            face, bold, italic = entry["face"], entry["bold"], entry["italic"]
            found = None
            searched = []
            why_best = None
            for lib in fontconfig["fontlibs"]:
                asset = overlay.resolve(lib)
                if asset is None:
                    searched.append("%s (missing)" % lib)
                    continue
                searched.append("%s (%s, %d B)"
                                % (lib, asset.source, asset.size))
                if lib not in candidate_cache:
                    data = _read_asset(overlay, asset)
                    try:
                        candidate_cache[lib] = collect_assets(
                            data, lib.replace("\\", "/")).fonts
                    except SwfError as exc:
                        candidate_cache[lib] = []
                        report.notes.append("%s: %s" % (lib, exc))
                match, why = _match_face(candidate_cache[lib], face, bold,
                                         italic)
                if match:
                    found = (lib, asset, match)
                    break
                if why and "ambiguous" in why:
                    why_best = why
            if found is None:
                reason = why_best or (
                    "no fontlib defines face '%s' %s%s"
                    % (face, "Bold" if bold else "Normal",
                       " Italic" if italic else ""))
                record["missing"].append({"symbol": sym["name"],
                                          "reason": reason,
                                          "searched": searched})
                report.missing.append({
                    "kind": "import", "url": url, "symbol": sym["name"],
                    "reason": reason, "searched": searched})
                record["symbols_resolved"].append({
                    **resolved, "alias": entry["alias"], "face": face,
                    "reason": reason})
                continue
            lib, asset, match = found
            record["symbols_resolved"].append({
                **resolved, "how": "fontconfig", "alias": entry["alias"],
                "face": face, "style": entry["style"] or "Normal",
                "fontlib": lib, "source": asset.source,
                "fontlib_bytes": asset.size, "font_id": match["id"],
                "glyphs": match["glyphs"], "font_kind": match["kind"]})
            report.resolved += 1
        if record["missing"]:
            record["resolution"] = "unresolved"
        else:
            record["resolution"] = ("virtual font library via fontconfig"
                                    if fontconfig["maps"]
                                    else "virtual font library (no fontconfig)")

    for image in report.external_images:
        path = image["file"].replace("\\", "/")
        if not path.lower().startswith("interface/"):
            path = "%s/%s" % (movie_dir, path) if movie_dir else path
        asset = overlay.resolve(path)
        if asset is None:
            report.missing.append({
                "kind": "external-image", "id": image["id"], "path": path,
                "reason": "not present in the Data overlay"})
            image["resolution"] = "missing"
        else:
            image["resolution"] = asset.as_dict()
            image["path"] = path
            report.resolved += 1

    if not report.imports and not report.external_images:
        report.notes.append("no external asset reference in the movie")
    return report


def _read_asset(overlay: DataOverlay, asset: ResolvedAsset) -> bytes:
    if asset.source == "loose":
        return (overlay.data_dir / asset.path.replace("\\", "/")).read_bytes()
    archive = next(a for name, a in overlay.archives
                   if name == asset.source)
    return archive.read_file(asset.path)


# ------------------------------------------------------------------- print ----
def format_report(report: AssetReport, overlay: DataOverlay | None) -> str:
    movie = report.movie
    lines = ["StartMenu.swf asset closure (read-only)"]
    source = movie.get("archive") or "?"
    lines.append("  movie: %s from %s (%d B, %s, SWF %d)"
                 % (movie["path"], source, movie["bytes"],
                    movie["signature"], movie["version"]))
    lines.append("  tags: %d (%d nested), bitmaps=%d fonts=%d sounds=%d "
                 "imports=%d external_images=%d"
                 % (report.tags, report.nested_tags,
                    report.counts["bitmaps"], report.counts["fonts"],
                    report.counts["sounds"], report.counts["imports"],
                    report.counts["external_images"]))
    for bmp in report.bitmaps:
        lines.append("  bitmap: id %d %s %dx%d, %d B (inline)"
                     % (bmp["id"], bmp["kind"], bmp["width"], bmp["height"],
                        bmp["bytes"]))
    for font in report.fonts:
        lines.append("  font: id %d %s \"%s\" %d glyphs%s, %d B (inline)"
                     % (font["id"], font["kind"], font["name"],
                        font["glyphs"],
                        " Bold" if font["bold"] else
                        (" Italic" if font["italic"] else ""),
                        font["bytes"]))
    for sound in report.sounds:
        lines.append("  sound: id %d %d Hz %s %.3f s, %d B (inline, deferred)"
                     % (sound["id"], sound["rate"],
                        "stereo" if sound["stereo"] else "mono",
                        sound["seconds"], sound["bytes"]))
    for record in report.imports:
        lines.append("  import: %s %s [%s]"
                     % (record["kind"], record["path"],
                        record.get("resolution", "?")))
        for sym in record["symbols_resolved"]:
            if sym.get("how") == "fontconfig":
                lines.append(
                    "    id %d %s -> \"%s\" %s -> %s %s DefineFont3 id %d, "
                    "%d glyphs (%d B)"
                    % (sym["id"], sym["name"], sym["face"], sym["style"],
                       sym["source"], sym["fontlib"], sym["font_id"],
                       sym["glyphs"], sym["fontlib_bytes"]))
            elif sym.get("how") == "external":
                lines.append("    id %d %s -> external movie"
                             % (sym["id"], sym["name"]))
            else:
                lines.append("    id %d %s -> MISSING: %s"
                             % (sym["id"], sym["name"], sym.get("reason")))
    for image in report.external_images:
        lines.append("  external image: id %d %s %dx%d"
                     % (image["id"], image["file"], image["width"],
                        image["height"]))
    if report.deferred:
        lines.append("  deferred: %s"
                     % ", ".join("id %d %s" % (d["id"], d["kind"])
                                 for d in report.deferred))
    for note in report.notes:
        lines.append("  note: %s" % note)
    lines.append("  resolved=%d missing=%d" % (report.resolved,
                                               len(report.missing)))
    for miss in report.missing:
        lines.append("  MISSING: %s" % json.dumps(miss, ensure_ascii=False))
    lines.append("  status: %s" % ("CLOSED" if report.ok else "OPEN"))
    return "\n".join(lines)


# -------------------------------------------------------------------- main ----
def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_movie(data_dir: Path, archive_name: str | None,
               movie: str) -> tuple[bytes, str, dict]:
    """Read the movie only (read-only) from one archive or the overlay."""
    archive_name = archive_name or DEFAULT_ARCHIVE
    path = data_dir / archive_name
    if not path.exists():
        raise SystemExit("archive not found: %s" % path)
    archive = bsa.open_archive(str(path))
    try:
        entry = archive.find(movie)
        if entry is None:
            raise SystemExit("%s does not contain %s" % (path, movie))
        data = archive.read_file(movie)
        info = {"archive": archive_name, "archive_path": str(path),
                "archive_bytes": path.stat().st_size,
                "archive_sha256": _sha256_file(path),
                "stored": entry.size, "entry": entry.path}
    finally:
        archive.close()
    return data, movie.replace("\\", "/"), info


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", default=str(DEFAULT_DATA),
                        help="Skyrim Data directory (default: %(default)s)")
    parser.add_argument("--bsa", default=DEFAULT_ARCHIVE,
                        help="archive holding the movie (default: %(default)s)")
    parser.add_argument("--movie", default=MOVIE,
                        help="virtual movie path (default: %(default)s)")
    parser.add_argument("--json", action="store_true",
                        help="print the full report as JSON")
    args = parser.parse_args(argv)

    data_dir = Path(args.data)
    movie_bytes, movie_path, info = load_movie(data_dir, args.bsa,
                                               args.movie)
    report = collect_assets(movie_bytes, movie_path)
    report.movie["archive"] = info["archive"]
    report.movie["archive_bytes"] = info["archive_bytes"]
    report.movie["archive_sha256"] = info["archive_sha256"]
    report.movie["stored_bytes"] = info["stored"]
    with DataOverlay(data_dir) as overlay:
        close_assets(report, overlay)
        if overlay.errors:
            report.notes.extend(overlay.errors)
        if args.json:
            print(json.dumps(report.as_dict(), indent=2))
        else:
            print(format_report(report, overlay))
    return 0 if report.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
