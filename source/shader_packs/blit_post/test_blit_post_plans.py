#!/usr/bin/env python3
"""blit_post pack tests: catalog vs manifest, TEV contract, plan vs IR.

Run offline (no retail archive needed)::

    python -m unittest discover -s source/shader_packs/blit_post -p "test_*.py"

The optional IR checks use ``E:\\host_sm3`` (or ``--corpus``) when it exists
and skip cleanly otherwise.  Nothing retail-derived is committed.
"""

from __future__ import annotations

import copy
import json
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for path in (REPO, os.path.join(REPO, "tools")):
    if path not in sys.path:
        sys.path.insert(0, path)

from shaders import plan_verify  # noqa: E402

PLANS = os.path.join(HERE, "plans.json")
REFUSALS = os.path.join(HERE, "refusals.json")
MANIFEST = os.path.join(HERE, "manifest.json")
DEFAULT_CORPUS = r"E:\host_sm3"
RUNTIME_CERTIFIED = {
    "6022044ef19e4b1c241482207acca5a6be484081e629b6d72cdefdd9568a45de",
    "0ccc7542aa06b2e20a41283acc2d5473602fc03f5495cc95166ec3b01820d2f7",
}
EXPECTED = {
    "planned": 67,
    "exact_draw_enabled": 0,
    "approx_translated": 53,
    "contract_only_vs": 8,
    "refused": 0,
}


def _load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def validate_catalog(doc):
    """Semantic rules specific to this pack, on top of the shared schema."""
    errors = []
    ids, hashes = set(), set()
    for entry in doc["entries"]:
        plan_verify.validate_entry(entry, errors, entry.get("id", "?"))
        if entry["id"] in ids:
            errors.append("%s: duplicate id" % entry["id"])
        if entry["sha256"] in hashes:
            errors.append("%s: duplicate hash" % entry["id"])
        ids.add(entry["id"])
        hashes.add(entry["sha256"])
        if entry["draw_enabled"] == 1:
            if entry.get("quality") != "exact":
                errors.append("%s: draw_enabled needs quality=exact"
                              % entry["id"])
            if entry.get("approximation"):
                errors.append("%s: an approximation cannot be draw-enabled"
                              % entry["id"])
            if entry.get("runtime") == 1:
                errors.append("%s: runtime needs a separate certification"
                              % entry["id"])
        if entry.get("quality") == "approx":
            if not entry.get("approximation"):
                errors.append("%s: approx plan needs an approximation field"
                              % entry["id"])
            if entry["draw_enabled"] or entry.get("runtime") == 1:
                errors.append("%s: approx plan must stay draw/runtime 0"
                              % entry["id"])
        if entry.get("runtime") != 0:
            errors.append("%s: runtime must be 0 in the phase-1 catalog"
                          % entry["id"])
        if entry["kind"] == "tev":
            if entry["stages"] != len(entry["stage_data"]):
                errors.append("%s: stages/stage_data mismatch" % entry["id"])
            for i, stage in enumerate(entry["stage_data"]):
                for key in ("func_rgb", "func_a", "src_rgb", "src_a",
                            "op_rgb", "op_a", "scale_rgb", "scale_a"):
                    if key not in stage:
                        errors.append("%s stage %d missing %s"
                                      % (entry["id"], i, key))
            if entry["draw_enabled"] == 1 and entry["stages"] != 1:
                errors.append("%s: exact draw plan must be one stage"
                              % entry["id"])
        else:
            if entry["draw_enabled"]:
                errors.append("%s: pica_vs cannot be draw-enabled"
                              % entry["id"])
    return errors


class BlitPostCatalogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = _load(PLANS)
        cls.refusals = _load(REFUSALS)
        cls.manifest = _load(MANIFEST)
        cls.hashes = {row[0] for row in cls.manifest["hashes"]}

    def test_catalog_is_schema_valid(self):
        self.assertEqual(validate_catalog(self.doc), [])

    def test_counts(self):
        counts = self.refusals["counts"]
        for key, want in EXPECTED.items():
            self.assertEqual(counts[key], want, key)
        self.assertEqual(self.refusals["counts"]["planned"],
                         len(self.doc["entries"]))

    def test_partition_is_exhaustive(self):
        planned = {e["sha256"] for e in self.doc["entries"]}
        refused = {r["sha256"] for r in self.refusals["refusals"]}
        self.assertFalse(planned & refused)
        self.assertTrue(planned <= self.hashes)
        self.assertTrue(refused <= self.hashes)
        self.assertEqual(planned | refused | RUNTIME_CERTIFIED, self.hashes)
        self.assertEqual(len(planned) + len(refused) + len(RUNTIME_CERTIFIED),
                         69)

    def test_coverage_arithmetic(self):
        translated = sum(1 for e in self.doc["entries"])
        refused = len(self.refusals["refusals"])
        self.assertEqual(translated, 67)
        self.assertEqual(translated + refused + len(RUNTIME_CERTIFIED), 69)
        self.assertEqual(refused, 0)

    def test_draw_plans_are_full_material_contracts(self):
        # Catalog-only phase: exactness alone is not a runtime certification,
        # so the pack claims no draw-enabled row.
        draw = [e for e in self.doc["entries"] if e["draw_enabled"] == 1]
        self.assertEqual(len(draw), 0)
        for entry in draw:
            self.assertEqual(entry["kind"], "tev")
            self.assertEqual(entry["stages"], 1)
            self.assertLessEqual(len(entry["textures"]), 1)
            self.assertEqual(entry["uses_vertex_color"], 0)
            self.assertEqual(entry["requires_alpha_fixup"], 0)
            self.assertEqual(entry["constants"], [])
            stage = entry["stage_data"][0]
            self.assertEqual(stage["const_is_literal"], 1 if not entry["textures"]
                             else stage.get("const_is_literal", 0))
            if entry["textures"]:
                self.assertEqual(stage["src_rgb"][0], 3)  # SRC_TEX0
                self.assertEqual(stage["const_reg"], 255)

    def test_approximations_are_named(self):
        approx = [e for e in self.doc["entries"]
                  if e.get("quality") == "approx"]
        self.assertEqual(len(approx), 53)
        for entry in approx:
            self.assertTrue(entry.get("approximation"))
            self.assertEqual(entry["draw_enabled"], 0)
            self.assertIn(entry["kind"], ("tev", "pica_vs"))

    def test_vs_contracts_fit_pica(self):
        vs = [e for e in self.doc["entries"] if e["kind"] == "pica_vs"]
        self.assertEqual(len(vs), 8)
        for entry in vs:
            self.assertEqual(entry["stage"], "vs_3_0")
            self.assertEqual(entry["draw_enabled"], 0)
            self.assertTrue(1 <= len(entry["outputs"]) <= 7)
            self.assertLessEqual(len(entry["inputs"]), 16)
            self.assertLessEqual(entry["uniform_registers"], 96)
            varyings = [o for o in entry["outputs"] if o["usage"] != 0]
            self.assertLessEqual(len(varyings), 7)

    def test_escape_hatches_cover_every_census_class(self):
        classes = {row[0]: row[2] for row in self.manifest["hashes"]}
        planned = {e["sha256"] for e in self.doc["entries"]}
        self.assertEqual(set(classes) - RUNTIME_CERTIFIED - planned, set())

        constant = [e for e in self.doc["entries"] if e.get("constant_ops")]
        self.assertEqual(len(constant), 8)
        for entry in constant:
            self.assertEqual(classes[entry["sha256"]],
                             "def_literal_out_of_range")
            self.assertEqual(entry["quality"], "approx")

        dataflow = [e for e in self.doc["entries"]
                    if classes[e["sha256"]] == "dataflow"]
        self.assertEqual(sum(1 for e in dataflow if e.get("passes")), 14)

        for cls in ("unsupported_opcode", "def_literal_out_of_range",
                    "dataflow"):
            for entry in self.doc["entries"]:
                if classes[entry["sha256"]] == cls and entry["kind"] == "tev":
                    self.assertEqual(entry["quality"], "approx")
                    self.assertTrue(entry.get("approximation"), entry["id"])

        for entry in self.doc["entries"]:
            if classes[entry["sha256"]] == "tev_prebake":
                self.assertTrue(entry.get("preconditions"), entry["id"])
            if classes[entry["sha256"]] == "input_not_a_colour_varying":
                self.assertEqual(entry.get("uses_vertex_color"), 1)
                self.assertTrue(entry.get("inputs"), entry["id"])

    def test_negative_draw_enabled_approximation_is_rejected(self):
        mutated = copy.deepcopy(self.doc)
        victim = next(e for e in mutated["entries"]
                      if e.get("quality") == "approx")
        victim["draw_enabled"] = 1
        errors = validate_catalog(mutated)
        self.assertTrue(any("approximation cannot be draw-enabled" in e
                            for e in errors), errors)

    def test_negative_approx_without_approximation_is_rejected(self):
        mutated = copy.deepcopy(self.doc)
        victim = next(e for e in mutated["entries"]
                      if e.get("quality") == "approx")
        victim.pop("approximation")
        errors = validate_catalog(mutated)
        self.assertTrue(any("needs an approximation field" in e
                            for e in errors), errors)

    def test_negative_runtime_without_certification(self):
        mutated = copy.deepcopy(self.doc)
        victim = next(e for e in mutated["entries"]
                      if e.get("quality") == "exact")
        victim["draw_enabled"] = 1
        victim["runtime"] = 1
        errors = validate_catalog(mutated)
        self.assertTrue(any("runtime needs a separate certification" in e
                            for e in errors), errors)

    def test_negative_unknown_hash(self):
        mutated = copy.deepcopy(self.doc)
        mutated["entries"][0]["sha256"] = "0" * 64
        planned = {e["sha256"] for e in mutated["entries"]}
        self.assertFalse(planned <= self.hashes)


