"""Tests tools/deploy_device.py on a synthetic Skyrim Data tree.

No game asset is read: the fixture writes a minimal TES4 header, a v104 BSA
with three textures, and one loose override.  The test asserts the winner
resolution, the texture policy (keep / convert / refuse) and that ``stage``
really writes a DDS the parser accepts.
"""

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import deploy_device as deploy  # noqa: E402
from skyrim import bsa, bsa_write, dds, stream_index  # noqa: E402


def tes4_header(masters=()):
    payload = b"HEDR" + struct.pack("<H", 12) + struct.pack(
        "<fii", 1.7, 0, 0)
    payload += b"CNAM" + struct.pack("<H", 4) + b"test"
    for master in masters:
        raw = master.encode("cp1252") + b"\x00"
        payload += b"MAST" + struct.pack("<H", len(raw)) + raw
    return b"TES4" + struct.pack("<IIIHHHH", len(payload), 0x1, 0,
                                 0, 0, 44, 0) + payload


def dds_file(width, height, *, fourcc=None, bit_count=32,
             masks=(0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000),
             payload=None):
    pf_flags = 0x4 if fourcc else 0x41
    if fourcc:
        fourcc_value = struct.unpack("<I", fourcc)[0]
        bit_count = 0
        masks = (0, 0, 0, 0)
    header = struct.pack("<4s7I", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000,
                         height, width, 0, 0, 1)
    header += b"\x00" * 44
    header += struct.pack("<8I", 32, pf_flags, fourcc_value if fourcc else 0,
                          bit_count, *masks)
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    assert len(header) == 128
    if payload is None:
        if fourcc in (b"DXT1", b"BC1U"):
            payload = b"\x00" * (max(1, width // 4) * max(1, height // 4) * 8)
        elif fourcc:
            payload = b"\x00" * (max(1, width // 4) * max(1, height // 4) * 16)
        else:
            payload = b"\x80" * (width * height * (bit_count // 8))
    return header + payload


def build_tree(root):
    data = root / "Data"
    (data / "textures" / "arch").mkdir(parents=True)
    (data / "textures" / "loose").mkdir(parents=True)
    (data / "meshes").mkdir(parents=True)
    (data / "Video").mkdir(parents=True)
    (data / "Skyrim.esm").write_bytes(tes4_header())
    (root / "TESV.exe").write_bytes(b"MZ" + b"\x00" * 64)
    (data / "Video" / "BGS_Logo.bkr").write_bytes(b"BKR1" + b"\x00" * 32)
    bsa_write.write_bsa(
        str(data / "Skyrim - Textures.bsa"),
        [("textures\\arch\\keep_dxt1.dds", dds_file(8, 8, fourcc=b"DXT1")),
         ("textures\\arch\\plain_rgb.dds",
          dds_file(8, 8, bit_count=24, masks=(0xFF, 0xFF00, 0xFF0000, 0))),
         ("textures\\arch\\bad_bc5.dds", dds_file(8, 8, fourcc=b"BC5U"))],
        version=104, compressed=False)
    # Loose overrides the BSA copy of plain_rgb.dds and must be converted.
    (data / "textures" / "arch" / "plain_rgb.dds").write_bytes(
        dds_file(8, 8, bit_count=24, masks=(0xFF, 0xFF00, 0xFF0000, 0)))
    (data / "meshes" / "loose.nif").write_bytes(b"NIF0" + b"\x00" * 16)
    (data / "Skyrim.ini").write_text("[Archive]\n", encoding="utf-8")


def main():
    checks = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        build_tree(root)
        out = root / "stage"
        rc = deploy.main(["plan", "--data", str(root / "Data"),
                          "--tesv", str(root / "TESV.exe"),
                          "--json", str(root / "plan.json")])
        assert rc == 0, rc
        report = json.loads((root / "plan.json").read_text(encoding="utf-8"))
        assert report["schema"] == deploy.SCHEMA
        assert report["assets"]["textures"] == 3, report["assets"]
        actions = report["textures"]["actions"]
        assert actions.get("keep") == 1, actions
        assert actions.get("bc1") == 1, actions
        assert actions.get("refuse") == 1, actions
        assert report["refusals"][0]["path"].lower().endswith("bad_bc5.dds")
        checks += 4

        rc = deploy.main(["stage", "--data", str(root / "Data"),
                          "--tesv", str(root / "TESV.exe"),
                          "--out", str(out)])
        assert rc == 0, rc
        manifest = json.loads((out / "deploy_manifest.json").read_text(
            encoding="utf-8"))
        assert (out / "TESV.exe").is_file()
        assert (out / "Data" / "Skyrim.esm").is_file()
        assert (out / "Data" / "Skyrim - Textures.bsa").is_file()
        assert (out / "Data" / "meshes" / "loose.nif").is_file()
        assert (out / "Data" / "Video" / "BGS_Logo.bkr").is_file()
        checks += 4

        converted = out / "Data" / "textures" / "arch" / "plain_rgb.dds"
        assert converted.is_file(), converted
        info = dds.parse_dds(converted.read_bytes())
        assert info.width == 8 and info.height == 8, info.describe()
        assert info.kind == "bc1", info.format_name
        assert any(row["container"] == "dds-bc1"
                   for row in manifest["textures"]), manifest["textures"]
        assert any(row["path"].lower().endswith("bad_bc5.dds")
                   for row in manifest["refused"])
        checks += 3

        # The policy is a pure function; pin the NPOT and resize branches.
        npot = deploy.classify_texture(dds_file(24, 8, fourcc=b"DXT1"), 100, 0)
        assert npot["action"] == "rgba8" and "power-of-two" in npot["reason"]
        big = deploy.classify_texture(dds_file(1024, 1024, fourcc=b"DXT1"),
                                      1000, 256)
        assert big["action"] == "rgba8" and "downscale" in big["reason"]
        rgb = deploy.classify_texture(
            dds_file(8, 8, bit_count=24, masks=(0xFF, 0xFF00, 0xFF0000, 0)),
            100, 0)
        assert rgb["action"] == "bc1", rgb
        checks += 3

        # Cooked mode: winners only, no original BSA, INI points at cooked.
        cooked_out = root / "cooked"
        rc = deploy.main(["stage", "--data", str(root / "Data"),
                          "--tesv", str(root / "TESV.exe"),
                          "--out", str(cooked_out), "--pack", "cooked"])
        assert rc == 0, rc
        assert not (cooked_out / "Data" / "Skyrim - Textures.bsa").exists()
        archive_paths = sorted((cooked_out / "Data").glob("Skyrim - Cooked*.bsa"))
        assert archive_paths, "no cooked archive written"
        found = {}
        for path in archive_paths:
            with bsa.BsaArchive(str(path)) as archive:
                for entry in archive.list_files():
                    found[entry.path.lower()] = archive.read_file(entry.path)
        assert "textures\\arch\\keep_dxt1.dds" in found, sorted(found)
        assert "textures\\arch\\plain_rgb.dds" in found
        assert "textures\\arch\\bad_bc5.dds" not in found
        assert "meshes\\loose.nif" in found
        assert dds.parse_dds(found["textures\\arch\\plain_rgb.dds"]).kind == "bc1"
        ini = (cooked_out / "Data" / "Skyrim.ini").read_text(encoding="utf-8")
        assert "sResourceArchiveList=" in ini and "Skyrim - Cooked" in ini, ini
        cooked_manifest = json.loads(
            (cooked_out / "deploy_manifest.json").read_text(encoding="utf-8"))
        assert cooked_manifest["cooked"] is True
        assert any(row["path"].lower().endswith("bad_bc5.dds")
                   for row in cooked_manifest["refused"])
        checks += 8

        # The cooked stage writes a stream index next to the manifest; every
        # range it publishes is re-derived from the archive directory.
        index_path = cooked_out / "stream_index.bin"
        assert index_path.is_file(), index_path
        index = stream_index.read_index(str(index_path))
        report = cooked_manifest["stream_index"]
        assert report["resources"] == len(index.resources) == 3, report
        assert report["archives"] == len(index.archives) >= 1
        assert stream_index.verify_index(index, str(cooked_out / "Data")) == []
        file_sha = {row["path"].lower(): row["sha256"]
                    for row in cooked_manifest["files"]}
        for row in index.resources:
            archive_row = index.archives[row.archive]
            with bsa.BsaArchive(str(cooked_out / "Data" / archive_row.name)) as archive:
                entry = archive.find(row.path)
                assert entry is not None, row.path
                assert entry.offset == row.entry_offset
                assert entry.offset + entry.size == \
                    (row.physical_offset + row.payload_length)
                assert row.payload_length == len(archive.read_file(row.path))
                assert row.sha256 == file_sha[row.path]
        checks += 7

        # Same inputs produce byte-identical tables and generation.
        cooked_again = root / "cooked_again"
        rc = deploy.main(["stage", "--data", str(root / "Data"),
                          "--tesv", str(root / "TESV.exe"),
                          "--out", str(cooked_again), "--pack", "cooked"])
        assert rc == 0, rc
        assert (cooked_again / "stream_index.bin").read_bytes() == \
            index_path.read_bytes()
        again = json.loads((cooked_again / "deploy_manifest.json").read_text(
            encoding="utf-8"))
        assert again["stream_index"]["generation"] == report["generation"]
        checks += 3

        # The opt-out writes neither the file nor the manifest key.
        plain = root / "cooked_plain"
        rc = deploy.main(["stage", "--data", str(root / "Data"),
                          "--tesv", str(root / "TESV.exe"),
                          "--out", str(plain), "--pack", "cooked",
                          "--no-stream-index"])
        assert rc == 0, rc
        assert not (plain / "stream_index.bin").exists()
        plain_manifest = json.loads(
            (plain / "deploy_manifest.json").read_text(encoding="utf-8"))
        assert "stream_index" not in plain_manifest
        checks += 3

        rc = deploy.main(["plan", "--data", str(root / "Data"),
                          "--tesv", str(root / "TESV.exe"),
                          "--pack", "cooked", "--json", str(root / "cooked_plan.json")])
        assert rc == 0, rc
        cooked_plan = json.loads((root / "cooked_plan.json").read_text(
            encoding="utf-8"))
        assert cooked_plan["cooked"]["files"] == 3, cooked_plan["cooked"]
        assert cooked_plan["cooked"]["archives_estimate"] >= 1
        checks += 2

    print("deploy_device_test: %d checks OK" % checks)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
