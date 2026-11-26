"""Minimal BSA v104/v105 writer for offline mod repacking.

Only the layout understood by ``tools/skyrim/bsa.py`` and
``source/bsa_reader.c`` is emitted: 36-byte header, folder records, folder
bstrings, file records, a NUL-terminated base-name block, then data.  The
64-bit TES4 hash fields are written as zero: this port's reader indexes names
case-insensitively and never consults the hashes, so a repacked archive works
here.  Do **not** ship these archives to the Creation Kit or to a mod manager
that validates TES4 hashes -- they are port input, not editor input.

Compression is zlib (archive flag 0x004) with the documented 4-byte unpacked
size prefix; ``embed_names`` (0x100) adds the length-prefixed full path in
front of each payload exactly like the retail archives.
"""

from __future__ import annotations

import os
import struct
import zlib
from dataclasses import dataclass
from typing import Iterable, List, Mapping, Sequence, Tuple, Union

MAGIC = b"BSA\x00"
HEADER_SIZE = 36
FLAG_DIRECTORY_NAMES = 0x001
FLAG_FILE_NAMES = 0x002
FLAG_COMPRESSED = 0x004
FLAG_EMBED_NAMES = 0x100


class BsaWriteError(Exception):
    pass


@dataclass
class _Item:
    folder: str
    name: str
    payload: bytes
    stored: bytes = b""
    offset: int = 0


def _normalize(path: str) -> Tuple[str, str]:
    path = path.replace("/", "\\").strip("\\")
    folder, _, name = path.rpartition("\\")
    if not name or name in (".", ".."):
        raise BsaWriteError("bad archive path %r" % path)
    for part in folder.split("\\"):
        if part in (".", ".."):
            raise BsaWriteError("bad archive folder in %r" % path)
    return folder, name


def _pack(files: Iterable[Tuple[str, bytes]], *, compressed: bool,
          embed_names: bool, level: int) -> Tuple[bytes, List[_Item]]:
    items: List[_Item] = []
    for path, payload in files:
        folder, name = _normalize(path)
        item = _Item(folder, name, bytes(payload))
        stored = bytearray()
        if embed_names:
            full = (folder + "\\" + name) if folder else name
            raw = full.encode("cp1252")
            if not raw or len(raw) > 255:
                raise BsaWriteError("embedded name %r is not encodable" % full)
            stored += bytes([len(raw)]) + raw
        if compressed:
            stored += struct.pack("<I", len(item.payload))
            stored += zlib.compress(item.payload, level)
        else:
            stored += item.payload
        item.stored = bytes(stored)
        items.append(item)
    items.sort(key=lambda i: (i.folder.lower(), i.name.lower()))
    return b"", items


def write_bsa(path: str, files: Union[Mapping[str, bytes], Sequence[Tuple[str, bytes]]],
              *, version: int = 104, compressed: bool = True,
              embed_names: bool = False, level: int = 9) -> dict:
    """Write one archive and return a small manifest of what was stored."""
    if version not in (104, 105):
        raise BsaWriteError("only v104/v105 output is supported (got %d)" % version)
    pairs = list(files.items()) if isinstance(files, Mapping) else list(files)
    if not pairs:
        raise BsaWriteError("refusing to write an empty archive")
    _, items = _pack(pairs, compressed=compressed, embed_names=embed_names,
                     level=level)

    folders: List[str] = []
    by_folder: List[List[_Item]] = []
    for item in items:
        if not folders or folders[-1].lower() != item.folder.lower():
            folders.append(item.folder)
            by_folder.append([])
        by_folder[-1].append(item)
    if len(folders) != len({f.lower() for f in folders}):
        raise BsaWriteError("folder names collide case-insensitively")

    flags = FLAG_DIRECTORY_NAMES | FLAG_FILE_NAMES
    if compressed:
        flags |= FLAG_COMPRESSED
    if embed_names:
        flags |= FLAG_EMBED_NAMES

    total_folder_names = sum(len(f.encode("cp1252")) + 1 for f in folders)
    total_file_names = sum(len(i.name.encode("cp1252")) + 1 for i in items)

    directory_size = HEADER_SIZE + 16 * len(folders)
    for folder, group in zip(folders, by_folder):
        directory_size += 1 + len(folder.encode("cp1252")) + 1
        directory_size += 16 * len(group)
    data_start = directory_size + total_file_names

    cursor = data_start
    for item in items:
        item.offset = cursor
        cursor += len(item.stored)

    out = bytearray()
    out += struct.pack("<4s8I", MAGIC, version, HEADER_SIZE, flags,
                       len(folders), len(items), total_folder_names,
                       total_file_names, 0)
    index = 0
    for folder, group in zip(folders, by_folder):
        out += struct.pack("<QII", 0, len(group), 0)
        index += len(group)
    index = 0
    for folder, group in zip(folders, by_folder):
        raw = folder.encode("cp1252") + b"\x00"
        out += bytes([len(raw)]) + raw
        for item in group:
            out += struct.pack("<QII", 0, len(item.stored), item.offset)
            index += 1
    for item in items:
        out += item.name.encode("cp1252") + b"\x00"
    for item in items:
        out += item.stored

    if len(out) != cursor:
        raise BsaWriteError("internal size mismatch (%d != %d)" % (len(out), cursor))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(out)
    return {
        "path": os.path.abspath(path),
        "version": version,
        "compressed": compressed,
        "embed_names": embed_names,
        "folders": len(folders),
        "files": len(items),
        "bytes": len(out),
        "data_start": data_start,
        "names_point_to": list(i.name for i in items),
    }