def _load_corpus(corpus):
    rows = _load(os.path.join(corpus, "corpus.json"))["rows"]
    texts = {}
    for kind, name in (("ps", "ps_ir.bin"), ("vs", "vs_ir.bin")):
        with open(os.path.join(corpus, name), "rb") as handle:
            data = handle.read()
        count = struct.unpack("<I", data[7:11])[0]
        off = 11
        for _ in range(count):
            index, _status, _instr, tlen = struct.unpack(
                "<IIII", data[off:off + 16])
            off += 16
            text = data[off:off + tlen].decode("utf-8", "replace")
            off += tlen
            texts[(kind, index)] = text
    out = {}
    for row in rows:
        key = (row["kind"], row["index"])
        if key in texts:
            out[row["sha256"]] = texts[key]
    return out


class BlitPostIRTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = _load(PLANS)
        corpus = os.environ.get("SM3_CORPUS", DEFAULT_CORPUS)
        if not os.path.isfile(os.path.join(corpus, "corpus.json")):
            raise unittest.SkipTest("host corpus absent: %s" % corpus)
        cls.ir = _load_corpus(corpus)

    def text(self, sha):
        return self.ir.get(sha, "")

    def test_exact_copy_matches_ir(self):
        for sha in (
            "911297c9666e388aab307d72feaad9761379287276405a17732958d8ca6a1b26",
            "39f0dfdfc574d3e7f4c4ee0578bcc265b75ac05f1e0b9eb17087e52c6cc35d5a",
        ):
            self.assertIn("texld oC0, v0, s0", self.text(sha))
            self.assertEqual(self.text(sha).count("texld"), 1)

    def test_constant_fills_match_ir(self):
        expected = {
            "817340bfabd9a9b175aadbbdc0af49258dd38f2772985b1a40ba48d3eb1bf10e":
                (0x80808080, "mov oC0, c0.x"),
            "4813dba351f7114f15a15a1d9c5308823e7fcfcbed2fe0dc1960662a2feca520":
                (0x00008080, "mov_pp oC0, c0.xxy"),
            "6628a644b06721d0e5538d63bed3299bee56749ddb33af7300293cf38f3bbc6b":
                (0xFF000000, "mov oC0, c0.xxxy"),
            "9d2692a9640d3e1c9768fe5dfacc04a68ba13e5196d491ad6e905d3851cb4088":
                (0xFFFFFFFF, "mov oC0, c0.x"),
        }
        for sha, (rgba, line) in expected.items():
            self.assertIn(line, self.text(sha), sha)
            entry = next(e for e in self.doc["entries"]
                         if e["sha256"] == sha)
            self.assertEqual(entry["stage_data"][0]["const_rgba8"], rgba, sha)

    def test_approx_tap_plans_really_drop_taps(self):
        taps = {
            "70a85a92b46bbe8e963af44ecf6ca75c9c7d5b48ff4963eb389c665984675777":
                (2, "49"),
            "de28caa3cf7bb11d98788a133e5eb7c457ae2c8ae208f251a4c9a95ea7d130f2":
                (16, "16"),
            "0072af4b81a7b06b4b5bd6f54db0466ac2ebbf3ec6e1c127ad57608f2e6e3d89":
                (8, "8"),
        }
        for sha, (minimum, declared) in taps.items():
            self.assertGreaterEqual(self.text(sha).count("texld"), minimum, sha)
            entry = next(e for e in self.doc["entries"]
                         if e["sha256"] == sha)
            self.assertEqual(entry["stages"], 1)
            self.assertEqual(len(entry["textures"]), 1)
            self.assertIn(declared, entry["approximation"])

    def test_vs_descriptors_match_declarations(self):
        for entry in self.doc["entries"]:
            if entry["kind"] != "pica_vs":
                continue
            text = self.text(entry["sha256"])
            dcls = re.findall(r"dcl_(\w+)(\d*) (?:v|o)(\d+)", text)
            inputs = [d for d in dcls if d[0] != "position" or True]
            self.assertTrue(inputs, entry["id"])
            self.assertLessEqual(len(entry["outputs"]), 7)
            self.assertLessEqual(len(entry["inputs"]), 16)


if __name__ == "__main__":
    unittest.main()
