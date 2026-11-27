"""Write and validate the device ``stream_index.bin`` beside a cooked deploy.

The index is the only file the ARM11 reader needs to turn a canonical virtual
path into a physical ``(archive, offset, length)`` range.  It is deliberately
a flat little-endian binary: no JSON, no zlib, no string hashing tables on the
device, and no address derived from an archive name, directory order or a
converter estimate.  Every resource row is built by reopening the archive
written by ``write_bsa_stream()`` and reading the real ``BsaFileEntry``.

Layout (schema 1)
-----------------

Header, 80 bytes, little-endian::

    off  size  field
      0     4  magic = b"SRNX"
      4     2  schema version (1)
      6     2  header size (80)
      8     2  archive entry size (56)
     10     2  resource entry size (104)
     12     2  cell entry size (64)
     14     2  flags (bit 0 = little-endian tables, all others 0)
     16     4  archive count
     20     4  resource count
     24     4  cell count (0 until phase 2)
     28     4  reserved (0)
     32     8  generation (content digest, see ``generation_digest``)
     40     8  archive table offset (80)
     48     8  resource table offset
     56     8  path pool offset
     64     8  cell table offset (8-aligned)
     72     8  total size including the trailing CRC-32
     80   ...  tables
    last     4  CRC-32 of every preceding byte

Archive entry, 56 bytes::

      0  4  name offset into the path pool
      4  4  name length in bytes
      8  8  file size in bytes
     16 32  SHA-256 of the archive file
     48  4  priority (index in the manifest archive list; 0 = highest)
     52  4  number of resource rows that point at this archive

Resource entry, 104 bytes, sorted by normalized path::

      0  4  resource id (equal to the row index)
      4  4  path offset into the path pool
      8  4  path length in bytes
     12  4  archive index
     16  8  physical payload offset (after the embedded-name/compressed prefix)
     24  4  payload length (the range this device may read; for a compressed
             archive this is the stored deflate length and ``bsa_read_range``
             refuses it)
     28  4  stored size from the directory record
     32  4  stored offset from the directory record
     36  4  flags (bit 0 = compressed, bit 1 = embedded name)
     40 32  SHA-256 of the stored payload (what ``bsa_read_range`` returns;
             for a compressed row the digest is of the unpacked payload,
             because that is what ``read_file``/``verify_index`` check)
     72 32  SHA-256 of the original winning asset before cooking
    104 end

The path pool stores lowercase canonical paths (``\\`` separators) and is
padded with zeroes to 8 bytes.  The cell table is empty in schema 1; its entry
format is reserved as ``key[24] + neighbour_count u32 + flags u32 + 32 bytes of
class ids`` (64 bytes) so the table can grow without a schema bump.
"""

from __future__ import annotations

import hashlib
import os
import struct
import zlib
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence

from skyrim import bsa

MAGIC = b"SRNX"
SCHEMA_VERSION = 1
HEADER_SIZE = 80
ARCHIVE_ENTRY_SIZE = 56
RESOURCE_ENTRY_SIZE = 104
CELL_ENTRY_SIZE = 64
FLAG_LITTLE_ENDIAN = 0x0001

_HEADER = struct.Struct("<4s6H4I6Q")
_ARCHIVE = struct.Struct("<IIQ32sII")
_RESOURCE = struct.Struct("<IIIIQIIII32s32s")

# Keep the documented sizes honest at import time.
assert _HEADER.size == HEADER_SIZE
assert _ARCHIVE.size == ARCHIVE_ENTRY_SIZE
assert _RESOURCE.size == RESOURCE_ENTRY_SIZE


class StreamIndexError(Exception):
    pass


def normalize_path(path: str) -> str:
    """Canonical device form: lowercase, backslash separators, no edges."""
    if not isinstance(path, str):
        raise StreamIndexError("path must be a string")
    text = path.replace("/", "\\").strip("\\").lower()
    if not text:
        raise StreamIndexError("empty archive path")
    for part in text.split("\\"):
        if part in ("", ".", ".."):
            raise StreamIndexError("bad archive path %r" % path)
    raw = text.encode("cp1252")
    if len(raw) >= 512:
        raise StreamIndexError("archive path is too long: %r" % path)
    return text


def _file_sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


