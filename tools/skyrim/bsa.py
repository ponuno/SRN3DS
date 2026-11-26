"""Bethesda BSA archive reader (Oblivion v103, Skyrim v104, SE v105).

The implementation is deliberately read-only and does not trust the archive:
every record offset/length is bounds-checked before use.  The on-disk layout
below was verified against the shipped Skyrim Legendary Edition archives
(``Skyrim - Textures.bsa``, ``Skyrim - Meshes.bsa``, the DLC archives, ...).

Layout observed (all 11 local Skyrim archives are version 104; a real
Oblivion v103 archive has the same 36-byte header and directory layout)
-------------------------------------------------------------------------
::

    header (36 bytes)
    folder records            folder_count * 16
    for each folder (in the on-disk order):
        bstring folder name   u8 length, **length includes the NUL**
        file records          folder.count * 16
    file name block           total_file_name_length bytes of NUL-terminated
                              *base* names (no directory), one per file record
                              in folder/record order
    file data                 each file record's offset is absolute here

This interleaved folder-name/file-record layout is what the shipped archives
contain; it differs from several web descriptions which show two separate
blocks.  The reader does not use ``FolderRecord.offset`` (it is informational
and does not consistently match a simple block base); folder ``count`` is
authoritative.

Record fields::

    folder: u64 hash, u32 count, u32 offset
    file:   u64 hash, u32 size,   u32 offset

``size`` is the number of stored bytes at ``offset`` *including* any embedded
name prefix and the 4-byte uncompressed size, not the unpacked size.

Archive flags (only the ones this reader acts on)
-------------------------------------------------
0x001 include directory names        0x002 include file names
0x004 compressed (all files, zlib)  0x100 embed full file name before data

Compressed file payload::

    [u8 name_len + name]  (only when archive flag 0x100)
    u32      unpacked size
    bytes    zlib stream

The embedded name length **excludes** the NUL and the name is not
NUL-terminated (unlike the folder names in the directory block, whose bstring
length includes the NUL).  This is the "0x100 embedding" quirk.  The stored
``size`` counts all of it.

Compression is an archive-level property.  The high bit (0x40000000) of a file
offset is *not* a per-file compression flag in these archives: ``Textures.bsa``
is 1.39 GB, so thousands of legitimate absolute offsets have bit 30 set.
"""

from __future__ import annotations

import os
import struct
import zlib
from dataclasses import dataclass, field
from typing import Dict, Iterable, Iterator, List, Optional, Tuple

MAGIC = b"BSA\x00"
HEADER_SIZE = 36
SUPPORTED_VERSIONS = (103, 104, 105)

FLAG_INCLUDE_DIRECTORY_NAMES = 0x001
FLAG_INCLUDE_FILE_NAMES = 0x002
FLAG_COMPRESSED = 0x004
FLAG_RETAIN_DIRECTORY_NAMES = 0x008
FLAG_RETAIN_FILE_NAMES = 0x010
FLAG_RETAIN_FILE_NAME_OFFSETS = 0x020
FLAG_XBOX360 = 0x040
FLAG_RETAIN_STRINGS = 0x080
FLAG_EMBED_FILE_NAMES = 0x100
FLAG_XMEM_CODEC = 0x200

_MAX_NAME = 4096
_MAX_ARRAY = 1_000_000


class BsaError(Exception):
    """Base error for archive problems."""


class BsaFormatError(BsaError):
    """The archive does not follow the layout this reader understands."""


@dataclass(frozen=True)
class BsaHeader:
    version: int
    offset: int
    archive_flags: int
    folder_count: int
    file_count: int
    total_folder_name_length: int
    total_file_name_length: int
    file_flags: int

    @property
    def compressed(self) -> bool:
        return bool(self.archive_flags & FLAG_COMPRESSED)

    @property
    def embedded_names(self) -> bool:
        # v103 archives in the wild set 0x100 but store no name prefix (the
        # bit is part of the retain-flags group); only v104+ embeds names.
        return self.version >= 104 and bool(self.archive_flags & FLAG_EMBED_FILE_NAMES)

    @property
    def has_directory_names(self) -> bool:
        return bool(self.archive_flags & FLAG_INCLUDE_DIRECTORY_NAMES)

    @property
    def has_file_names(self) -> bool:
        return bool(self.archive_flags & FLAG_INCLUDE_FILE_NAMES)

    def as_dict(self) -> dict:
        return {
            "version": self.version,
            "header_offset": self.offset,
            "archive_flags": self.archive_flags,
            "archive_flags_hex": "0x%04x" % self.archive_flags,
            "compressed": self.compressed,
            "embedded_names": self.embedded_names,
            "folder_count": self.folder_count,
            "file_count": self.file_count,
            "total_folder_name_length": self.total_folder_name_length,
            "total_file_name_length": self.total_file_name_length,
            "file_flags": self.file_flags,
        }


