"""Synthetic tests for the mod layer (no game assets required).

Run: python -m unittest tests.mod_compat_test -v
"""

from __future__ import annotations

import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from skyrim import bsa, bsa_write, data_overlay, dds  # noqa: E402
import mod_optimize  # noqa: E402
import mod_scan  # noqa: E402


def osp(*parts):
    """Host path from components (the test host may be POSIX or Windows)."""
    return os.path.join(*parts)


def slurp(path):
    with open(path, "rb") as handle:
        return handle.read()


def spit(path, data):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)


def write_text(path, text):
    with open(path, "w") as handle:
        handle.write(text)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


def write_plugin(path: str, masters=(), *, is_esl: bool = False,
                 form_version: int = 40, body: bytes = b"\x00" * 32) -> str:
    subs = bytearray()
    subs += b"HEDR" + struct.pack("<H", 12) + struct.pack("<fiI", 1.7, 7, 0)
    subs += b"CNAM" + struct.pack("<H", 5) + b"test\x00"
    for master in masters:
        raw = master.encode("cp1252") + b"\x00"
        subs += b"MAST" + struct.pack("<H", len(raw)) + raw
        subs += b"DATA" + struct.pack("<H", 8) + struct.pack("<Q", 0)
    payload = bytes(subs)
    if body:
        payload += b"AAAA" + struct.pack("<H", len(body)) + body
    flags = 0x1 if path.lower().endswith(".esm") else 0
    if is_esl:
        flags |= 0x200
    header = struct.pack("<4sIIIHHHH", b"TES4", len(payload), flags, 0,
                         0, 0, form_version, 0)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(header + payload)
    return path