@dataclass
class IndexArchive:
    name: str
    bytes: int
    sha256: str
    priority: int
    files: int = 0
    # Derived from the archive header / resource flags, never serialised:
    # schema 1 has no spare bit in the 56-byte archive record, and the C
    # parser rejects unknown header flags.  Per-entry flag bit 0 is the
    # authoritative signal used by ``bsa_read_range``.
    compressed: bool = False


@dataclass
class IndexResource:
    id: int
    path: str
    archive: int
    physical_offset: int
    payload_length: int
    stored_size: int
    entry_offset: int
    flags: int
    sha256: str
    source_sha256: str


@dataclass
class StreamIndex:
    archives: List[IndexArchive] = field(default_factory=list)
    resources: List[IndexResource] = field(default_factory=list)
    generation: int = 0
    cell_count: int = 0

    # -- lookups -----------------------------------------------------------

    def find_path(self, path: str) -> Optional[IndexResource]:
        want = normalize_path(path)
        lo, hi = 0, len(self.resources)
        while lo < hi:
            mid = (lo + hi) // 2
            have = self.resources[mid].path
            if have < want:
                lo = mid + 1
            elif have > want:
                hi = mid
            else:
                return self.resources[mid]
        return None

    def find_id(self, resource_id: int) -> Optional[IndexResource]:
        if 0 <= resource_id < len(self.resources):
            return self.resources[resource_id]
        return None

    # -- serialization -----------------------------------------------------

    def to_bytes(self) -> bytes:
        if not self.archives:
            raise StreamIndexError("refusing to write an index with no archives")
        resource_table = bytearray()
        pool = bytearray()
        last_path = None
        for row in self.resources:
            path = row.path.encode("cp1252")
            if last_path is not None and row.path <= last_path:
                raise StreamIndexError(
                    "resource paths are not strictly sorted at id %d" % row.id)
            last_path = row.path
            resource_table += _RESOURCE.pack(
                row.id, len(pool), len(path), row.archive,
                row.physical_offset, row.payload_length, row.stored_size,
                row.entry_offset, row.flags, bytes.fromhex(row.sha256),
                bytes.fromhex(row.source_sha256))
            pool += path
        while len(pool) % 8:
            pool.append(0)
        archive_names = bytearray()
        archive_offsets = []
        # Names live in the same pool after the resource paths.
        base = len(pool)
        for row in self.archives:
            raw = row.name.encode("cp1252")
            archive_offsets.append((base + len(archive_names), len(raw)))
            archive_names += raw
        pool += archive_names
        while len(pool) % 8:
            pool.append(0)

        archive_table = bytearray()
        for row, (off, length) in zip(self.archives, archive_offsets):
            archive_table += _ARCHIVE.pack(off, length, row.bytes,
                                           bytes.fromhex(row.sha256),
                                           row.priority, row.files)

        archive_off = HEADER_SIZE
        resource_off = archive_off + len(archive_table)
        pool_off = resource_off + len(resource_table)
        cell_off = pool_off + len(pool)
        total = cell_off + CELL_ENTRY_SIZE * self.cell_count + 4
        header = _HEADER.pack(
            MAGIC, SCHEMA_VERSION, HEADER_SIZE, ARCHIVE_ENTRY_SIZE,
            RESOURCE_ENTRY_SIZE, CELL_ENTRY_SIZE, FLAG_LITTLE_ENDIAN,
            len(self.archives), len(self.resources), self.cell_count, 0,
            self.generation, archive_off, resource_off, pool_off, cell_off,
            total)
        body = bytes(header) + bytes(archive_table) + bytes(resource_table) + \
            bytes(pool)
        body += b"\x00" * (CELL_ENTRY_SIZE * self.cell_count)
        crc = zlib.crc32(body) & 0xFFFFFFFF
        return body + struct.pack("<I", crc)

    @classmethod
    def from_bytes(cls, blob: bytes) -> "StreamIndex":
        if len(blob) < HEADER_SIZE + 4:
            raise StreamIndexError("index is shorter than its header")
        (magic, schema, header_size, archive_size, resource_size, cell_size,
         flags, archive_count, resource_count, cell_count, reserved,
         generation, archive_off, resource_off, pool_off, cell_off,
         total) = _HEADER.unpack_from(blob, 0)
        if magic != MAGIC:
            raise StreamIndexError("bad index magic %r" % magic)
        if schema != SCHEMA_VERSION:
            raise StreamIndexError("unsupported index schema %d" % schema)
        if (header_size, archive_size, resource_size, cell_size) != (
                HEADER_SIZE, ARCHIVE_ENTRY_SIZE, RESOURCE_ENTRY_SIZE,
                CELL_ENTRY_SIZE):
            raise StreamIndexError("index entry sizes do not match schema 1")
        if flags != FLAG_LITTLE_ENDIAN:
            raise StreamIndexError("unsupported index flags 0x%04x" % flags)
        if reserved != 0:
            raise StreamIndexError("reserved header field is not zero")
        if total != len(blob):
            raise StreamIndexError(
                "index declares %d bytes, file has %d" % (total, len(blob)))
        if archive_off != HEADER_SIZE or \
                resource_off != archive_off + archive_count * ARCHIVE_ENTRY_SIZE:
            raise StreamIndexError("index table offsets are inconsistent")
        if pool_off != resource_off + resource_count * RESOURCE_ENTRY_SIZE:
            raise StreamIndexError("index path pool offset is inconsistent")
        if cell_off < pool_off or cell_off % 8:
            raise StreamIndexError("index cell table offset is misaligned")
        if cell_off + cell_count * CELL_ENTRY_SIZE + 4 != total:
            raise StreamIndexError("index cell table size is inconsistent")
        stored_crc = struct.unpack_from("<I", blob, total - 4)[0]
        if zlib.crc32(blob[:-4]) & 0xFFFFFFFF != stored_crc:
            raise StreamIndexError("index CRC-32 mismatch")
        pool = blob[pool_off:cell_off]

        index = cls(generation=generation, cell_count=cell_count)
        for i in range(archive_count):
            (name_off, name_len, size, digest, priority,
             files) = _ARCHIVE.unpack_from(blob, archive_off + i * ARCHIVE_ENTRY_SIZE)
            if not name_len or name_off + name_len > len(pool):
                raise StreamIndexError("archive %d name is outside the pool" % i)
            raw = pool[name_off:name_off + name_len]
            try:
                name = raw.decode("cp1252")
            except UnicodeDecodeError as exc:
                raise StreamIndexError("archive %d name is not cp1252: %s"
                                       % (i, exc))
            index.archives.append(IndexArchive(
                name=name, bytes=size, sha256=digest.hex(),
                priority=priority, files=files))

        last = None
        for i in range(resource_count):
            (rid, path_off, path_len, archive_index, physical, payload_len,
             stored_size, entry_offset, rflags, digest,
             source_digest) = _RESOURCE.unpack_from(
                 blob, resource_off + i * RESOURCE_ENTRY_SIZE)
            if rid != i:
                raise StreamIndexError("resource %d has id %d" % (i, rid))
            if not path_len or path_off + path_len > len(pool):
                raise StreamIndexError("resource %d path is outside the pool" % i)
            if archive_index >= archive_count:
                raise StreamIndexError("resource %d archive index is invalid" % i)
            try:
                path = pool[path_off:path_off + path_len].decode("cp1252")
            except UnicodeDecodeError as exc:
                raise StreamIndexError("resource %d path is not cp1252: %s"
                                       % (i, exc))
            if normalize_path(path) != path:
                raise StreamIndexError("resource %d path is not canonical" % i)
            if last is not None and path <= last:
                raise StreamIndexError("resource paths are not strictly sorted")
            last = path
            archive = index.archives[archive_index]
            if physical + payload_len > archive.bytes:
                raise StreamIndexError(
                    "resource %d range %d+%d is outside archive %s"
                    % (i, physical, payload_len, archive.name))
            if physical > 0xFFFFFFFF or physical + payload_len > 0xFFFFFFFF:
                raise StreamIndexError(
                    "resource %d does not fit the 32-bit BSA offset field" % i)
            if entry_offset > 0xFFFFFFFF:
                raise StreamIndexError("resource %d entry offset is not 32-bit" % i)
            index.resources.append(IndexResource(
                id=rid, path=path, archive=archive_index,
                physical_offset=physical, payload_length=payload_len,
                stored_size=stored_size, entry_offset=entry_offset,
                flags=rflags, sha256=digest.hex(),
                source_sha256=source_digest.hex()))
        counts = [0] * archive_count
        for row in index.resources:
            counts[row.archive] += 1
            if row.flags & 0x1:
                # Schema 1 stores the compressed bit per resource, not per
                # archive (the 56-byte archive record is full and the C parser
                # rejects new header flags).
                index.archives[row.archive].compressed = True
        for i, row in enumerate(index.archives):
            if row.priority != i:
                raise StreamIndexError("archive %d priority is not its index" % i)
            if row.files != counts[i]:
                raise StreamIndexError(
                    "archive %s declares %d files, %d rows point at it"
                    % (row.name, row.files, counts[i]))
        return index


