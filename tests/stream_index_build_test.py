"""Real-data check for tools/stream_index_build.py (interior cell table).

Skipped when the Skyrim Data tree is absent.  Builds RiverwoodSleepingGiantInn
(0x133C6) twice, checks determinism, parses the device format with the existing
Python reader, verifies the 64-byte cell row and resolves every indexed path
through the real archive directory (unpacked size > 0 and first bytes readable).

The overlay-level check (GetFileAttributesExA / ReadFile in guest shims) is
tests/stream_cell_real_smoke.c, which takes the same index path.
"""

from __future__ import annotations

import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from skyrim import bsa, stream_index  # noqa: E402
import stream_index_build  # noqa: E402

DATA = os.environ.get(
    "SKYRIM_DATA", r"E:/SteamLibrary/steamapps/common/Skyrim/Data")
ESM = os.path.join(DATA, "Skyrim.esm")
FORM_ID = 0x133C6

checks = 0
failures = 0


def check(condition, what):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print("FAIL: %s" % what)


def parse_cells(blob: bytes):
    header = struct.unpack_from("<4s6H4I6Q", blob, 0)
    (_magic, _schema, _hs, _as, _rs, _cs, _flags, archives, resources,
     cells, _reserved, _generation, _ao, _ro, _po, cell_off, _total) = header
    rows = []
    for i in range(cells):
        raw = struct.unpack_from("<IIiiIIII8I", blob,
                                 cell_off + i * stream_index_build.CELL_ENTRY_SIZE)
        rows.append({
            "kind": raw[0], "form_id": raw[1], "gx": raw[2], "gy": raw[3],
            "ws": raw[4], "count": raw[6], "ids": list(raw[8:16]),
        })
    return archives, resources, rows


def main() -> int:
    if not os.path.isfile(ESM):
        print("stream_index_build_test: skipped (no %s)" % ESM)
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        first, report = stream_index_build.build(
            DATA, ESM, ["0x%X" % FORM_ID],
            list(stream_index_build.DEFAULT_ARCHIVES), False)
        second, _ = stream_index_build.build(
            DATA, ESM, ["0x%X" % FORM_ID],
            list(stream_index_build.DEFAULT_ARCHIVES), False)
        path = os.path.join(tmp, "stream_index.bin")
        with open(path, "wb") as handle:
            handle.write(first)

        check(first == second, "two builds of the same inputs are byte-identical")
        check(len(first) > stream_index_build.HEADER_SIZE + 4,
              "index is larger than its header")

        parsed = stream_index.read_index(path)
        check(parsed.cell_count == 1, "exactly one interior cell row")
        archives, resources, rows = parse_cells(first)
        check(archives == len(parsed.archives), "archive count matches table")
        check(resources == len(parsed.resources), "resource count matches table")
        check(len(rows) == 1 and rows[0]["kind"] == 0,
              "cell row is an interior key")
        check(rows[0]["form_id"] == FORM_ID,
              "cell row key is RiverwoodSleepingGiantInn")
        check(0 < rows[0]["count"] <= stream_index_build.CELL_IDS,
              "cell row has 1..8 resource ids")
        check(all(i < len(parsed.resources) for i in rows[0]["ids"][:rows[0]["count"]]),
              "every cell id is inside the resource table")

        cell_stats = report["cells"][0]
        check(cell_stats["editor_id"] == "RiverwoodSleepingGiantInn",
              "report names the cell")
        check(cell_stats["meshes_resolved"] == 142,
              "all MODL meshes of the cell resolve in the archives")
        check(cell_stats["assets_total"] >= cell_stats["indexed"],
              "indexed count is a truncation of resolved assets")
        print("riverwood: refs=%d meshes=%d assets=%d indexed=%d bytes=%d "
              "truncated=%d" % (
                  cell_stats["refs"], cell_stats["meshes_resolved"],
                  cell_stats["assets_total"], cell_stats["indexed"],
                  cell_stats["indexed_bytes"], cell_stats["truncated"]))

        opened = {}
        for i in rows[0]["ids"][:rows[0]["count"]]:
            resource = parsed.resources[i]
            archive_name = parsed.archives[resource.archive].name
            archive_path = os.path.join(DATA, archive_name)
            if archive_path not in opened:
                opened[archive_path] = bsa.BsaArchive(archive_path, verify=False)
            archive = opened[archive_path]
            entry = archive.find(resource.path)
            check(entry is not None, "path exists in %s: %s" % (
                archive_name, resource.path))
            if entry is None:
                continue
            unpacked = archive.unpacked_size(resource.path)
            check(unpacked > 0, "unpacked size > 0: %s" % resource.path)
            head = archive.read_prefix(resource.path, min(4096, unpacked))
            check(len(head) > 0, "prefix readable: %s" % resource.path)
            check(resource.payload_length > 0 and
                  resource.payload_length <= entry.size,
                  "payload range is inside the stored entry")
            check(resource.physical_offset + resource.payload_length <=
                  parsed.archives[resource.archive].bytes,
                  "physical range is inside the archive")

        for archive in opened.values():
            archive.close()

    print("stream_index_build_test: %d checks, %d failure(s)" %
          (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
