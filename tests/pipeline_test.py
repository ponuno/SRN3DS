#!/usr/bin/env python3
"""Data/schema tests for the specialised graphics-pipeline registry."""

from __future__ import annotations

import copy
import json
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from tools.pipelines import generate_registry, inventory

ROOT = Path(__file__).resolve().parents[1]
REGISTRY_PATH = ROOT / "tools" / "pipelines" / "registry.json"
COVERAGE_PATH = ROOT / "tools" / "pipelines" / "coverage_base.json"
GENERATED_PATH = ROOT / "source" / "d3d9_pipeline_registry.c"
DOCS_PATH = ROOT / "docs" / "PIPELINES.md"

EXPECTED_SOURCE_FAMILIES = {
    "ui/interface": (314, 195, 119, 113, 1),
    "sky": (10, 10, 0, 7, 1),
    "water": (72, 72, 0, 38, 1),
    "terrain/landscape": (327, 327, 0, 247, 11),
    "lighting/deferred/shadows": (7175, 6902, 273, 4824, 91),
    "skinning/characters": (2162, 1872, 290, 1194, 18),
    "particles/effects": (284, 172, 112, 167, 3),
    "grass/trees": (0, 0, 0, 0, 0),
    "post-process/imagespace": (120, 60, 60, 61, 1),
    "blit/copy": (11, 11, 0, 5, 1),
}


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


class RegistryDataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.registry = load(REGISTRY_PATH)
        cls.coverage = load(COVERAGE_PATH)

    def test_eight_required_families(self) -> None:
        self.assertEqual(
            {row["name"] for row in self.registry["families"]},
            set(generate_registry.FAMILY_ENUMS),
        )
        self.assertEqual(len(self.registry["families"]), 8)

    def test_generated_c_is_in_sync(self) -> None:
        generated = generate_registry.generate(self.registry)
        self.assertEqual(GENERATED_PATH.read_text(encoding="utf-8"), generated)
        self.assertNotIn("fog interpolation", generated.lower())
        self.assertNotIn("output tint", generated.lower())

    def test_descriptor_contracts_are_complete(self) -> None:
        ids = [row["id"] for row in self.registry["pipelines"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(len(ids), 13)
        for row in self.registry["pipelines"]:
            for field in (
                "inventory_signature", "accepted_original", "package_rule",
                "vertex_inputs", "texture_units", "quality_reductions",
                "unsupported", "fallback", "memory",
            ):
                self.assertTrue(row[field], (row["id"], field))
            self.assertGreaterEqual(len(row["passes"]), 1)
            self.assertLessEqual(len(row["passes"]), 6)
            for pass_row in row["passes"]:
                self.assertLessEqual(pass_row["stages"], 6)
                self.assertLessEqual(pass_row["textures"], 3)
            self.assertFalse({"sha1", "hash", "bytecode_hash"} & set(row))

    def test_support_split_is_explicit(self) -> None:
        support = {}
        for row in self.registry["pipelines"]:
            support[row["support"]] = support.get(row["support"], 0) + 1
        self.assertEqual(support, {"structural": 3, "preprocess": 2, "planned": 8})
        for row in self.registry["pipelines"]:
            if row["support"] == "planned":
                self.assertEqual(row["matcher"], "none")
            else:
                self.assertNotEqual(row["matcher"], "none")

    def test_generator_rejects_hash_acceptance(self) -> None:
        bad = copy.deepcopy(self.registry)
        bad["pipelines"][0]["bytecode_hash"] = "not-an-acceptance-key"
        with self.assertRaisesRegex(ValueError, "hashes cannot"):
            generate_registry.generate(bad)

    def test_generator_rejects_duplicate_and_missing_family(self) -> None:
        bad = copy.deepcopy(self.registry)
        bad["pipelines"][1]["id"] = bad["pipelines"][0]["id"]
        with self.assertRaisesRegex(ValueError, "duplicate pipeline id"):
            generate_registry.generate(bad)
        bad = copy.deepcopy(self.registry)
        bad["families"] = bad["families"][:7]
        with self.assertRaisesRegex(ValueError, "eight required families"):
            generate_registry.generate(bad)

    def test_fixed_glyph_has_no_fxp_claim(self) -> None:
        glyph = next(row for row in self.registry["pipelines"]
                     if row["id"] == "ui.scaleform_glyph")
        self.assertEqual(glyph["source"], "fixed_scaleform")
        self.assertEqual(glyph["features"]["version"], 0)
        self.assertIn("No .fxp", glyph["accepted_original"])
        self.assertEqual(glyph["passes"][0]["stages"], 1)
        self.assertEqual(glyph["passes"][0]["textures"], 1)

    def test_static_descriptors_require_preprocessing(self) -> None:
        for pipeline_id in ("static.p5_mad_normal", "static.p1200005_add_normal"):
            row = next(row for row in self.registry["pipelines"] if row["id"] == pipeline_id)
            self.assertEqual(row["support"], "preprocess")
            self.assertEqual(set(row["preconditions"]), {
                "normal_prebaked", "tangent_basis_prebaked",
                "ambient_emissive_varying", "material_alpha_c4",
            })
            self.assertEqual(row["passes"][0]["stages"], 5)
            self.assertEqual(row["passes"][0]["textures"], 2)
            description = json.dumps(row).lower()
            self.assertIn("c7.a", description)
            self.assertIn("c7.rgb is ignored", description)
            self.assertIn("alpha passthrough", row["passes"][0]["summary"].lower())
            self.assertNotIn("fog interpolation", description)
            self.assertNotIn("output tint", description)

    def test_static_docs_match_final_fade_stage(self) -> None:
        text = DOCS_PATH.read_text(encoding="utf-8")
        section = text.split("### 4. Static geometry", 1)[1].split("### 5.", 1)[0]
        lowered = section.lower()
        self.assertIn("c7.a", lowered)
        self.assertIn("c7.rgb", lowered)
        self.assertIn("ignored", lowered)
        self.assertIn("alpha passthrough", lowered)
        self.assertNotIn("fog interpolation", lowered)
        self.assertNotIn("output tint", lowered)


class CoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.coverage = load(COVERAGE_PATH)
        cls.registry = load(REGISTRY_PATH)

    def test_dataset_matches_measured_base_archive(self) -> None:
        self.assertEqual(self.coverage["dataset"]["archive"], "Skyrim - Shaders.bsa")
        self.assertFalse(self.coverage["dataset"]["include_update"])
        self.assertEqual(self.coverage["dataset"]["packages_effective"], 122)
        self.assertEqual(self.coverage["inventory_totals"], {
            "packages_effective": 122,
            "chunks": 10475,
            "ps": 9621,
            "vs": 854,
            "distinct_instruction_streams": 6655,
            "distinct_opcode_sequences": 6455,
        })

    def test_every_source_family_matches_docs_shaders(self) -> None:
        for family, expected in EXPECTED_SOURCE_FAMILIES.items():
            row = self.coverage["source_families"][family]
            actual = (row["chunks"], row["ps"], row["vs"],
                      row["distinct_instruction_streams"], row["packages"])
            self.assertEqual(actual, expected, family)

    def test_measured_descriptor_counts(self) -> None:
        families = self.coverage["pipeline_families"]
        ui = families["interface_text"]["descriptors"]
        self.assertEqual(ui["ui.copy"]["selected_chunks"], 7)
        self.assertEqual(ui["ui.copy"]["selected_distinct_instruction_streams"], 1)
        self.assertEqual(ui["ui.simple_fxp"]["selected_chunks"], 10)
        self.assertEqual(ui["ui.simple_fxp"]["selected_distinct_instruction_streams"], 5)
        self.assertEqual(ui["ui.scaleform_glyph"]["selected_chunks"], 0)
        self.assertEqual(families["interface_text"]["selected_chunks"], 17)
        self.assertEqual(families["interface_text"]["selected_distinct_instruction_streams"], 6)
        self.assertEqual(families["interface_text"]["selected_by_source_family"], {
            "blit/copy": 8, "particles/effects": 5, "post-process/imagespace": 4,
        })
        static = families["static_geometry"]["descriptors"]
        self.assertEqual(static["static.p5_mad_normal"]["selected_chunks"], 5)
        self.assertEqual(static["static.p1200005_add_normal"]["selected_chunks"], 1)
        self.assertEqual(families["static_geometry"]["selected_chunks"], 6)
        self.assertEqual(families["static_geometry"]["selected_distinct_instruction_streams"], 2)
        for name in ("sky", "landscape", "characters_animation", "vegetation",
                     "water", "particles"):
            self.assertEqual(families[name]["selected_chunks"], 0, name)

    def test_static_coverage_is_not_all_lighting(self) -> None:
        static = self.coverage["pipeline_families"]["static_geometry"]
        self.assertEqual(static["reference_inventory"]["lighting/deferred/shadows"]["chunks"], 7175)
        self.assertEqual(static["selected_chunks"], 6)
        self.assertIn("Semantic subset", static["inventory_scope_note"])
        p120 = static["descriptors"]["static.p1200005_add_normal"]["examples"][0]
        self.assertEqual(p120["meta"], "0x01200005")
        self.assertEqual(p120["offset"], 12)
        self.assertEqual(p120["bytecode_bytes"], 852)
        self.assertEqual(p120["reflection_trailer_bytes"], 78)

    def test_real_chunk_metadata_is_retained(self) -> None:
        ui = self.coverage["pipeline_families"]["interface_text"]["descriptors"]
        copy_examples = ui["ui.copy"]["examples"]
        self.assertEqual({row["bytecode_bytes"] for row in copy_examples}, {184, 188})
        self.assertTrue(all(row["reflection_trailer_bytes"] == 78 for row in copy_examples))
        simple_examples = ui["ui.simple_fxp"]["examples"]
        self.assertEqual(simple_examples[0]["package"], "shadersfx\\imagespace002.fxp")
        self.assertEqual(simple_examples[0]["bytecode_bytes"], 136)
        self.assertEqual(simple_examples[0]["reflection_trailer_bytes"], 78)
        self.assertEqual(simple_examples[0]["instructions"], 2)
        self.assertEqual(simple_examples[0]["texture_samples"], 0)

    def test_vegetation_zero_is_explicit(self) -> None:
        vegetation = self.coverage["pipeline_families"]["vegetation"]
        self.assertEqual(vegetation["reference_inventory"]["grass/trees"]["chunks"], 0)
        self.assertEqual(vegetation["selected_chunks"], 0)
        self.assertIn("zero shipped chunks", vegetation["inventory_note"])

    def test_no_invented_percentages(self) -> None:
        def walk(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    self.assertNotIn("percent", key.lower())
                    self.assertNotIn("fraction", key.lower())
                    self.assertFalse(key.lower().endswith("_pct"))
                    walk(child)
            elif isinstance(value, list):
                for child in value:
                    walk(child)
        walk(self.coverage)

    def test_parse_failures_are_zero(self) -> None:
        self.assertEqual(self.coverage["measurement"]["parse_failures"], 0)

    def test_status_does_not_claim_rendering(self) -> None:
        note = self.coverage["status_note"].lower()
        self.assertIn("no d3d9 com hook", note)
        self.assertIn("complete menu/world frame is claimed", note)


class DumpFormatTests(unittest.TestCase):
    def test_dump_uses_metadata_and_temp_bytecode(self) -> None:
        chunk = SimpleNamespace(
            meta=0x1200005, offset=12, trailer_size=78,
            code=b"\x00\x03\xff\xff\xff\xff\x00\x00",
        )
        package_name = "shadersfx\\lighting\\059\\p1200005.fxp"
        family_name = "lighting/deferred/shadows"
        record = SimpleNamespace(package=SimpleNamespace(path=package_name),
                                 family=family_name, chunk=chunk,
                                 program_sha1="a" * 40)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "chunks.bin"
            self.assertEqual(inventory.write_dump([record], path), 1)
            data = path.read_bytes()
        self.assertEqual(data[:8], b"D9PIPE01")
        self.assertEqual(struct.unpack_from("<I", data, 8)[0], 1)
        header = struct.unpack_from("<IIIIHHB3x", data, 12)
        self.assertEqual(header, (8, 0x1200005, 12, 78,
                                  len(package_name), len(family_name), 40))
        self.assertIn(package_name.encode("ascii"), data)
        self.assertIn(family_name.encode("ascii"), data)
        self.assertTrue(data.endswith(chunk.code))


if __name__ == "__main__":
    unittest.main()
