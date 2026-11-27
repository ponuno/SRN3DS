"""Real-data check for tools/stream_index_build.py (schema 2 cell table).

Skipped when the Skyrim Data tree is absent.  Builds RiverwoodSleepingGiantInn
(0x133C6) twice for determinism and a 3x3 Tamriel window around Riverwood
(5,-11) for exterior rows.  Checks the 64-byte cell keys, resolves every
indexed path through the real archive directory (unpacked size > 0 and first
bytes readable), and re-reads the output with the Python device-format reader.

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
    (_magic, schema, _hs, _as, _rs, _cs, _flags, archives, resources,
     cells, _reserved, _generation, _ao, _ro, _po, cell_off, _total) = header
    rows = []
    for i in range(cells):
        raw = struct.unpack_from("<IIiiIIII8I", blob,
                                 cell_off + i * stream_index_build.CELL_ENTRY_SIZE)
        rows.append({
            "kind": raw[0], "form_id": raw[1], "gx": raw[2], "gy": raw[3],
            "ws": raw[4], "count": raw[6], "ids": list(raw[8:16]),
        })
    return schema, archives, resources, rows


def verify_rows(parsed, rows, data_dir):
    opened = {}
    for row in rows:
        for i in row["ids"][:row["count"]]:
            resource = parsed.resources[i]
            archive_name = parsed.archives[resource.archive].name
            archive_path = os.path.join(data_dir, archive_name)
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
        schema, archives, resources, rows = parse_cells(first)
        check(schema == 2, "builder writes schema 2")
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

        verify_rows(parsed, rows, DATA)

        # Exterior window: Tamriel around Riverwood (grid 5,-11, radius 1).
        ext_blob, ext_report = stream_index_build.build(
            DATA, ESM, [], list(stream_index_build.DEFAULT_ARCHIVES), False,
            worlds=["Tamriel"], grid=(5, -11), radius=1)
        ext_path = os.path.join(tmp, "stream_index_exterior.bin")
        with open(ext_path, "wb") as handle:
            handle.write(ext_blob)
        ext_parsed = stream_index.read_index(ext_path)
        ext_schema, _a, _r, ext_rows = parse_cells(ext_blob)
        check(ext_schema == 2, "exterior build is schema 2")
        check(ext_report["exterior_count"] == ext_report["cell_count"] == 9,
              "radius-1 window emits nine exterior cells")
        check(ext_report["interior_count"] == 0,
              "exterior-only window has no interior rows")
        check(all(row["kind"] == 1 and row["form_id"] == 0 for row in ext_rows),
              "exterior rows use kind 1 and a zero form id")
        check(all(row["ws"] == 0x3C for row in ext_rows),
              "exterior rows carry Tamriel's form id")
        keys = [(row["gx"], row["gy"], row["ws"]) for row in ext_rows]
        check(keys == sorted(keys) and len(set(keys)) == len(keys),
              "exterior keys are unique and sorted")
        check(all(-1 <= row["gx"] - 5 <= 1 and -1 <= row["gy"] + 11 <= 1
                  for row in ext_rows), "rows stay inside the requested window")
        check(all(0 < row["count"] <= stream_index_build.CELL_IDS
                  for row in ext_rows), "every exterior row has 1..8 ids")
        verify_rows(ext_parsed, ext_rows, DATA)
        print("tamriel 5,-11 r1: cells=%d resources=%d archives=%d "
              "resolver=%s" % (
                  ext_report["cell_count"], ext_report["resource_count"],
                  ext_report["archive_count"], ext_report["resolver"]))
        check(0 < ext_report["resource_count"] < 1000,
              "bounded window has a bounded resource table")

    print("stream_index_build_test: %d checks, %d failure(s)" %
          (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
