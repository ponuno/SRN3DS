#!/usr/bin/env python3
"""Synthetic, bounded-memory tests for the offline menu preprocessors.

Run from the repository root:
    .venv\\Scripts\\python.exe tests\\menu_preprocess_test.py

No retail bytes are embedded.  The optional retail sample lives in
``tools/menu_preprocess/sample.py`` and is not required by this suite.
"""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from menu_preprocess.common import (  # noqa: E402
    BudgetError,
    ContractError,
    DataError,
    Limits,
    load_json,
    parse_json,
)
from menu_preprocess.glyph import validate_glyph  # noqa: E402
from menu_preprocess.material import validate_material  # noqa: E402
from menu_preprocess.normal import NormalPolicy, preprocess_normal  # noqa: E402
from menu_preprocess.signatures import (  # noqa: E402
    SIGNATURES,
    decode_normal_byte,
    decode_normal_components,
    probe_bytecode,
)
from skyrim import dds  # noqa: E402


def rgba_dds(width: int, height: int, pixels: bytes,
             levels: list[tuple[int, int, bytes]] | None = None) -> bytes:
    payloads = levels or [(width, height, pixels)]
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000
    caps = 0x1000
    mips = len(payloads)
    if mips > 1:
        flags |= 0x20000
        caps |= 0x8 | 0x400000
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, flags, height, width,
                     width * 4, 0, mips)
    struct.pack_into("<8I", header, 76, 32, 0x41, 0, 32,
                     0xFF, 0xFF00, 0xFF0000, 0xFF000000)
    struct.pack_into("<5I", header, 108, caps, 0, 0, 0, 0)
    return bytes(header) + b"".join(payload for _w, _h, payload in payloads)


def bc5_snorm_header(width: int = 4, height: int = 4) -> bytes:
    header = bytearray(148)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x1007, height, width,
                     ((width + 3) // 4) * ((height + 3) // 4) * 16, 0, 1)
    struct.pack_into("<8I", header, 76, 32, 0x4, int.from_bytes(b"DX10", "little"),
                     0, 0, 0, 0, 0)
    struct.pack_into("<5I", header, 108, 0x1000, 0, 0, 0, 0)
    struct.pack_into("<5I", header, 128, 84, 3, 0, 1, 0)  # BC5_SNORM
    return bytes(header)


def bc2_header(fourcc: bytes = b"DXT2") -> bytes:
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x1007, 4, 4, 16, 0, 1)
    struct.pack_into("<8I", header, 76, 32, 0x4, int.from_bytes(fourcc, "little"),
                     0, 0, 0, 0, 0)
    struct.pack_into("<5I", header, 108, 0x1000, 0, 0, 0, 0)
    return bytes(header) + b"\0" * 16


def normal_pixels(layout: str = "rgba_xzyw") -> bytes:
    # Raw XYZ byte triples; all spans are non-zero and the mean is balanced.
    triples = ((192, 192, 128), (128, 128, 128), (64, 64, 128),
               (160, 160, 160), (96, 96, 96), (128, 192, 64),
               (128, 64, 192), (224, 128, 128), (32, 128, 128),
               (128, 128, 224), (128, 128, 32), (200, 56, 200),
               (56, 200, 56), (72, 184, 184), (184, 72, 184),
               (128, 128, 128))
    out = bytearray()
    for index, (x, y, z) in enumerate(triples):
        if layout == "rgba_xzyw":
            out += bytes((x, z, 33 + index, y))
        else:
            out += bytes((x, y, z, 255))
    return bytes(out)


def write_basis(path: Path, rows: tuple[tuple[float, ...], ...],
                count: int = 2) -> None:
    data = bytearray(struct.pack("<4sIII", b"MVB1", 1, count, 36))
    tangent, bitangent, normal = rows
    for _ in range(count):
        data += struct.pack("<9f", *tangent, *bitangent, *normal)
    path.write_bytes(data)


