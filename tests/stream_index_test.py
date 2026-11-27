"""Tests for tools/skyrim/stream_index.py on synthetic cooked archives.

No game asset is read.  The fixture writes real v104 BSAs with
``bsa_write.write_bsa_stream`` (the same writer ``stage --pack cooked`` uses),
builds the manifest rows deploy_device would produce, and checks the index
round-trip, determinism, range bounds and the refusal paths.
"""

import hashlib
import json
import shutil
import struct
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from skyrim import bsa, bsa_write, stream_index  # noqa: E402


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_row(path, archive, payload, source=None):
    return {
        "path": path,
        "archive": archive,
        "bytes": len(payload),
        "sha256": sha(payload),
        "source_sha256": sha(source if source is not None else payload),
    }


def archive_row(path):
    blob = Path(path).read_bytes()
    return {
        "name": Path(path).name,
        "bytes": len(blob),
        "sha256": hashlib.sha256(blob).hexdigest(),
    }


def build_fixture(root, *, embed_names=False, compressed=False):
    data = root / "Data"
    data.mkdir()
    payloads = {
        "meshes\\arch\\crate.nif": b"NIF!" + bytes(range(64)) * 3,
        "textures\\arch\\stone.dds": b"DDS " + bytes(range(128)) * 5,
        "sound\\fx\\door.wav": b"RIFF" + b"\x00\xff" * 40,
        "interface\\map.swf": b"FWS" + bytes(range(32)),
    }
    groups = {
        "Skyrim - Cooked meshes.bsa": ["meshes\\arch\\crate.nif"],
        "Skyrim - Cooked textures.bsa": ["textures\\arch\\stone.dds"],
        "Skyrim - Cooked other.bsa": ["sound\\fx\\door.wav",
                                      "interface\\map.swf"],
    }
    rows = []
    for name, paths in groups.items():
        bsa_write.write_bsa_stream(
            str(data / name), [(path, payloads[path]) for path in paths],
            compressed=compressed, embed_names=embed_names)
        for path in paths:
            rows.append(file_row(path, name, payloads[path]))
    archives = [archive_row(data / name) for name in groups]
    return data, archives, rows, payloads


def expect_error(fn, needle):
    try:
        fn()
    except stream_index.StreamIndexError as exc:
        assert needle in str(exc), (needle, str(exc))
        return
    raise AssertionError("expected StreamIndexError containing %r" % needle)