def read_index(path: str) -> StreamIndex:
    with open(path, "rb") as handle:
        return StreamIndex.from_bytes(handle.read())


# ---------------------------------------------------------------------------
# Building from a cooked manifest


def _payload_prefix(entry: bsa.BsaFileEntry, archive: bsa.BsaArchive) -> int:
    """Return the prefix length removed by ``entry_prefix()`` in the C reader.

    This reads the real bytes at ``entry.offset`` instead of deriving the
    layout from the archive name or the writer's settings.
    """
    prefix = 0
    handle = archive._file  # tool-internal: the archive keeps one open handle
    if archive.header.embedded_names:
        handle.seek(entry.offset)
        first = handle.read(1)
        if len(first) != 1:
            raise StreamIndexError("truncated embedded name at %d" % entry.offset)
        name_len = first[0]
        if name_len == 0 or 1 + name_len > entry.size:
            raise StreamIndexError(
                "bad embedded name length %d at %d" % (name_len, entry.offset))
        handle.seek(entry.offset + 1)
        raw = handle.read(name_len)
        visible = raw[:-1] if raw.endswith(b"\x00") else raw
        try:
            embedded = visible.decode("cp1252").replace("/", "\\").strip("\\").lower()
        except UnicodeDecodeError:
            embedded = ""
        if embedded != normalize_path(entry.folder + "\\" + entry.name
                                      if entry.folder else entry.name):
            raise StreamIndexError(
                "embedded name at %d does not match %s" % (entry.offset, entry.path))
        prefix += 1 + name_len
    if archive.header.compressed:
        prefix += 4
    return prefix