def write_glyph_vertices(path: Path, quads: int = 1) -> None:
    records = []
    for index in range(quads):
        x0 = -0.8 + index * 0.1
        x1 = x0 + 0.4
        y0, y1 = -0.5, 0.5
        corners = ((x0, y0, 0.0, 1.0, 0.0, 0.0),
                   (x1, y0, 0.0, 1.0, 1.0, 0.0),
                   (x0, y1, 0.0, 1.0, 0.0, 1.0),
                   (x1, y1, 0.0, 1.0, 1.0, 1.0))
        for corner in (0, 1, 2, 2, 1, 3):
            records.append(struct.pack("<4f2f4B", *corners[corner], 255, 255, 255, 255))
    path.write_bytes(struct.pack("<4sIII", b"MGV1", 1, len(records), 28)
                     + b"".join(records))


def dr(reg_type: int, index: int, mask: int = 0xF, mod: int = 0) -> int:
    return (0x80000000 | ((reg_type & 7) << 28) | ((reg_type & 0x18) << 8)
            | (index & 0x7FF) | ((mask & 0xF) << 16) | ((mod & 0xF) << 20))


def sr(reg_type: int, index: int, swizzle: int = 0xE4, mod: int = 0) -> int:
    return (0x80000000 | ((reg_type & 7) << 28) | ((reg_type & 0x18) << 8)
            | (index & 0x7FF) | ((swizzle & 0xFF) << 16) | ((mod & 0xF) << 24))


def fbits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def probe_program(add_normal: bool, *, c8=None, swizzle: int = 0xD8) -> bytes:
    words = [0xFFFF0300]
    words += [0x05000051, dr(2, 8)]
    literal = c8 if c8 is not None else (
        (-0.5, 1.0, 0.0, 0.0) if add_normal else (2.0, -1.0, 1.0, 0.0)
    )
    words += [fbits(value) for value in literal]
    for usage, index, reg_type, reg, tex_type in (
        (5, 0, 1, 0, 0), (10, 0, 1, 1, 0), (10, 1, 1, 2, 0),
        (0, 0, 10, 0, 2), (0, 0, 10, 1, 2),
    ):
        words += [0x0200001F, 0x80000000 | usage | (index << 16) | (tex_type << 27),
                  dr(reg_type, reg)]
    words += [0x03000042, dr(0, 1), sr(1, 0), sr(10, 1)]
    if add_normal:
        words += [0x03000002, dr(0, 1, 7), sr(0, 1, swizzle), sr(2, 8, 0x00)]
    else:
        words += [0x04000004, dr(0, 1, 7), sr(0, 1, swizzle),
                  sr(2, 8, 0x00), sr(2, 8, 0x55)]
    instruction_count = 8
    while instruction_count < (25 if add_normal else 24):
        words.append(0x00000000)  # nop
        instruction_count += 1
    words.append(0x0000FFFF)
    return struct.pack("<%dI" % len(words), *words)


class SignatureTests(unittest.TestCase):
    def test_decode_formulas_and_channel_order(self) -> None:
        # Retail physical channels are R=X, G=Z, A=Y; B is ignored.
        self.assertEqual(decode_normal_byte(
            SIGNATURES["p5_MAD_NORMAL"], 192, 64, 246, 160), (192, 160, 64, 255))
        self.assertEqual(decode_normal_components(
            SIGNATURES["p1200005_ADD_NORMAL"], 192, 160, 64), (192, 160, 64, 255))
        for value in range(256):
            p5 = decode_normal_components(
                SIGNATURES["p5_MAD_NORMAL"], value, 128, 255 - value)
            p120 = decode_normal_components(
                SIGNATURES["p1200005_ADD_NORMAL"], value, 128, 255 - value)
            self.assertEqual(p5, p120)

    def test_partial_bytecode_probe(self) -> None:
        for add_normal, name in ((False, "p5_MAD_NORMAL"),
                                 (True, "p1200005_ADD_NORMAL")):
            report = probe_bytecode(probe_program(add_normal))
            self.assertEqual(report["signature"], name)
            self.assertEqual(report["instructions"], 24 + int(add_normal))
        with self.assertRaisesRegex(DataError, "def c8|normal decode"):
            probe_bytecode(probe_program(False, c8=(1.5, -1.0, 1.0, 0.0)))
        with self.assertRaisesRegex(DataError, "normal decode"):
            probe_bytecode(probe_program(False, swizzle=0xE4))


class NormalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "source.dds"
        self.output = self.root / "normal.dds"
        self.report = self.root / "normal.json"
        self.source.write_bytes(rgba_dds(4, 4, normal_pixels()))

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_both_signatures_produce_xyz_and_full_mip_chain(self) -> None:
        source = rgba_dds(4, 4, normal_pixels(),
                          [(4, 4, normal_pixels()),
                           (2, 2, normal_pixels()[:16])])
        self.source.write_bytes(source)
        reports = []
        for signature in ("p5_MAD_NORMAL", "p1200005_ADD_NORMAL"):
            output = self.root / (signature + ".dds")
            report = preprocess_normal(self.source, signature=signature,
                                       output_path=output, report_path=self.report)
            reports.append(report)
            self.assertEqual(report["status"], "pass")
            self.assertEqual(report["validation"]["channel_order"]["selected"],
                             "rgba_xzyw")
            self.assertEqual(report["output"]["mip_count"], 2)
            self.assertEqual(report["output"]["kind"], "rgba8_xyz_opaque")
            self.assertLessEqual(report["memory"]["peak_bytes"],
                                 report["limits"]["max_memory_bytes"])
            width, height, pixels = dds.decode_dds(output.read_bytes())
            self.assertEqual((width, height), (4, 4))
            self.assertEqual(pixels[:4], bytes((192, 192, 128, 255)))
            self.assertTrue(all(pixels[pos + 3] == 255
                                for pos in range(0, len(pixels), 4)))
            loaded = load_json(self.report, Limits(), "normal report")
            self.assertEqual(loaded["output"]["sha256"], report["output"]["sha256"])
        self.assertEqual(reports[0]["output"]["sha256"],
                         reports[1]["output"]["sha256"])

    def test_validation_only(self) -> None:
        report = preprocess_normal(self.source, signature="p5_MAD_NORMAL")
        self.assertIsNone(report["output"])
        self.assertFalse(self.output.exists())

    def test_wrong_channel_order_is_explicit(self) -> None:
        conventional = self.root / "conventional.dds"
        conventional.write_bytes(rgba_dds(4, 4, normal_pixels("rgba_xyzw")))
        with self.assertRaisesRegex(ContractError, "require rgba_xzyw"):
            preprocess_normal(conventional, signature="p5_MAD_NORMAL",
                              output_path=self.output)
        self.assertFalse(self.output.exists())
        with self.assertRaisesRegex(ContractError, "contradiction"):
            preprocess_normal(conventional, signature="p5_MAD_NORMAL",
                              policy=NormalPolicy(source_layout="rgba_xzyw"))
        report = preprocess_normal(
            conventional, signature="p5_MAD_NORMAL", output_path=self.output,
            policy=NormalPolicy(source_layout="rgba_xyzw",
                                allow_layout_conversion=True),
        )
        self.assertTrue(report["decode"]["layout_conversion"])

    def test_snorm_range_is_rejected_before_decode(self) -> None:
        bad = self.root / "snorm.dds"
        bad.write_bytes(bc5_snorm_header())
        with self.assertRaisesRegex(DataError, "SNORM"):
            preprocess_normal(bad, signature="p5_MAD_NORMAL",
                              output_path=self.output)
        self.assertFalse(self.output.exists())

    def test_strict_statistical_range_gate_and_default_warning(self) -> None:
        biased = bytearray()
        for index in range(16):
            biased += bytes((208 + index, 224 + (index % 16), 0,
                             216 + (index % 16)))  # all decoded components positive
        self.source.write_bytes(rgba_dds(4, 4, bytes(biased)))
        report = preprocess_normal(self.source, signature="p5_MAD_NORMAL")
        self.assertTrue(report["validation"]["warnings"])
        with self.assertRaisesRegex(ContractError, "strict-range"):
            preprocess_normal(
                self.source, signature="p5_MAD_NORMAL", output_path=self.output,
                policy=NormalPolicy(max_mean_normal_length=0.5),
            )
        self.assertFalse(self.output.exists())

    def test_constant_channel_and_truncated_mips_fail(self) -> None:
        constant = bytearray(normal_pixels())
        for pos in range(0, len(constant), 4):
            constant[pos] = 128
            constant[pos + 1] = 128
            constant[pos + 3] = 128
        self.source.write_bytes(rgba_dds(4, 4, bytes(constant)))
        with self.assertRaisesRegex(ContractError, "constant"):
            preprocess_normal(self.source, signature="p5_MAD_NORMAL")

        full = rgba_dds(4, 4, normal_pixels(),
                        [(4, 4, normal_pixels()), (2, 2, normal_pixels()[:16])])
        self.source.write_bytes(full[:-1])
        with self.assertRaisesRegex(DataError, "truncated"):
            preprocess_normal(self.source, signature="p5_MAD_NORMAL",
                              output_path=self.output)

    def test_constant_component_is_a_warning_not_an_automatic_failure(self) -> None:
        pixels = bytearray(normal_pixels())
        for pos in range(1, len(pixels), 4):
            pixels[pos] = 128  # constant Z is valid for an axis-aligned flat
        self.source.write_bytes(rgba_dds(4, 4, bytes(pixels)))
        report = preprocess_normal(self.source, signature="p5_MAD_NORMAL")
        self.assertTrue(any("constant normal component" in warning
                            for warning in report["validation"]["warnings"]))

    def test_bounded_memory_failure_writes_no_output(self) -> None:
        with self.assertRaisesRegex(ContractError, "max_memory_bytes"):
            preprocess_normal(self.source, signature="p5_MAD_NORMAL",
                              output_path=self.output, report_path=self.report,
                              limits=Limits(max_memory_bytes=1024))
        self.assertFalse(self.output.exists())
        failure = load_json(self.report, Limits(), "failure report")
        self.assertEqual(failure["status"], "fail")
        self.assertIsNone(failure["output"])

    def test_pixel_budget_failure_also_writes_a_failure_report(self) -> None:
        with self.assertRaises(BudgetError):
            preprocess_normal(self.source, signature="p5_MAD_NORMAL",
                              output_path=self.output, report_path=self.report,
                              limits=Limits(max_pixels=1))
        self.assertFalse(self.output.exists())
        self.assertEqual(load_json(self.report, Limits(), "budget failure")["status"],
                         "fail")

    def test_implausible_header_fails_before_payload_allocation(self) -> None:
        huge = bytearray(128)
        huge[:4] = b"DDS "
        struct.pack_into("<7I", huge, 4, 124, 0x1007, 32768, 32768,
                         32768 * 4, 0, 1)
        struct.pack_into("<8I", huge, 76, 32, 0x41, 0, 32,
                         0xFF, 0xFF00, 0xFF0000, 0xFF000000)
        struct.pack_into("<5I", huge, 108, 0x1000, 0, 0, 0, 0)
        self.source.write_bytes(huge)
        with self.assertRaises(BudgetError):
            preprocess_normal(self.source, signature="p5_MAD_NORMAL")

    def test_input_output_must_be_distinct(self) -> None:
        with self.assertRaisesRegex(ContractError, "distinct"):
            preprocess_normal(self.source, signature="p5_MAD_NORMAL",
                              output_path=self.source)