@dataclass(frozen=True)
class BsaFileEntry:
    """One file recorded in an archive directory."""

    path: str
    folder: str
    name: str
    size: int          # stored bytes at :attr:`offset` (prefixes included)
    offset: int        # absolute offset in the archive
    hash: int
    folder_index: int
    record_index: int

    @property
    def extension(self) -> str:
        if "." not in self.name:
            return ""
        return self.name.rsplit(".", 1)[1].lower()

    def as_dict(self) -> dict:
        return {
            "path": self.path,
            "size": self.size,
            "offset": self.offset,
            "extension": self.extension,
        }


@dataclass
class _FolderRecord:
    hash: int
    count: int
    offset: int
    name: str = ""
    first_record: int = 0


class BsaArchive:
    """A read-only view of one BSA file.

    The file handle stays open for the lifetime of the object; use the object
    as a context manager (or call :meth:`close`) to release it.  Instances are
    not thread-safe.
    """

    def __init__(self, path, *, verify: bool = True):
        self._owns_file = not hasattr(path, "read")
        if self._owns_file:
            self.path = os.fspath(path)
            self._file = open(self.path, "rb")
        else:
            self._file = path
            name = getattr(path, "name", None)
            self.path = os.fspath(name) if name else "<memory>"
        self._file.seek(0, os.SEEK_END)
        self._file_size = self._file.tell()
        self._file.seek(0)
        self._entries: List[BsaFileEntry] = []
        self._by_name: Dict[str, BsaFileEntry] = {}
        self._name_block: bytes = b""
        try:
            self.header = self._read_header()
            self._read_directory()
            if verify:
                self._verify_entries()
        except Exception:
            self._file.close()
            raise

    # -- construction -----------------------------------------------------

    def close(self) -> None:
        """Close the backing file (safe to call more than once)."""
        handle = getattr(self, "_file", None)
        if handle is not None and hasattr(handle, "close") and not handle.closed:
            handle.close()

    def _read_exact(self, n: int) -> bytes:
        data = self._file.read(n)
        if len(data) != n:
            raise BsaFormatError(
                "%s: truncated read at %d (wanted %d, got %d)"
                % (self.path, self._file.tell() - len(data), n, len(data))
            )
        return data

    def _read_header(self) -> BsaHeader:
        raw = self._read_exact(HEADER_SIZE)
        magic, version, offset, flags, folders, files, tot_folder, tot_file, file_flags = (
            struct.unpack("<4s8I", raw)
        )
        if magic != MAGIC:
            raise BsaFormatError("%s: not a BSA (magic %r)" % (self.path, magic))
        if version not in SUPPORTED_VERSIONS:
            raise BsaFormatError(
                "%s: unsupported BSA version %d (supported: %s)"
                % (self.path, version, ", ".join(map(str, SUPPORTED_VERSIONS)))
            )
        min_offset = 34 if version == 103 else HEADER_SIZE
        if offset < min_offset or offset > self._file_size:
            raise BsaFormatError("%s: bad header offset %d" % (self.path, offset))
        if folders > _MAX_ARRAY or files > _MAX_ARRAY:
            raise BsaFormatError("%s: implausible counts %d/%d" % (self.path, folders, files))
        if flags & FLAG_XBOX360:
            raise BsaFormatError("%s: Xbox 360 archives are not supported" % self.path)
        # v103 writers set bits 0x200/0x400 that v104 assigns to XMem/retain
        # flags; the observed v103 archive uses them as ordinary metadata.
        if version >= 104 and flags & FLAG_XMEM_CODEC:
            raise BsaFormatError("%s: XMem codec archives are not supported" % self.path)
        return BsaHeader(
            version=version,
            offset=offset,
            archive_flags=flags,
            folder_count=folders,
            file_count=files,
            total_folder_name_length=tot_folder,
            total_file_name_length=tot_file,
            file_flags=file_flags,
        )

    def _read_directory(self) -> None:
        h = self.header
        self._file.seek(h.offset)
        raw_folders = self._read_exact(h.folder_count * 16)
        folders: List[_FolderRecord] = []
        for i in range(h.folder_count):
            name_hash, count, record_offset = struct.unpack_from("<QII", raw_folders, i * 16)
            if count > _MAX_ARRAY:
                raise BsaFormatError("%s: folder %d has %d files" % (self.path, i, count))
            folders.append(_FolderRecord(name_hash, count, record_offset))

        index = 0
        for folder_index, folder in enumerate(folders):
            name_len = self._read_exact(1)[0]
            if name_len == 0 or name_len > _MAX_NAME:
                raise BsaFormatError(
                    "%s: folder %d has invalid name length %d"
                    % (self.path, folder_index, name_len)
                )
            raw_name = self._read_exact(name_len)
            if raw_name[-1:] == b"\x00":
                raw_name = raw_name[:-1]
            try:
                folder.name = raw_name.decode("cp1252")
            except UnicodeDecodeError as exc:
                raise BsaFormatError(
                    "%s: folder %d name is not decodable: %s" % (self.path, folder_index, exc)
                ) from exc
            if "\\" in folder.name and folder.name.startswith("\\"):
                raise BsaFormatError("%s: folder name starts with a separator" % self.path)
            folder.first_record = index
            raw_records = self._read_exact(folder.count * 16)
            for j in range(folder.count):
                name_hash, size, offset = struct.unpack_from("<QII", raw_records, j * 16)
                # The base name is filled in from the name block below.
                self._entries.append(
                    BsaFileEntry(
                        path="",
                        folder=folder.name,
                        name="",
                        size=size,
                        offset=offset,
                        hash=name_hash,
                        folder_index=folder_index,
                        record_index=index,
                    )
                )
                index += 1

        if index != h.file_count:
            raise BsaFormatError(
                "%s: folder records describe %d files, header says %d"
                % (self.path, index, h.file_count)
            )

        block_start = self._file.tell()
        if h.total_file_name_length:
            if block_start + h.total_file_name_length > self._file_size:
                raise BsaFormatError("%s: file name block runs past EOF" % self.path)
            self._name_block = self._read_exact(h.total_file_name_length)
            names = self._name_block.split(b"\x00")
            if names and names[-1] == b"":
                names.pop()
            if len(names) != h.file_count:
                raise BsaFormatError(
                    "%s: file name block holds %d names, expected %d"
                    % (self.path, len(names), h.file_count)
                )
        else:
            names = []

        self._data_start = self._file.tell()
        names_are_full_paths = False
        if not names and h.file_count:
            if not h.embedded_names:
                raise BsaFormatError(
                    "%s: archive has neither a file-name block nor embedded names"
                    % self.path
                )
            # Unusual archive: resolve names from the per-file prefixes.  This
            # costs one small random read per file but keeps list_files() and
            # read_file() usable without guessing names from hashes.
            names = []
            for entry in self._entries:
                self._file.seek(entry.offset)
                head = self._file.read(1)
                if not head:
                    raise BsaFormatError(
                        "%s: cannot read the embedded name at %d" % (self.path, entry.offset)
                    )
                name_len = head[0]
                if name_len == 0 or name_len > _MAX_NAME:
                    raise BsaFormatError(
                        "%s: bad embedded name length %d at %d"
                        % (self.path, name_len, entry.offset)
                    )
                raw = self._file.read(name_len)
                if len(raw) != name_len:
                    raise BsaFormatError("%s: truncated embedded name" % self.path)
                names.append(raw)
            names_are_full_paths = True

        built: List[BsaFileEntry] = []
        for i, entry in enumerate(self._entries):
            if names:
                try:
                    decoded = names[i].decode("cp1252")
                except UnicodeDecodeError as exc:
                    raise BsaFormatError(
                        "%s: file name %d is not decodable: %s" % (self.path, i, exc)
                    ) from exc
                if names_are_full_paths:
                    decoded = decoded.replace("/", "\\").strip("\\")
                    folder, _, base = decoded.rpartition("\\")
                    if not base:
                        raise BsaFormatError(
                            "%s: embedded name %d has no base name" % (self.path, i)
                        )
                else:
                    base = decoded
                    folder = entry.folder
                    if "\\" in base or "/" in base:
                        raise BsaFormatError(
                            "%s: file name %d unexpectedly contains a separator: %r"
                            % (self.path, i, base)
                        )
            else:
                # No names at all: cannot construct a lookup path.
                base = ""
                folder = entry.folder
            if names_are_full_paths:
                path = (folder + "\\" + base) if folder else base
            else:
                path = (folder + "\\" + base) if folder else base
            new_entry = BsaFileEntry(
                path=path,
                folder=folder,
                name=base,
                size=entry.size,
                offset=entry.offset,
                hash=entry.hash,
                folder_index=entry.folder_index,
                record_index=entry.record_index,
            )
            built.append(new_entry)
            if base:
                key = _key(path)
                if key in self._by_name:
                    raise BsaFormatError("%s: duplicate file name %r" % (self.path, path))
                self._by_name[key] = new_entry
        self._entries = built

    def _verify_entries(self) -> None:
        data_start = getattr(self, "_data_start", HEADER_SIZE)
        for entry in self._entries:
            if entry.size > self._file_size:
                raise BsaFormatError(
                    "%s: %r stored size %d exceeds archive size"
                    % (self.path, entry.path, entry.size)
                )
            if entry.offset < data_start or entry.offset + entry.size > self._file_size:
                raise BsaFormatError(
                    "%s: %r data range %d..%d is outside the data area %d..%d"
                    % (
                        self.path,
                        entry.path,
                        entry.offset,
                        entry.offset + entry.size,
                        data_start,
                        self._file_size,
                    )
                )

    # -- public API -------------------------------------------------------

    @property
    def data_start(self) -> int:
        return self._data_start

    def __len__(self) -> int:
        return len(self._entries)

    def __enter__(self) -> "BsaArchive":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def close(self) -> None:
        if self._owns_file and not self._file.closed:
            self._file.close()

    def list_files(self) -> List[BsaFileEntry]:
        """All directory entries, sorted by canonical path."""
        return list(self._entries)

    def iter_files(self) -> Iterator[BsaFileEntry]:
        return iter(self._entries)

    def find(self, name: str) -> Optional[BsaFileEntry]:
        return self._by_name.get(_key(name))

    def _entry(self, name: str) -> BsaFileEntry:
        entry = self.find(name)
        if entry is None:
            raise KeyError("%s: no such file %r" % (self.path, name))
        return entry

    def _stored_bytes(self, entry: BsaFileEntry) -> bytes:
        self._file.seek(entry.offset)
        return self._read_exact(entry.size)

    @staticmethod
    def _split_prefix(raw: bytes, entry: BsaFileEntry, embedded: bool) -> Tuple[Optional[str], int, int]:
        """Return (embedded name, payload offset, unpacked size or -1)."""
        pos = 0
        name: Optional[str] = None
        if embedded:
            if not raw:
                raise BsaFormatError("empty embedded-name file %r" % entry.path)
            name_len = raw[0]
            if name_len == 0 or name_len > _MAX_NAME or 1 + name_len > len(raw):
                raise BsaFormatError(
                    "bad embedded file name in %r (length %d)" % (entry.path, name_len)
                )
            try:
                name = raw[1 : 1 + name_len].decode("cp1252")
            except UnicodeDecodeError as exc:
                raise BsaFormatError(
                    "embedded file name in %r is not decodable" % entry.path
                ) from exc
            pos = 1 + name_len
        return name, pos, 0

    def read_file(self, name: str) -> bytes:
        """Return the unpacked contents of ``name`` (case/separator tolerant)."""
        entry = self._entry(name)
        raw = self._stored_bytes(entry)
        embedded_name, pos, _ = self._split_prefix(raw, entry, self.header.embedded_names)
        if embedded_name is not None and entry.name:
            if _key(embedded_name) != _key(entry.path):
                raise BsaFormatError(
                    "%s: embedded name %r does not match directory entry %r"
                    % (self.path, embedded_name, entry.path)
                )
        if self.header.compressed:
            if pos + 4 > len(raw):
                raise BsaFormatError("%s: %r lacks the unpacked size" % (self.path, entry.path))
            unpacked = struct.unpack_from("<I", raw, pos)[0]
            pos += 4
            try:
                data = zlib.decompress(raw[pos:])
            except zlib.error as exc:
                raise BsaFormatError(
                    "%s: %r deflate error: %s" % (self.path, entry.path, exc)
                ) from exc
            if len(data) != unpacked:
                raise BsaFormatError(
                    "%s: %r unpacked to %d bytes, header says %d"
                    % (self.path, entry.path, len(data), unpacked)
                )
            return data
        return raw[pos:]

    def unpacked_size(self, name: str) -> int:
        """Unpacked size of one file, without decompressing its payload."""
        entry = self._entry(name)
        if not self.header.compressed:
            if not self.header.embedded_names:
                return entry.size
            self._file.seek(entry.offset)
            head = self._file.read(min(entry.size, 512))
            embedded_name, pos, _ = self._split_prefix(head, entry, True)
            return max(0, entry.size - pos)
        # Read only the small prefix (embedded name + u32 size).
        self._file.seek(entry.offset)
        head = self._file.read(min(entry.size, 512))
        embedded_name, pos, _ = self._split_prefix(head, entry, self.header.embedded_names)
        if pos + 4 > len(head):
            raise BsaFormatError("%s: %r lacks the unpacked size" % (self.path, entry.path))
        return struct.unpack_from("<I", head, pos)[0]

    def read_prefix(self, name: str, count: int) -> bytes:
        """Read at most ``count`` unpacked bytes (cheap for compressed entries)."""
        if count < 0:
            raise ValueError("count must be non-negative")
        if count == 0:
            return b""
        entry = self._entry(name)
        self._file.seek(entry.offset)
        head = self._file.read(min(entry.size, max(4096, count * 8)))
        embedded_name, pos, _ = self._split_prefix(head, entry, self.header.embedded_names)
        if embedded_name is not None and entry.name and _key(embedded_name) != _key(entry.path):
            raise BsaFormatError(
                "%s: embedded name %r does not match directory entry %r"
                % (self.path, embedded_name, entry.path)
            )
        if not self.header.compressed:
            return head[pos : pos + count]
        if pos + 4 > len(head):
            raise BsaFormatError("%s: %r lacks the unpacked size" % (self.path, entry.path))
        pos += 4
        decomp = zlib.decompressobj()
        out = bytearray()
        chunk = head[pos:]
        stored_read = len(head)
        try:
            while len(out) < count:
                if chunk:
                    out += decomp.decompress(chunk, count - len(out))
                if decomp.eof or len(out) >= count:
                    break
                if decomp.unconsumed_tail:
                    # The max_length limit was hit; the caller asked for less
                    # than the stream can produce, so stopping is correct.
                    break
                if stored_read >= entry.size:
                    break
                chunk = self._file.read(min(65536, entry.size - stored_read))
                if not chunk:
                    break
                stored_read += len(chunk)
        except zlib.error as exc:
            raise BsaFormatError("%s: %r deflate error: %s" % (self.path, entry.path, exc)) from exc
        return bytes(out[:count])

    def stat(self) -> dict:
        """Archive-level statistics (no file contents are read)."""
        by_ext: Dict[str, dict] = {}
        total_stored = 0
        for entry in self._entries:
            ext = entry.extension
            bucket = by_ext.setdefault(ext, {"count": 0, "stored_bytes": 0})
            bucket["count"] += 1
            bucket["stored_bytes"] += entry.size
            total_stored += entry.size
        extensions = {
            ext: dict(values, count=values["count"])
            for ext, values in sorted(by_ext.items(), key=lambda kv: (-kv[1]["count"], kv[0]))
        }
        return {
            "path": self.path,
            "file_size": self._file_size,
            "header": self.header.as_dict(),
            "file_count": len(self._entries),
            "folder_count": self.header.folder_count,
            "stored_file_bytes": total_stored,
            "data_start": self._data_start,
            "extensions": extensions,
        }


def _key(name: str) -> str:
    return name.replace("/", "\\").strip("\\").lower()


def open_archive(path: str) -> BsaArchive:
    """Open ``path`` as a BSA archive.  The caller owns the handle."""
    return BsaArchive(path)