def write_rgba_dds(path: str, width: int, height: int,
                   pixels=None) -> str:
    if pixels is None:
        pixels = bytearray()
        for y in range(height):
            for x in range(width):
                pixels += bytes((x * 255 // max(1, width - 1),
                                 y * 255 // max(1, height - 1), 96, 255))
    header = struct.pack("<4s7I", b"DDS ", 124,
                         0x1 | 0x2 | 0x4 | 0x8 | 0x1000,
                         height, width, width * 4, 0, 1)
    header += b"\x00" * 44
    header += struct.pack("<8I", 32, 0x41, 0, 32,
                          0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(header + bytes(pixels))
    return path


class FixtureCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="modcompat_")
        self.root = self.tmp.name
        self.data = os.path.join(self.root, "Data")
        os.makedirs(self.data)

    def tearDown(self):
        self.tmp.cleanup()

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    def plugin(self, name, masters=(), **kwargs):
        return write_plugin(os.path.join(self.data, name), masters, **kwargs)

    def archive(self, name, files, **kwargs):
        return bsa_write.write_bsa(os.path.join(self.data, name), files, **kwargs)


# ---------------------------------------------------------------------------
# BSA
# ---------------------------------------------------------------------------


class BsaTests(FixtureCase):
    def test_writer_reader_roundtrip(self):
        path = self.path("roundtrip.bsa")
        report = bsa_write.write_bsa(path, {
            "textures\\a\\one.dds": b"one" * 100,
            "meshes\\two.nif": b"two" * 50,
            "meshes\\sub\\three.nif": b"three",
        }, compressed=True, embed_names=True)
        self.assertEqual(report["folders"], 3)
        with bsa.BsaArchive(path) as archive:
            self.assertEqual(archive.header.version, 104)
            self.assertTrue(archive.header.compressed)
            self.assertEqual(len(archive.list_files()), 3)
            self.assertEqual(archive.read_file("TEXTURES/A/ONE.DDS"),
                             b"one" * 100)
            self.assertEqual(archive.read_file("meshes\\sub\\three.nif"),
                             b"three")

    def test_v103_header_accepted(self):
        path = self.path("v103.bsa")
        bsa_write.write_bsa(path, {"interface\\a.txt": b"hello"},
                            compressed=False)
        raw = bytearray(slurp(path))
        struct.pack_into("<I", raw, 4, 103)
        struct.pack_into("<I", raw, 12, 0x703)  # bits v104 calls XMem
        spit(path, raw)
        with bsa.BsaArchive(path) as archive:
            self.assertEqual(archive.header.version, 103)
            self.assertEqual(archive.read_file("Interface\\a.txt"), b"hello")

    def test_bad_version_is_named(self):
        path = self.path("v999.bsa")
        bsa_write.write_bsa(path, {"a.txt": b"x"}, compressed=False)
        raw = bytearray(slurp(path))
        struct.pack_into("<I", raw, 4, 999)
        spit(path, raw)
        with self.assertRaises(bsa.BsaFormatError) as caught:
            bsa.BsaArchive(path)
        self.assertIn("unsupported BSA version", str(caught.exception))


# ---------------------------------------------------------------------------
# plugins.txt and load order
# ---------------------------------------------------------------------------


class LoadOrderTests(FixtureCase):
    def test_parse_stars_and_comments(self):
        entries = data_overlay.parse_plugin_list_text(
            "\ufeff# comment\n*Skyrim.esm\nSkyrim - Update.esp\n*Mod.esp\n")
        self.assertEqual([(e.name, e.active) for e in entries],
                         [("Skyrim.esm", True), ("Skyrim - Update.esp", False),
                          ("Mod.esp", True)])

    def test_no_star_list_is_all_active(self):
        entries = data_overlay.parse_plugin_list_text(
            "Skyrim.esm\nMod.esp\n", explicit_active=True)
        self.assertTrue(all(entry.active for entry in entries))

    def test_duplicate_and_path_rejected(self):
        with self.assertRaises(data_overlay.PluginListError):
            data_overlay.parse_plugin_list_text("*A.esp\n*A.esp\n")
        with self.assertRaises(data_overlay.PluginListError):
            data_overlay.parse_plugin_list_text("*Sub\\A.esp\n")

    def test_masters_forced_before_dependent(self):
        self.plugin("Skyrim.esm")
        self.plugin("A.esp", ["Skyrim.esm"])
        self.plugin("B.esp", ["A.esp", "Skyrim.esm"])
        plugins = self.path("plugins.txt")
        open(plugins, "w").write("*B.esp\n*A.esp\n")
        overlay = data_overlay.DataOverlay(self.data, plugins_path=plugins,
                                           auto_discover=False)
        self.assertEqual([p.name for p in overlay.load_order],
                         ["Skyrim.esm", "A.esp", "B.esp"])
        overlay.close()

    def test_missing_master_is_named(self):
        self.plugin("Skyrim.esm")
        self.plugin("A.esp", ["Skyrim.esm", "Ghost.esm"])
        plugins = self.path("plugins.txt")
        open(plugins, "w").write("*A.esp\n")
        with self.assertRaises(data_overlay.MissingMasterError) as caught:
            data_overlay.DataOverlay(self.data, plugins_path=plugins,
                                     auto_discover=False)
        self.assertEqual(caught.exception.master, "Ghost.esm")
        self.assertEqual(caught.exception.plugin, "A.esp")
        self.assertEqual(caught.exception.code, "missing-master")

    def test_master_present_but_not_starred_is_auto_activated(self):
        self.plugin("Skyrim.esm")
        self.plugin("Master.esm")
        self.plugin("A.esp", ["Master.esm"])
        plugins = self.path("plugins.txt")
        with open(plugins, "w") as handle:
            handle.write("*A.esp\n")
        overlay = data_overlay.DataOverlay(self.data, plugins_path=plugins,
                                           auto_discover=False)
        try:
            self.assertEqual([p.name for p in overlay.load_order],
                             ["Master.esm", "A.esp"])
        finally:
            overlay.close()

    def test_cycle_is_rejected(self):
        self.plugin("Skyrim.esm")
        self.plugin("A.esm")
        self.plugin("B.esp", ["A.esm"])
        self.plugin("C.esp", ["B.esp"])
        self.plugin("D.esp", ["C.esp"])
        # Rebuild A with a dependency on D to close the cycle.
        write_plugin(os.path.join(self.data, "A.esm"), ["D.esp"])
        plugins = self.path("plugins.txt")
        open(plugins, "w").write("*D.esp\n")
        with self.assertRaises(data_overlay.PluginOrderError):
            data_overlay.DataOverlay(self.data, plugins_path=plugins,
                                     auto_discover=False)

    def test_env_data_root(self):
        self.plugin("Skyrim.esm")
        saved = os.environ.get("TESV_DATA_ROOT")
        os.environ["TESV_DATA_ROOT"] = self.data
        try:
            self.assertEqual(data_overlay.resolve_data_root(), self.data)
        finally:
            if saved is None:
                os.environ.pop("TESV_DATA_ROOT", None)
            else:
                os.environ["TESV_DATA_ROOT"] = saved


# ---------------------------------------------------------------------------
# Overlay precedence
# ---------------------------------------------------------------------------


class OverlayTests(FixtureCase):
    def test_loose_overrides_bsa_and_later_plugin_wins(self):
        self.plugin("Skyrim.esm")
        self.plugin("A.esp", ["Skyrim.esm"])
        self.plugin("B.esp", ["Skyrim.esm"])
        self.archive("Skyrim - Textures.bsa", {"textures\\x.dds": b"vanilla"})
        self.archive("A.bsa", {"textures\\x.dds": b"mod-a"})
        self.archive("B.bsa", {"textures\\x.dds": b"mod-b"})
        plugins = self.path("plugins.txt")
        open(plugins, "w").write("*A.esp\n*B.esp\n")
        overlay = data_overlay.DataOverlay(self.data, plugins_path=plugins,
                                           auto_discover=False)
        try:
            hit = overlay.resolve("textures/x.dds")
            self.assertEqual(hit.kind, "bsa")
            self.assertEqual(hit.archive, "B.bsa")
            self.assertEqual(overlay._open(overlay.archives[-1]).read_file(
                "textures\\x.dds"), b"mod-b")
            loose = os.path.join(self.data, "textures")
            os.makedirs(loose)
            open(os.path.join(loose, "X.DDS"), "wb").write(b"loose")
            hit = overlay.resolve("Textures\\x.dds")
            self.assertEqual(hit.kind, "loose")
            self.assertIn("X.DDS", hit.host_path)
        finally:
            overlay.close()

    def test_update_bsa_mounts_last(self):
        self.plugin("Skyrim.esm")
        self.plugin("Update.esm")
        self.archive("Skyrim - Textures.bsa", {"textures\\y.dds": b"old"})
        self.archive("Update.bsa", {"textures\\y.dds": b"new"})
        overlay = data_overlay.DataOverlay(self.data, auto_discover=False)
        try:
            names = [m.name for m in overlay.archives]
            self.assertEqual(names[-1], "Update.bsa")
            hit = overlay.resolve("textures\\y.dds")
            self.assertEqual(hit.archive, "Update.bsa")
        finally:
            overlay.close()

    def test_missing_plugin_is_named(self):
        self.plugin("Skyrim.esm")
        plugins = self.path("plugins.txt")
        open(plugins, "w").write("*Skyrim.esm\n*Ghost.esp\n")
        with self.assertRaises(data_overlay.MissingPluginError) as caught:
            data_overlay.DataOverlay(self.data, plugins_path=plugins,
                                     auto_discover=False)
        self.assertIn("Ghost.esp", str(caught.exception))

    def test_default_vanilla_path_without_lists(self):
        self.plugin("Skyrim.esm")
        self.plugin("Update.esm")
        self.archive("Update.bsa", {"textures\\z.dds": b"z"})
        overlay = data_overlay.DataOverlay(self.data, auto_discover=False)
        try:
            self.assertEqual([p.name for p in overlay.load_order],
                             ["Skyrim.esm", "Update.esm"])
            self.assertEqual(overlay.resolve("textures\\z.dds").archive,
                             "Update.bsa")
        finally:
            overlay.close()


# ---------------------------------------------------------------------------
# Scanner and optimizer
# ---------------------------------------------------------------------------


class ScanTests(FixtureCase):
    def test_tier3_skse_and_esl(self):
        plain = self.plugin("Plain.esp", body=b"VMAD" * 2)
        self.assertEqual(mod_scan.scan_plugin(plain)["tier"], 1)
        skse = self.plugin("Skse.esp", body=b"SKSE" + b"\x00" * 16)
        self.assertEqual(mod_scan.scan_plugin(skse)["tier"], 3)
        esl = self.plugin("Light.esl", is_esl=True)
        self.assertEqual(mod_scan.scan_plugin(esl)["tier"], 3)
        se = self.plugin("Se.esp", form_version=43)
        self.assertEqual(mod_scan.scan_plugin(se)["tier"], 3)

    def test_tier2_script_heavy(self):
        heavy = self.plugin("Scripts.esp", body=b"VMAD" * 120)
        result = mod_scan.scan_plugin(heavy)
        self.assertEqual(result["tier"], 2)
        self.assertGreaterEqual(result["vmad_count"], 120)

    def test_texture_and_bsa_scan(self):
        source = write_rgba_dds(osp(self.data, "textures", "big.dds"), 1024, 1024)
        self.archive("Mod.bsa", {"textures\\big.dds": open(source, "rb").read()})
        report = mod_scan.scan_bsa(os.path.join(self.data, "Mod.bsa"))
        textures = report["textures"]
        self.assertEqual(textures["count"], 1)
        self.assertEqual(textures["buckets"].get("1K"), 1)
        self.assertGreater(textures["vram_etc1_cap"], 0)

    def test_scan_mod_reports_budget(self):
        self.plugin("Skyrim.esm")
        source = write_rgba_dds(osp(self.data, "textures", "t.dds"), 512, 512)
        self.archive("Skyrim - Textures.bsa",
                     {"textures\\t.dds": open(source, "rb").read()})
        report = mod_scan.scan_mod(self.data, auto_discover=False)
        self.assertEqual(report["tier"], 1)
        self.assertIn("vram_etc1_bytes_at_cap", report["budget"])
        self.assertTrue(any(p["name"] == "Skyrim.esm"
                            for p in report["plugins"]))


class OptimizeTests(FixtureCase):
    def test_downscale_etc1_roundtrip(self):
        src = write_rgba_dds(self.path("in.dds"), 512, 256)
        data = open(src, "rb").read()
        before = data[:]
        result = mod_optimize.optimize_texture(
            data, "textures\\in.dds", self.path("out"),
            mod_optimize.TexturePolicy(max_dim=128, fmt="etc1", mips=True))
        self.assertTrue(result["downscaled"])
        self.assertEqual((result["target"]["width"],
                          result["target"]["height"]), (128, 64))
        self.assertEqual(data, before, "optimizer must not touch the source")
        blob = open(result["output"], "rb").read()
        parsed = mod_optimize.etc1.read_sktx(blob)
        self.assertEqual(parsed["format"], "etc1")
        self.assertEqual(parsed["width"], 128)
        # Decode the first tiled level and compare roughly with the source.
        level = parsed["levels"][0]
        decoded = mod_optimize.etc1.decode_etc1_image(
            blob[level["offset"]:level["offset"] + level["bytes"]], 128, 64)
        expected = dds.decode_dds(data, 0)[2]
        expected = mod_optimize.resize_rgba(expected, 512, 256, 128, 64)
        score = mod_optimize.etc1.psnr_rgb(decoded, expected, 128, 64)
        self.assertGreater(score["psnr_db"], 20.0)

    def test_bc1_output_is_valid_dds(self):
        src = write_rgba_dds(self.path("in2.dds"), 64, 64)
        data = open(src, "rb").read()
        result = mod_optimize.optimize_texture(
            data, "x.dds", self.path("out2"),
            mod_optimize.TexturePolicy(max_dim=32, fmt="bc1", mips=False))
        blob = open(result["output"], "rb").read()
        info = dds.parse_dds(blob)
        self.assertEqual((info.width, info.height), (32, 32))
        self.assertEqual(info.format_name, "DXT1")
        self.assertEqual(info.mip_count, 1)

    def test_tree_reports_errors_without_aborting(self):
        write_rgba_dds(osp(self.path("tree"), "ok.dds"), 128, 128)
        open(osp(self.path("tree"), "bad.dds"), "wb").write(b"nope")
        report = mod_optimize.optimize_tree(self.path("tree"),
                                            self.path("tree_out"),
                                            mod_optimize.TexturePolicy(max_dim=64))
        self.assertEqual(len(report["textures"]), 1)
        self.assertEqual(len(report["errors"]), 1)

    def test_pack_bsa_roundtrip(self):
        source = write_rgba_dds(osp(self.path("pack"), "textures", "p.dds"),
                                32, 32)
        out = self.path("packed.bsa")
        report = mod_optimize.pack_bsa(out, [source])
        self.assertEqual(report["version"], 104)
        with bsa.BsaArchive(out) as archive:
            self.assertEqual(len(archive.list_files()), 1)
            self.assertEqual(sorted(e.path for e in archive.list_files()),
                             ["p.dds"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