class MenuFixture:
    def __init__(self, root: Path):
        self.root = root
        self.normal_source = root / "normal_source.dds"
        self.normal_output = root / "normal_output.dds"
        self.normal_manifest = root / "normal.json"
        self.diffuse = root / "diffuse.dds"
        self.basis = root / "basis.mvb1"
        self.descriptor = root / "material.json"
        self.normal_source.write_bytes(rgba_dds(4, 4, normal_pixels()))
        preprocess_normal(self.normal_source, signature="p5_MAD_NORMAL",
                          output_path=self.normal_output,
                          report_path=self.normal_manifest)
        self.diffuse.write_bytes(rgba_dds(4, 4, b"\xff\x00\x00\xff" * 16))
        write_basis(self.basis, ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                                 (0.0, 0.0, 1.0)))
        self.data = {
            "schema_version": 1,
            "pipeline": "static.p5_mad_normal",
            "signature": "p5_MAD_NORMAL",
            "constants": {
                "c0": [1.0, 0.0, 0.0, 0.0],
                "c1": [0.0, 1.0, 0.0, 0.0],
                "c2": [0.0, 0.0, 1.0, 0.0],
                "c3": [0.0, 0.0, 1.0, 0.0],
                "c4": [1.0, 1.0, 1.0, 1.0],
                "c5": [0.0, 0.0, 0.5, 0.0],
                "c6": [0.1, 0.2, 0.3, 0.0],
                "c7": [1.0, 1.0, 1.0, 0.25],
            },
            "ctab": {
                "DiffuseSampler": {"set": 3, "index": 0},
                "NormalSampler": {"set": 3, "index": 1},
                "DirLightDirection": {"set": 2, "index": 3},
                "DirLightColor": {"set": 2, "index": 4},
                "MaterialData": {"set": 2, "index": 5},
                "EmitColor": {"set": 2, "index": 6},
                "FogColor": {"set": 2, "index": 7},
                "DirectionalAmbient": {"set": 2, "index": 0,
                                       "disposition": "unused_shadowed_by_basis"},
            },
            "ambient_rgb": [0.05, 0.1, 0.15],
            "normal_map": self.normal_manifest.name,
            "diffuse_texture": self.diffuse.name,
            "vertex_declaration": {
                "position": {"components": 4, "space": "clip"},
                "texcoord0": {"components": 2},
                "color0": {"components": 4, "encoding": "rgba8_unorm"},
                "color1": {"components": 4, "encoding": "rgba8_unorm",
                           "rgb": "ambient_plus_emissive"},
                "normal": {"components": 3, "space": "lighting"},
                "tangent": {"components": 3, "space": "lighting"},
                "bitangent": {"components": 3, "space": "lighting", "source": "stored"},
            },
            "basis": {"scope": "constant", "handedness": "right",
                      "records": self.basis.name},
            "preconditions": {
                "normal_decoded": True,
                "basis_prebaked": True,
                "secondary_rgb": "ambient_plus_emissive",
                "material_alpha_register": "c4",
                "fog_rgb_register": "c7",
                "fog_factor_component": "a",
            },
        }

    def save(self) -> Path:
        self.descriptor.write_text(json.dumps(self.data), encoding="utf-8")
        return self.descriptor


class MaterialTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.fixture = MenuFixture(self.root)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def validate(self):
        return validate_material(self.fixture.save())

    def test_complete_contract(self) -> None:
        report = self.validate()
        self.assertEqual(report["status"], "pass")
        self.assertFalse(report["runtime_integrated"])
        self.assertEqual(report["future_loader_inputs"]["tev_constants_rgba8"]["c4"],
                         [255, 255, 255, 128])
        self.assertEqual(report["future_loader_inputs"]["color1_rgb"],
                         [0.15, 0.3, 0.45])

    def test_secondary_colour_is_explicitly_clamped_for_rgba8(self) -> None:
        self.fixture.data["ambient_rgb"] = [1.0, 1.0, 1.0]
        report = self.validate()
        inputs = report["future_loader_inputs"]
        self.assertEqual(inputs["color1_rgb"], [1.0, 1.0, 1.0])
        self.assertTrue(inputs["color1_rgb_clamped"])
        self.assertEqual(inputs["color1_rgb_unclamped"], [1.1, 1.2, 1.3])

    def test_missing_extra_and_contradictory_constants(self) -> None:
        del self.fixture.data["constants"]["c6"]
        with self.assertRaisesRegex(ContractError, "missing c6"):
            self.validate()
        self.fixture.data["constants"]["c6"] = [0.1, 0.2, 0.3, 0.0]
        self.fixture.data["constants"]["c8"] = [2.0, -1.0, 1.0, 0.0]
        with self.assertRaisesRegex(ContractError, "c8"):
            self.validate()
        del self.fixture.data["constants"]["c8"]
        self.fixture.data["constants"]["c0"] = [0.2, 0.3, 0.4, 0.0]
        with self.assertRaisesRegex(ContractError, "unit length|orthogonal"):
            self.validate()

    def test_ctab_and_prebaked_range_contradictions(self) -> None:
        self.fixture.data["ctab"]["DirLightDirection"]["index"] = 4
        with self.assertRaisesRegex(ContractError, "expected set 2 index 3"):
            self.validate()
        self.fixture.data["ctab"]["DirLightDirection"]["index"] = 3
        self.fixture.data["ctab"]["DirectionalAmbient"].pop("disposition")
        with self.assertRaisesRegex(ContractError, "unused_shadowed_by_basis"):
            self.validate()
        self.fixture.data["ctab"]["DirectionalAmbient"]["disposition"] = "unused_shadowed_by_basis"
        self.fixture.data["constants"]["c3"] = [0.0, 0.0, -1.0, 0.0]
        with self.assertRaisesRegex(ContractError, "positive RGBA8 UNORM"):
            self.validate()

    def test_vertex_basis_must_match_constant_right_handed_rows(self) -> None:
        write_basis(self.fixture.basis, ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                                         (0.0, 0.0, -1.0)))
        with self.assertRaisesRegex(ContractError, "determinant|contradicts"):
            self.validate()
        write_basis(self.fixture.basis, ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                                         (0.0, 0.0, 1.0)), count=2)
        blob = bytearray(self.fixture.basis.read_bytes())
        struct.pack_into("<9f", blob, 16 + 36,
                         0.0, 1.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 1.0)
        self.fixture.basis.write_bytes(blob)
        with self.assertRaisesRegex(ContractError, "contradicts constants.c0"):
            self.validate()

    def test_basis_file_bounds_and_truncation(self) -> None:
        self.fixture.basis.write_bytes(struct.pack("<4sIII", b"MVB1", 1, 2, 36))
        with self.assertRaisesRegex(DataError, "expected exactly"):
            self.validate()
        self.fixture.basis.write_bytes(struct.pack("<4sIII", b"MVB1", 1, 2_000_000, 36))
        with self.assertRaisesRegex(ContractError, "max_vertex_records"):
            self.validate()

    def test_normal_manifest_signature_hash_and_dimension(self) -> None:
        self.fixture.data["signature"] = "p1200005_ADD_NORMAL"
        self.fixture.data["pipeline"] = "static.p1200005_add_normal"
        with self.assertRaisesRegex(ContractError, "signature.name"):
            self.validate()
        self.fixture.data["signature"] = "p5_MAD_NORMAL"
        self.fixture.data["pipeline"] = "static.p5_mad_normal"
        manifest = load_json(self.fixture.normal_manifest, Limits(), "normal")
        manifest["output"]["sha256"] = "0" * 64
        self.fixture.normal_manifest.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ContractError, "sha256"):
            self.validate()
        self.fixture = MenuFixture(self.root)
        self.fixture.diffuse.write_bytes(rgba_dds(2, 2, b"\0" * 16))
        with self.assertRaisesRegex(ContractError, "does not match"):
            self.validate()

    def test_duplicate_json_keys_are_rejected(self) -> None:
        with self.assertRaisesRegex(ContractError, "duplicate JSON key"):
            parse_json(b'{"schema_version":1,"schema_version":1}', "test", 1024)


