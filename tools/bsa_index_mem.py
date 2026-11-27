#!/usr/bin/env python3
"""Measure the resident BSA directory index and build a lookup corpus.

The C reader (source/bsa_reader.c) exposes bsa_index_bytes(), so the real
"before/after" figure comes from tests/bsa_index_smoke.c.  This script is the
independent companion:

  * parses the archive directories with only the documented layout (header,
    folder records, folder names, file records, file-name block, bounded
    embedded prefixes);
  * reports the old resident layout (entry/folder structs + resident name
    bytes + open-addressed u32 table) and the compact layout target;
  * writes a deterministic query corpus used by the C test: real paths with
    case and separator variants, cross-archive duplicates and misses.

Read-only.  F: (the SD card) is never touched; the default root is the E:
harness install.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import tempfile

ENTRY_OLD = 24          # BsaEntryInternal: hash8 + 4 u32
FOLDER_OLD = 8          # BsaFolderInternal: two u32


def u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def u64(b: bytes, o: int) -> int:
    return struct.unpack_from("<Q", b, o)[0]


def next_pow2(n: int) -> int:
    v = 16
    while v < n:
        v <<= 1
    return v


class Archive:
    def __init__(self, path: str) -> None:
        self.path = path
        self.name = os.path.basename(path)
        with open(path, "rb") as f:
            self.size = os.fstat(f.fileno()).st_size
            head = f.read(36)
            if len(head) != 36 or head[:4] != b"BSA\0":
                raise ValueError(f"{path}: not a BSA")
            (self.version, header_off, self.flags, self.folder_count,
             self.file_count, self.folder_name_bytes,
             self.file_name_bytes, self.file_flags) = struct.unpack(
                "<IIIIIIII", head[4:36])
            if self.version not in (103, 104, 105):
                raise ValueError(f"{path}: version {self.version}")
            self.embedded = self.version >= 104 and (self.flags & 0x100) != 0
            cursor = header_off
            record_bytes = f.read if False else None
            f.seek(cursor)
            raw = f.read(self.folder_count * 16)
            if len(raw) != self.folder_count * 16:
                raise ValueError(f"{path}: truncated folder records")
            counts = [u32(raw, i * 16 + 8) for i in range(self.folder_count)]
            cursor += self.folder_count * 16
            folders = []
            for i in range(self.folder_count):
                f.seek(cursor)
                ln = f.read(1)
                if len(ln) != 1 or ln[0] == 0 or ln[0] >= 512:
                    raise ValueError(f"{path}: bad folder name length")
                n = ln[0]
                raw_name = f.read(n)
                if len(raw_name) != n:
                    raise ValueError(f"{path}: truncated folder name")
                cursor += 1 + n
                name = raw_name[:-1] if raw_name.endswith(b"\0") else raw_name
                folders.append((name.decode("latin1").replace("/", "\\"),
                                cursor, counts[i]))
                cursor += 16 * counts[i]
            if sum(counts) != self.file_count:
                raise ValueError(f"{path}: folder counts do not sum to file_count")

            records = []
            for fi, (fname, rec_at, n) in enumerate(folders):
                f.seek(rec_at)
                for j in range(n):
                    rec = f.read(16)
                    if len(rec) != 16:
                        raise ValueError(f"{path}: truncated file record")
                    records.append((u64(rec, 0), u32(rec, 8), u32(rec, 12),
                                    fi, fname))
            self.data_start = cursor

            if self.file_name_bytes:
                f.seek(cursor)
                block = f.read(self.file_name_bytes)
                if len(block) != self.file_name_bytes:
                    raise ValueError(f"{path}: truncated file-name block")
                cursor += self.file_name_bytes
                base_names = block.split(b"\0")
                if len(base_names) - 1 != self.file_count:
                    raise ValueError(f"{path}: name count mismatch")
                self.paths = []
                for i, (_h, _s, _o, _fi, fname) in enumerate(records):
                    base = base_names[i].decode("latin1")
                    self.paths.append(
                        (fname + "\\" + base) if fname else base)
            else:
                if not self.embedded:
                    raise ValueError(f"{path}: no names")
                self.paths = []
                for (_h, size, off, _fi, _fname) in records:
                    f.seek(off)
                    head = f.read(1)
                    if len(head) != 1 or head[0] == 0 or head[0] >= 512:
                        raise ValueError(f"{path}: bad embedded name length")
                    n = head[0]
                    raw_name = f.read(n)
                    if len(raw_name) != n or 1 + n > size:
                        raise ValueError(f"{path}: bad embedded name")
                    if raw_name.endswith(b"\0"):
                        raw_name = raw_name[:-1]
                    self.paths.append(
                        raw_name.decode("latin1").replace("/", "\\"))

            self.records = records
            self.name_bytes = sum(len(p) + 1 for p in self.paths)
            self.table_slots = next_pow2(self.file_count * 2)
            self.old_bytes = (self.file_count * ENTRY_OLD +
                              self.folder_count * FOLDER_OLD +
                              self.name_bytes + self.folder_name_bytes +
                              self.table_slots * 4)
            # Compact: u32 record-index table + u8 tag (~0.8 load max),
            # u32 name checkpoints every 16 names, 20-byte folder locator.
            self.new_slots = next_pow2((self.file_count * 5) // 4 + 1)
            self.new_bytes = (self.new_slots * 5 +
                              ((self.file_count + 15) // 16 + 1) * 4 +
                              self.folder_count * 20)

    def find(self, path: str):
        key = path.strip("/\\").replace("/", "\\").lower()
        for i, p in enumerate(self.paths):
            if p.lower() == key:
                return i
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.environ.get(
        "TESV_DATA_ROOT", os.environ.get(
            "SKYRIM_DATA", r"E:\harness_skyrim\Data")))
    ap.add_argument("--paths-out", default=os.path.join(
        tempfile.gettempdir(), "bsa_index_paths.txt"))
    ap.add_argument("--sample", type=int, default=30000)
    ap.add_argument("--misses", type=int, default=3000)
    ap.add_argument("--max-mib", type=float, default=2.0)
    args = ap.parse_args()

    if not os.path.isdir(args.data):
        print(f"data root absent: {args.data}", file=sys.stderr)
        return 2
    files = sorted(p for p in os.listdir(args.data)
                   if p.lower().endswith(".bsa"))
    if not files:
        print(f"no archives under {args.data}", file=sys.stderr)
        return 2

    archives = []
    old_total = new_total = files_total = folders_total = 0
    print(f"{'archive':28s} {'files':>7s} {'folders':>7s} "
          f"{'old KiB':>9s} {'new KiB':>9s}")
    for name in files:
        a = Archive(os.path.join(args.data, name))
        archives.append(a)
        old_total += a.old_bytes
        new_total += a.new_bytes
        files_total += a.file_count
        folders_total += a.folder_count
        print(f"{a.name:28s} {a.file_count:7d} {a.folder_count:7d} "
              f"{a.old_bytes / 1024:9.1f} {a.new_bytes / 1024:9.1f}")
    print(f"{'TOTAL':28s} {files_total:7d} {folders_total:7d} "
          f"{old_total / 1024:9.1f} {new_total / 1024:9.1f}")
    print(f"resident index: old {old_total / (1 << 20):.2f} MiB -> "
          f"compact plan {new_total / (1 << 20):.2f} MiB "
          f"(target <= {args.max_mib:.2f} MiB)")
    if new_total > args.max_mib * (1 << 20):
        print("WARNING: compact plan exceeds the target", file=sys.stderr)

    # Deterministic corpus: evenly spaced real paths, case and separator
    # variants on every 7th/11th entry, cross-archive duplicates, misses.
    all_paths = []
    for a in archives:
        all_paths.extend(a.paths)
    step = max(1, len(all_paths) // max(1, args.sample))
    corpus = []
    for i in range(0, len(all_paths), step):
        p = all_paths[i]
        if i % 7 == 0:
            p = p.upper()
        elif i % 11 == 0:
            p = p.replace("\\", "/")
        corpus.append(p)
        if len(corpus) >= args.sample:
            break
    seen = {}
    for a in archives:
        for p in a.paths:
            seen.setdefault(p.lower(), []).append(p)
    dups = [v[0] for v in seen.values() if len(v) > 1]
    corpus.extend(dups[: max(1, args.sample // 10)])
    for i in range(args.misses):
        corpus.append(f"__missing__\\bsa_index_{i:05d}.nif")
    # A few empty / separator-only and root-level probes.
    corpus.extend(["", "\\", "/", ".\\Interface\\", "interface/startmenu.swf"])

    with open(args.paths_out, "w", encoding="utf-8", newline="\n") as f:
        for p in corpus:
            f.write(p + "\n")
    print(f"query corpus: {len(corpus)} paths -> {args.paths_out}")
    print(f"cross-archive duplicate paths: {len(dups)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