def _generation(archives: Sequence[IndexArchive],
                resources: Sequence[IndexResource]) -> int:
    digest = hashlib.sha256()
    digest.update(b"SRNX-generation-v1\0")
    for row in archives:
        digest.update(row.name.encode("utf-8") + b"\0")
        digest.update(struct.pack("<Q", row.bytes))
        digest.update(bytes.fromhex(row.sha256))
    for row in resources:
        digest.update(struct.pack("<I", row.id))
        digest.update(row.path.encode("cp1252") + b"\0")
        digest.update(struct.pack("<IQIII", row.archive, row.physical_offset,
                                  row.payload_length, row.stored_size,
                                  row.flags))
        digest.update(bytes.fromhex(row.sha256))
        digest.update(bytes.fromhex(row.source_sha256))
    return int.from_bytes(digest.digest()[:8], "little")


def build_index(archive_rows: Sequence[dict], file_rows: Sequence[dict],
                archives_dir: str) -> StreamIndex:
    """Turn the manifest's archive/file rows into a verified index.

    ``archive_rows`` order is the priority order.  Every row is checked
    against the archive actually on disk; a size or digest mismatch aborts
    the whole build rather than publishing a mixed generation.
    """
    by_name: Dict[str, dict] = {}
    for row in archive_rows:
        name = row["name"]
        if os.path.basename(name) != name:
            raise StreamIndexError("archive name is not a bare file name: %r" % name)
        if name.lower() in {k.lower() for k in by_name}:
            raise StreamIndexError("duplicate archive name %r" % name)
        by_name[name.lower()] = row

    archives: List[IndexArchive] = []
    for priority, row in enumerate(archive_rows):
        path = os.path.join(archives_dir, row["name"])
        real_size = os.path.getsize(path)
        if real_size != row["bytes"]:
            raise StreamIndexError(
                "archive %s is %d bytes, manifest says %d"
                % (row["name"], real_size, row["bytes"]))
        digest = _file_sha256(path)
        if digest != row["sha256"].lower():
            raise StreamIndexError(
                "archive %s SHA-256 %s does not match the manifest %s"
                % (row["name"], digest, row["sha256"]))
        archives.append(IndexArchive(name=row["name"], bytes=real_size,
                                     sha256=digest, priority=priority))

    by_archive: Dict[str, List[dict]] = {row["name"]: [] for row in archive_rows}
    for row in file_rows:
        name = row.get("archive")
        if not name:
            raise StreamIndexError(
                "file row %r has no archive assignment" % row.get("path"))
        if name not in by_archive:
            raise StreamIndexError(
                "file row %r names unknown archive %r" % (row.get("path"), name))
        by_archive[name].append(row)

    resources: List[IndexResource] = []
    seen = set()
    for archive_index, row in enumerate(archive_rows):
        name = row["name"]
        path = os.path.join(archives_dir, name)
        rows = sorted(by_archive[name], key=lambda item: normalize_path(item["path"]))
        with bsa.BsaArchive(str(path)) as archive:
            listed = {entry.path.replace("/", "\\").lower(): entry
                      for entry in archive.iter_files()}
            if len(listed) != len(rows):
                raise StreamIndexError(
                    "archive %s has %d files, manifest lists %d"
                    % (name, len(listed), len(rows)))
            if "compressed" in row and \
                    bool(row["compressed"]) != archive.header.compressed:
                raise StreamIndexError(
                    "archive %s is %scompressed, manifest says %scompressed"
                    % (name, "" if archive.header.compressed else "not ",
                       "" if row["compressed"] else "not "))
            for item in rows:
                key = normalize_path(item["path"])
                if key in seen:
                    raise StreamIndexError("duplicate resource path %r" % key)
                seen.add(key)
                entry = listed.get(key)
                if entry is None:
                    raise StreamIndexError(
                        "archive %s has no path %r" % (name, key))
                prefix = _payload_prefix(entry, archive)
                physical = entry.offset + prefix
                payload_len = entry.size - prefix
                payload = archive.read_file(entry.path)
                if not archive.header.compressed:
                    if payload_len != item["bytes"]:
                        raise StreamIndexError(
                            "%s: manifest says %d bytes, archive stores %d"
                            % (key, item["bytes"], payload_len))
                elif len(payload) != item["bytes"]:
                    # Compressed rows publish the stored deflate range in
                    # ``payload_length``; the manifest size stays unpacked.
                    raise StreamIndexError(
                        "%s: manifest says %d bytes, archive unpacks to %d"
                        % (key, item["bytes"], len(payload)))
                digest = hashlib.sha256(payload).hexdigest()
                if digest != item["sha256"].lower():
                    raise StreamIndexError(
                        "%s: payload SHA-256 does not match the manifest" % key)
                source = item.get("source_sha256") or digest
                flags = (0x1 if archive.header.compressed else 0) | \
                        (0x2 if archive.header.embedded_names else 0)
                resources.append(IndexResource(
                    id=0, path=key, archive=archive_index,
                    physical_offset=physical, payload_length=payload_len,
                    stored_size=entry.size, entry_offset=entry.offset,
                    flags=flags, sha256=digest, source_sha256=source))
            archives[archive_index].compressed = archive.header.compressed
        archives[archive_index].files = len(rows)

    resources.sort(key=lambda row: row.path)
    for i, row in enumerate(resources):
        row.id = i
    index = StreamIndex(archives=archives, resources=resources)
    index.generation = _generation(index.archives, index.resources)
    return index