class GlyphFixture:
    def __init__(self, root: Path):
        self.root = root
        self.atlas = root / "atlas.dds"
        self.vertices = root / "quads.mgv1"
        self.descriptor = root / "glyph.json"
        pixels = bytearray()
        for alpha in (0, 64, 128, 192, 255, 32, 224, 96,
                      0, 64, 128, 192, 255, 32, 224, 96):
            pixels += bytes((255, 255, 255, alpha))
        self.atlas.write_bytes(rgba_dds(4, 4, bytes(pixels)))
        write_glyph_vertices(self.vertices, quads=2)
        self.data = {
            "schema_version": 1,
            "material": {"source": "fixed_scaleform", "kind": "scaleform_glyph_atlas"},
            "bytecode": None,
            "atlas": self.atlas.name,
            "atlas_contract": {"alpha": "straight", "rgb": "white_mask",
                               "color_space": "linear"},
            "vertices": self.vertices.name,
            "vertex_contract": {"topology": "triangle_list", "vertices_per_quad": 6,
                                "position": "clip_xyzw", "texcoord0": "normalized_2d",
                                "color0": "rgba8_unorm"},
            "state": {"texture_units": 1, "sampler": "2d", "filter": "bilinear",
                      "blend": "src_alpha_one_minus_src_alpha",
                      "depth": "test_only_no_write", "alpha_test": "none",
                      "tev_stages": 1, "constants": 0},
            "variants": {"sdf": False, "outline": False, "shadow": False,
                         "color_layers": False, "subpixel": False},
        }

    def save(self) -> Path:
        self.descriptor.write_text(json.dumps(self.data), encoding="utf-8")
        return self.descriptor


class GlyphTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.fixture = GlyphFixture(self.root)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def validate(self):
        return validate_glyph(self.fixture.save())

    def test_complete_fixed_contract(self) -> None:
        report = self.validate()
        self.assertEqual(report["status"], "pass")
        self.assertIsNone(report["bytecode"])
        self.assertEqual(report["vertices"]["quads"], 2)
        self.assertFalse(report["runtime_integrated"])
        self.assertLessEqual(report["atlas"]["memory"]["peak_bytes"], 16 * 1024 * 1024)

    def test_no_fxp_and_registry_state_are_mandatory(self) -> None:
        self.fixture.data["bytecode"] = b"BSPS".decode("latin-1")
        with self.assertRaisesRegex(ContractError, "no .fxp"):
            self.validate()
        self.fixture.data["bytecode"] = None
        self.fixture.data["material"]["source"] = "fxp_ps3"
        with self.assertRaisesRegex(ContractError, "fixed_scaleform"):
            self.validate()
        self.fixture.data["material"]["source"] = "fixed_scaleform"
        self.fixture.data["state"]["blend"] = "one_one"
        with self.assertRaisesRegex(ContractError, "blend"):
            self.validate()
        self.fixture.data["state"]["blend"] = "src_alpha_one_minus_src_alpha"
        self.fixture.data["variants"]["sdf"] = True
        with self.assertRaisesRegex(ContractError, "variants.sdf"):
            self.validate()

    def test_premultiplied_dxt2_encoding_is_rejected(self) -> None:
        self.fixture.atlas.write_bytes(bc2_header(b"DXT2"))
        with self.assertRaisesRegex(DataError, "premultiplied DXT2"):
            self.validate()

    def test_premultiplied_and_colour_atlases_fail(self) -> None:
        premultiplied = bytearray()
        for alpha in (0, 64, 128, 192, 255, 32, 224, 96,
                      0, 64, 128, 192, 255, 32, 224, 96):
            premultiplied += bytes((alpha, alpha, alpha, alpha))
        self.fixture.atlas.write_bytes(rgba_dds(4, 4, bytes(premultiplied)))
        with self.assertRaisesRegex(ContractError, "premultiplied"):
            self.validate()
        colour = bytearray(self.fixture.atlas.read_bytes()[128:])
        colour[0:4] = bytes((255, 0, 0, 255))
        self.fixture.atlas.write_bytes(rgba_dds(4, 4, bytes(colour)))
        with self.assertRaisesRegex(ContractError, "non-greyscale"):
            self.validate()

    def test_vertex_topology_uv_clip_and_shared_corners(self) -> None:
        blob = bytearray(self.fixture.vertices.read_bytes())
        struct.pack_into("<I", blob, 8, 7)  # not divisible by six
        self.fixture.vertices.write_bytes(blob)
        with self.assertRaisesRegex(ContractError, "divisible by 6"):
            self.validate()
        write_glyph_vertices(self.fixture.vertices, quads=1)
        blob = bytearray(self.fixture.vertices.read_bytes())
        struct.pack_into("<f", blob, 16 + 16, 1.5)  # first vertex u
        self.fixture.vertices.write_bytes(blob)
        with self.assertRaisesRegex(ContractError, "texcoord0"):
            self.validate()
        write_glyph_vertices(self.fixture.vertices, quads=1)
        blob = bytearray(self.fixture.vertices.read_bytes())
        struct.pack_into("<f", blob, 16 + 12, 0.0)  # first vertex w
        self.fixture.vertices.write_bytes(blob)
        with self.assertRaisesRegex(ContractError, "w must be positive"):
            self.validate()
        write_glyph_vertices(self.fixture.vertices, quads=1)
        blob = bytearray(self.fixture.vertices.read_bytes())
        struct.pack_into("<f", blob, 16 + 3 * 28, -0.7)  # corner 2 duplicate x
        self.fixture.vertices.write_bytes(blob)
        with self.assertRaisesRegex(ContractError, "canonical|zero position"):
            self.validate()

    def test_vertex_file_bounds_and_truncation(self) -> None:
        self.fixture.vertices.write_bytes(struct.pack("<4sIII", b"MGV1", 1, 6, 28))
        with self.assertRaisesRegex(DataError, "expected exactly"):
            self.validate()
        self.fixture.vertices.write_bytes(struct.pack("<4sIII", b"MGV1", 1, 2_000_000, 28))
        with self.assertRaisesRegex(ContractError, "max_vertex_records"):
            self.validate()

    def test_small_memory_budget_is_enforced(self) -> None:
        with self.assertRaises(BudgetError):
            validate_glyph(self.fixture.save(), limits=Limits(max_memory_bytes=300))


class CliTests(unittest.TestCase):
    def test_normal_cli_success_and_honest_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.dds"
            output = root / "output.dds"
            report = root / "report.json"
            source.write_bytes(rgba_dds(4, 4, normal_pixels()))
            command = [sys.executable, "-m", "tools.menu_preprocess", "normal",
                       str(source), "--signature", "p5_MAD_NORMAL",
                       "--output", str(output), "--report", str(report)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.exists())
            self.assertEqual(load_json(report, Limits(), "CLI report")["status"], "pass")

            output.unlink()
            source.write_bytes(bc5_snorm_header())
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("SNORM", result.stderr)
            self.assertFalse(output.exists())
            self.assertEqual(load_json(report, Limits(), "CLI failure")["status"], "fail")


if __name__ == "__main__":
    unittest.main(verbosity=2)
