"""Havok HKX classification for Skyrim 2011 (Havok 2010.2.0-r1).

This module only detects and classifies HKX files.  It is **not** a Havok
reader: no object graph, class descriptors, fixups or animation data are
decoded.

Skyrim 2011 ships *packed binary* HKX files.  The first eight bytes are the
packfile tags (little endian)::

    tag0 = 0x57E0E057      bytes: 57 E0 E0 57
    tag1 = 0x10C0C010      bytes: 10 C0 C0 10

followed by the 64-byte packfile header (file version 11, layout rules 3 for
Skyrim).  Classic XML HKX (``<?xml ...`` / ``<hkpackfile>``) and uncompressed
tag0-only files are recognised for completeness.

A full reader would need, in this order:
  * the packfile header (parsed here) and the section table that follows it
    (u32 section tag, u32 absolute offset, u32 end offset, u32 internal offset),
  * the type/class name tables and the per-section object tables,
  * virtual fixups (object references are section-relative offsets, sometimes
    with the low bit marking an array),
  * type descriptors with member offsets/sizes, including arrays of
    references, string pointers and 16-byte alignment rules,
  * the Skyrim layout-rules id (3) and contents version string
    (``Havok-2010.2-r1``) to pick the class layouts.
None of that is implemented; callers get a classification plus header fields.
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass
from typing import Optional

TAG0_PACKFILE = 0x57E0E057
TAG1_PACKFILE = 0x10C0C010
PACKFILE_HEADER_SIZE = 64


class HkxError(Exception):
    """Malformed HKX header."""


@dataclass(frozen=True)
class HkxInfo:
    kind: str                  # "packed-binary", "xml", "unknown"
    size: int
    tag0: int = 0
    tag1: int = 0
    user_tag: int = 0
    file_version: int = 0
    layout_rules: int = 0
    num_sections: int = 0
    contents_section_index: int = 0
    contents_section_offset: int = 0
    contents_class_section_index: int = 0
    contents_class_section_offset: int = 0
    contents_version: str = ""
    flags: int = 0
    note: str = ""

    def as_dict(self) -> dict:
        return {
            "kind": self.kind,
            "size": self.size,
            "tag0": "0x%08x" % self.tag0 if self.tag0 else None,
            "tag1": "0x%08x" % self.tag1 if self.tag1 else None,
            "user_tag": self.user_tag,
            "file_version": self.file_version,
            "layout_rules": "0x%08x" % self.layout_rules,
            "num_sections": self.num_sections,
            "contents_section_index": self.contents_section_index,
            "contents_section_offset": self.contents_section_offset,
            "contents_class_section_index": self.contents_class_section_index,
            "contents_class_section_offset": self.contents_class_section_offset,
            "contents_version": self.contents_version,
            "flags": self.flags,
            "note": self.note,
        }


def classify(data: bytes) -> HkxInfo:
    size = len(data)
    if size >= 8:
        tag0, tag1 = struct.unpack_from("<II", data, 0)
    else:
        tag0 = tag1 = 0
    if tag0 == TAG0_PACKFILE and tag1 == TAG1_PACKFILE:
        if size < PACKFILE_HEADER_SIZE:
            raise HkxError("packed HKX is shorter than the 64-byte packfile header")
        (
            _t0,
            _t1,
            user_tag,
            file_version,
            layout_rules,
            num_sections,
            contents_index,
            contents_offset,
            class_index,
            class_offset,
        ) = struct.unpack_from("<10I", data, 0)
        version_raw = data[40:56]
        flags, = struct.unpack_from("<I", data, 56)
        try:
            version = version_raw.split(b"\x00", 1)[0].decode("ascii")
        except UnicodeDecodeError:
            version = version_raw.hex()
        if num_sections == 0 or num_sections > 256:
            raise HkxError("implausible Havok section count %d" % num_sections)
        return HkxInfo(
            kind="packed-binary",
            size=size,
            tag0=tag0,
            tag1=tag1,
            user_tag=user_tag,
            file_version=file_version,
            layout_rules=layout_rules,
            num_sections=num_sections,
            contents_section_index=contents_index,
            contents_section_offset=contents_offset,
            contents_class_section_index=class_index,
            contents_class_section_offset=class_offset,
            contents_version=version,
            flags=flags,
            note="packfile header parsed; object graph not decoded",
        )
    stripped = data[:64].lstrip()
    if stripped.startswith(b"<?xml") or b"<hkpackfile" in data[:4096]:
        return HkxInfo(kind="xml", size=size, note="Havok XML; not parsed")
    if tag0 == TAG0_PACKFILE:
        return HkxInfo(
            kind="unknown",
            size=size,
            tag0=tag0,
            tag1=tag1,
            note="tag0 without packfile tag1",
        )
    return HkxInfo(
        kind="unknown",
        size=size,
        tag0=tag0,
        tag1=tag1,
        note="first 8 bytes do not match a Havok tag (Skyrim uses packed binary)",
    )


def classify_file(path: str) -> HkxInfo:
    import dataclasses

    with open(path, "rb") as handle:
        info = classify(handle.read(4096))
    return dataclasses.replace(info, size=os.path.getsize(path))