def verify_index(index: StreamIndex, archives_dir: str) -> List[str]:
    """Reopen every archive and re-check every row.  Returns error strings."""
    errors: List[str] = []
    for archive_row in index.archives:
        path = os.path.join(archives_dir, archive_row.name)
        try:
            real_size = os.path.getsize(path)
        except OSError as exc:
            errors.append("%s: %s" % (archive_row.name, exc))
            continue
        if real_size != archive_row.bytes:
            errors.append("%s: size %d != %d" % (archive_row.name, real_size,
                                                 archive_row.bytes))
        if _file_sha256(path) != archive_row.sha256:
            errors.append("%s: SHA-256 changed after the index was written"
                          % archive_row.name)
    rows_by_archive: Dict[int, set] = {i: set() for i in range(len(index.archives))}
    for row in index.resources:
        rows_by_archive[row.archive].add(row.path)
        if row.archive >= len(index.archives):
            errors.append("resource %d archive index is invalid" % row.id)
            continue
        archive_row = index.archives[row.archive]
        path = os.path.join(archives_dir, archive_row.name)
        try:
            with bsa.BsaArchive(str(path)) as archive:
                entry = archive.find(row.path)
                if entry is None:
                    errors.append("%s: %s is missing" % (archive_row.name,
                                                         row.path))
                    continue
                if entry.offset != row.entry_offset or \
                        entry.size != row.stored_size:
                    errors.append("%s: %s directory record changed"
                                  % (archive_row.name, row.path))
                    continue
                physical = row.entry_offset + _payload_prefix(entry, archive)
                if physical != row.physical_offset:
                    errors.append("%s: %s physical offset %d != %d"
                                  % (archive_row.name, row.path, physical,
                                     row.physical_offset))
                if physical + row.payload_length > archive_row.bytes:
                    errors.append("%s: %s range runs past EOF"
                                  % (archive_row.name, row.path))
                if physical + row.payload_length > 0xFFFFFFFF:
                    errors.append("%s: %s range needs a 33-bit offset"
                                  % (archive_row.name, row.path))
                payload = archive.read_file(entry.path)
                if archive.header.compressed:
                    # ``payload_length`` is the stored deflate range; the
                    # digest is of the unpacked payload.
                    prefix = _payload_prefix(entry, archive)
                    if row.payload_length != entry.size - prefix:
                        errors.append(
                            "%s: %s compressed range %d != %d"
                            % (archive_row.name, row.path, row.payload_length,
                               entry.size - prefix))
                    if hashlib.sha256(payload).hexdigest() != row.sha256:
                        errors.append("%s: %s payload SHA-256 changed"
                                      % (archive_row.name, row.path))
                elif len(payload) != row.payload_length:
                    errors.append("%s: %s payload length %d != %d"
                                  % (archive_row.name, row.path, len(payload),
                                     row.payload_length))
                elif hashlib.sha256(payload).hexdigest() != row.sha256:
                    errors.append("%s: %s payload SHA-256 changed"
                                  % (archive_row.name, row.path))
        except (OSError, bsa.BsaError) as exc:
            errors.append("%s: %s" % (archive_row.name, exc))
    for i, archive_row in enumerate(index.archives):
        path = os.path.join(archives_dir, archive_row.name)
        try:
            with bsa.BsaArchive(str(path)) as archive:
                listed = {entry.path.replace("/", "\\").lower(): entry
                          for entry in archive.iter_files()}
        except (OSError, bsa.BsaError) as exc:
            errors.append("%s: %s" % (archive_row.name, exc))
            continue
        missing = set(listed) - rows_by_archive.get(i, set())
        extra = rows_by_archive.get(i, set()) - set(listed)
        if missing:
            errors.append("%s: %d archive files are not in the index (e.g. %s)"
                          % (archive_row.name, len(missing), sorted(missing)[0]))
        if extra:
            errors.append("%s: %d index rows have no archive file (e.g. %s)"
                          % (archive_row.name, len(extra), sorted(extra)[0]))
    return errors