def main():
    checks = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        data, archives, rows, payloads = build_fixture(root)
        out = root / "stream_index.bin"
        report = stream_index.write_index(archives, rows, str(data), str(out))
        blob = out.read_bytes()
        assert report["resources"] == len(rows) == 4, report
        assert report["archives"] == 3
        assert len(blob) == report["bytes"]
        checks += 4

        index = stream_index.read_index(str(out))
        assert index.generation == report["generation"]
        assert [row.name for row in index.archives] == [r["name"] for r in archives]
        assert [row.priority for row in index.archives] == [0, 1, 2]
        assert [row.files for row in index.archives] == [1, 1, 2]
        checks += 3

        # Every row is derived from the real directory record, not an estimate.
        for row in index.resources:
            archive_row_value = index.archives[row.archive]
            with bsa.BsaArchive(str(data / archive_row_value.name)) as archive:
                entry = archive.find(row.path)
                assert entry is not None, row.path
                assert row.entry_offset == entry.offset, row.path
                assert row.stored_size == entry.size, row.path
                assert row.physical_offset == entry.offset + 0  # no prefixes
                assert row.payload_length == entry.size
                assert row.physical_offset + row.payload_length <= \
                    archive_row_value.bytes
                assert row.sha256 == sha(archive.read_file(entry.path))
        checks += 1

        # Lookup accepts the reader's casing/separator normalisation.
        upper = index.find_path("MESHES/ARCH/CRATE.NIF")
        assert upper is not None and upper.id == index.find_path(
            "meshes\\arch\\crate.nif").id
        assert index.find_path("meshes\\arch\\missing.nif") is None
        assert index.find_id(0).path == index.resources[0].path
        assert index.find_id(len(index.resources)) is None
        assert index.find_id(0xFFFFFFFF) is None
        checks += 5

        # Determinism: rewritten and row-order-independent.
        again = root / "again.bin"
        reordered = list(reversed(rows))
        report2 = stream_index.write_index(archives, reordered, str(data),
                                           str(again))
        assert again.read_bytes() == blob
        assert report2["generation"] == report["generation"]
        checks += 2

        # Embedded-name archives: the physical offset skips the prefix.
        embed_root = root / "embed"
        embed_root.mkdir()
        edata, earchives, erows, epayloads = build_fixture(
            embed_root, embed_names=True)
        eout = root / "embed_index.bin"
        er = stream_index.write_index(earchives, erows, str(edata), str(eout))
        eindex = stream_index.read_index(str(eout))
        for row in eindex.resources:
            with bsa.BsaArchive(str(edata / eindex.archives[row.archive].name)) as a:
                entry = a.find(row.path)
                assert row.physical_offset == entry.offset + 1 + len(row.path)
                assert row.flags & 0x2
                assert a.read_file(row.path) == epayloads[row.path]
        assert er["resources"] == 4
        checks += 3

        # --- refusal paths ------------------------------------------------
        bad = [dict(r) for r in archives]
        bad[0] = dict(bad[0], bytes=bad[0]["bytes"] + 1)
        expect_error(lambda: stream_index.build_index(bad, rows, str(data)),
                     "manifest says")
        bad[0] = dict(archives[0], sha256="00" * 32)
        expect_error(lambda: stream_index.build_index(bad, rows, str(data)),
                     "SHA-256")
        bad_rows = [dict(r) for r in rows]
        bad_rows[0]["bytes"] += 1
        expect_error(lambda: stream_index.build_index(archives, bad_rows,
                                                      str(data)), "stores")
        bad_rows = [dict(r) for r in rows]
        bad_rows[0]["sha256"] = "11" * 32
        expect_error(lambda: stream_index.build_index(archives, bad_rows,
                                                      str(data)), "SHA-256")
        bad_rows = [dict(r) for r in rows]
        bad_rows[0]["archive"] = ""
        expect_error(lambda: stream_index.build_index(archives, bad_rows,
                                                      str(data)), "archive")
        bad_rows = [dict(r) for r in rows]
        duplicate = next(r for r in bad_rows
                         if r["archive"] == archives[1]["name"])
        duplicate["path"] = rows[0]["path"]
        expect_error(lambda: stream_index.build_index(archives, bad_rows,
                                                      str(data)), "duplicate")
        checks += 6

        comp_root = root / "compressed"
        comp_root.mkdir()
        cdata, carchives, crows, _ = build_fixture(comp_root, compressed=True)
        expect_error(lambda: stream_index.build_index(carchives, crows,
                                                      str(cdata)),
                     "compressed")
        checks += 1

        # Tampered tables: the parser validates offsets, ranges and the CRC.
        def tamper(offset, fmt, value, needle):
            raw = bytearray(blob)
            struct.pack_into(fmt, raw, offset, value)
            struct.pack_into("<I", raw, len(raw) - 4,
                             zlib.crc32(bytes(raw[:-4])) & 0xFFFFFFFF)
            path = root / "tampered.bin"
            path.write_bytes(raw)
            expect_error(lambda: stream_index.read_index(str(path)), needle)

        head = stream_index.HEADER_SIZE
        res0 = head + stream_index.ARCHIVE_ENTRY_SIZE * 3
        tamper(res0 + 16, "<Q", 1 << 40, "outside")
        tamper(res0 + 24, "<I", 0xFFFFFFFF, "outside")
        tamper(head + 52, "<I", 7, "declares")
        tamper(res0 + 8, "<I", 64, "canonical")
        tamper(4, "<H", 2, "schema")
        tamper(8, "<H", 64, "entry sizes")
        raw = bytearray(blob)
        raw[head + 20] ^= 0xFF          # archive SHA-256 field only
        path = root / "badcrc.bin"
        path.write_bytes(raw)
        expect_error(lambda: stream_index.read_index(str(path)), "CRC")
        path = root / "short.bin"
        path.write_bytes(blob[:-10])
        expect_error(lambda: stream_index.read_index(str(path)), "bytes")
        checks += 8

        # A changed archive is caught before ranges are published.
        verify_root = root / "verify"
        shutil.copytree(data, verify_root / "Data")
        with open(verify_root / "Data" / archives[0]["name"], "r+b") as handle:
            handle.seek(-1, 2)
            last = handle.read(1)
            handle.seek(-1, 2)
            handle.write(bytes([last[0] ^ 0xFF]))
        errors = stream_index.verify_index(index, str(verify_root / "Data"))
        assert errors and "SHA-256" in " ".join(errors), errors
        checks += 1

        # Manifest wiring: the same helper deploy_device calls.
        manifest = {"cooked": True, "out_root": str(root),
                    "archives": archives, "files": rows}
        manifest_out = root / "manifest_index.bin"
        mreport = stream_index.write_from_manifest(manifest, str(manifest_out))
        assert mreport["generation"] == report["generation"]
        assert manifest_out.read_bytes() == blob
        checks += 2

        json.dumps(report)  # the report must stay JSON-serialisable
        checks += 1

    print("stream_index_test: %d checks OK" % checks)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