def write_index(archive_rows: Sequence[dict], file_rows: Sequence[dict],
                archives_dir: str, out_path: str) -> dict:
    """Build, write, reopen and verify ``stream_index.bin``."""
    index = build_index(archive_rows, file_rows, archives_dir)
    blob = index.to_bytes()
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as handle:
        handle.write(blob)
    # Re-read through the device-facing parser, then reopen every archive.
    reopened = read_index(out_path)
    if reopened.to_bytes() != blob:
        raise StreamIndexError("index did not round-trip through the reader")
    errors = verify_index(reopened, archives_dir)
    if errors:
        raise StreamIndexError("; ".join(errors[:4]))
    return {
        "path": os.path.abspath(out_path),
        "bytes": len(blob),
        "schema": SCHEMA_VERSION,
        "generation": index.generation,
        "archives": len(index.archives),
        "resources": len(index.resources),
        "cells": index.cell_count,
    }


def write_from_manifest(manifest: dict, out_path: str,
                        archives_dir: Optional[str] = None) -> dict:
    if not manifest.get("cooked"):
        raise StreamIndexError("stream index needs a cooked manifest")
    if archives_dir is None:
        archives_dir = os.path.join(manifest["out_root"], "Data")
    return write_index(manifest.get("archives", []), manifest.get("files", []),
                       archives_dir, out_path)
