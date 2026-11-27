#!/usr/bin/env python3
"""sky_water pack: plan/IR, coverage and negative tests.

The pack owns 45 bytecode hashes.  This test is deliberately offline: it reads
the committed catalog, the pack manifest and the census refusal table, and it
re-derives the plan/IR relation when the host IR dump exists
(``E:\\host_sm3\\ps_ir.bin``).  No retail bytes are committed.

Layers:

* **schema** - every catalog row passes ``plan_verify.validate_entry``;
* **coverage** - plans and refusals partition the 45 hashes and 82 chunks, and
  the manifest agrees;
* **honesty** - ``quality=approx`` requires a non-empty ``approximation``;
  ``quality=exact`` requires ``contract=full``, preconditions and constant
  bindings; nothing is draw-enabled or runtime-enabled;
* **plan vs IR** - instruction count, declared sampler/texcoord types, and for
  exact rows a symbolic comparison of the final ``oC0`` expression between the
  IR and the TEV stage data (INTERPOLATE is expanded to the same lrp form).
"""

from __future__ import annotations

import json
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "shaders"))

from shaders import plan_verify  # noqa: E402

PACK_DIR = os.path.join(REPO, "source", "shader_packs", "sky_water")
CORPUS = os.environ.get("SM3_CORPUS", r"E:\host_sm3")
MANIFEST = os.path.join(PACK_DIR, "manifest.json")
PLANS = os.path.join(PACK_DIR, "plans.json")
REFUSALS = os.path.join(PACK_DIR, "refusals.json")
SCHEMA = os.path.join(REPO, "docs", "shader_plan_schema.json")

SRC_PRIMARY, SRC_TEX0, SRC_CONST, SRC_PREV = 0, 3, 14, 15
F_REPLACE, F_MODULATE, F_ADD, F_SUBTRACT = 0, 1, 2, 5
F_INTERPOLATE, F_MULTIPLY_ADD = 4, 8


def _load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


# ---------------------------------------------------------------------------
# Minimal symbolic model shared by the IR text and the TEV descriptor.

def _atom(kind, *args):
    return (kind,) + tuple(args)


def _canon(node):
    if not isinstance(node, tuple):
        return node
    kind = node[0]
    if kind in ("add", "mul"):
        flat = []
        for child in node[1:]:
            child = _canon(child)
            if isinstance(child, tuple) and child[0] == kind:
                flat.extend(child[1:])
            else:
                flat.append(child)
        # One and zero fold, commutative ordering.
        if kind == "mul" and _atom("one") in flat:
            flat = [x for x in flat if x != _atom("one")]
            if not flat:
                return _atom("one")
            if len(flat) == 1:
                return flat[0]
        flat.sort(key=repr)
        return (kind,) + tuple(flat)
    if kind == "lrp":
        c, a, b = (_canon(x) for x in node[1:])
        return _canon(_atom("add", _atom("mul", c, a),
                            _atom("mul", _atom("sub", _atom("one"), c), b)))
    if kind in ("sub", "neg"):
        return (kind,) + tuple(_canon(x) for x in node[1:])
    return (kind,) + tuple(_canon(x) for x in node[1:])


_COMP = {"x": 0, "y": 1, "z": 2, "w": 3, "r": 0, "g": 1, "b": 2, "a": 3}


def _operand(text, vars_):
    text = text.strip()
    neg = text.startswith("-")
    if neg:
        text = text[1:]
    parts = re.match(r"([a-zA-Z]+\d*)(?:\.([xyzwrgba]+))?$", text)
    if not parts:
        raise ValueError("operand %r" % text)
    head, swz = parts.group(1), parts.group(2)
    if head.startswith("oC"):
        base = _atom("out", int(head[2:]))
        comps = [_COMP[c] for c in swz] if swz else [0, 1, 2, 3]
    elif head[0] == "r":
        idx = int(head[1:])
        comps = [_COMP[c] for c in swz] if swz else [0, 1, 2, 3]
        base = _atom("r", idx)
    elif head[0] == "v":
        idx = int(head[1:])
        comps = [_COMP[c] for c in swz] if swz else [0, 1, 2, 3]
        base = _atom("v", idx)
    elif head[0] == "c":
        idx = int(head[1:])
        comps = [_COMP[c] for c in swz] if swz else [0, 1, 2, 3]
        base = _atom("c", idx)
    elif head[0] == "s":
        idx = int(head[1:])
        comps = [0]
        base = _atom("sampler", idx)
    else:
        raise ValueError("operand %r" % text)

    def value(ch):
        comp = comps[ch] if ch < len(comps) else comps[-1]
        if base[0] == "c":
            return _atom("c", base[1], "xyzw"[comp])
        if base[0] == "r" and base[1] in vars_:
            stored = vars_[base[1]][comp]
            return stored if stored is not None else base + (comp,)
        return base + (comp,)

    return (lambda ch: _atom("neg", value(ch)) if neg else value(ch)), comps


def _ir_outputs(text):
    """Execute the tiny exact subset and return the oC0 channel expressions."""
    regs = {}
    out = [None, None, None, None]
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("ps_", "vs_", "dcl", "def", "defi", "end")):
            continue
        m = re.match(r"([a-z0-9_]+)(?:_(?:sat|pp))*\s+(.+)", line)
        if not m:
            continue
        op, rest = m.group(1), m.group(2)
        args = [a.strip() for a in rest.split(",")]
        dst = args[0]
        dm = re.match(r"([a-zA-Z]+\d*)(?:\.([xyzw]+))?$", dst)
        if not dm:
            raise ValueError("dst %r" % dst)
        head, swz = dm.group(1), dm.group(2)
        mask = [_COMP[c] for c in swz] if swz else [0, 1, 2, 3]
        if op == "texld":
            sampler = int(args[2][1:])
            regs[int(head[1:])] = [_atom("tex", sampler, ch) for ch in range(4)]
            continue
        src = [_operand(a, regs) for a in args[1:]]

        def val(i, ch):
            return src[i][0](ch)

        result = []
        for ch in range(4):
            if op == "mov":
                e = val(0, ch)
            elif op == "mul":
                e = _atom("mul", val(0, ch), val(1, ch))
            elif op == "add":
                e = _atom("add", val(0, ch), val(1, ch))
            elif op == "sub":
                e = _atom("sub", val(0, ch), val(1, ch))
            elif op == "mad":
                e = _atom("add", _atom("mul", val(0, ch), val(1, ch)), val(2, ch))
            elif op == "lrp":
                e = _atom("lrp", val(0, ch), val(1, ch), val(2, ch))
            else:
                raise ValueError("unsupported exact op %r" % op)
            result.append(e)
        if head == "oC0":
            for ch in mask:
                out[ch] = result[ch]
        else:
            reg = regs.setdefault(int(head[1:]), [None] * 4)
            for ch in mask:
                reg[ch] = result[ch]
    return out


def _plan_outputs(entry):
    bind = entry.get("constant_bindings", {})
    prev = [None] * 4
    for stage in entry["stage_data"]:
        cur = []
        for alpha in (False, True):
            func = stage["func_a"] if alpha else stage["func_rgb"]
            srcs = stage["src_a"] if alpha else stage["src_rgb"]
            ops = stage["op_a"] if alpha else stage["op_rgb"]
            for ch in range(4):
                vals = []
                for i, src in enumerate(srcs):
                    sel = ops[i]
                    comp = 3 if alpha else ch
                    if not alpha and sel in (2, 3):
                        comp = 3
                    if src == SRC_PREV:
                        v = prev[comp]
                    elif SRC_TEX0 <= src <= 5:
                        unit = src - SRC_TEX0
                        sampler = entry["textures"][unit]["sampler_reg"]
                        v = _atom("tex", sampler, comp)
                    elif src == SRC_PRIMARY:
                        reg = next(i["reg_index"] for i in entry["inputs"]
                                   if i["tev_source"] == 0)
                        v = _atom("v", reg, comp)
                    elif src == SRC_CONST:
                        spec = bind[str(stage["const_reg"])]
                        key = "alpha" if (alpha or sel in (2, 3)) else "rgb"
                        ref = spec.get(key) or spec.get("rgb") or spec.get("alpha")
                        v = _atom("c", int(ref[1]), ref[3])
                    else:
                        raise ValueError("source %r" % src)
                    vals.append(v)
                if func == F_REPLACE:
                    e = vals[0]
                elif func == F_MODULATE:
                    e = _atom("mul", vals[0], vals[1])
                elif func == F_ADD:
                    e = _atom("add", vals[0], vals[1])
                elif func == F_SUBTRACT:
                    e = _atom("sub", vals[0], vals[1])
                elif func == F_MULTIPLY_ADD:
                    e = _atom("add", _atom("mul", vals[0], vals[1]), vals[2])
                elif func == F_INTERPOLATE:
                    # (A-B)*C+B, C from the alpha channel for RGB and A alike.
                    e = _atom("lrp", vals[2], vals[0], vals[1])
                else:
                    raise ValueError("func %r" % func)
                cur.append(e)
        prev = cur[:4]
        prev[3] = cur[7]
    return prev


def _ir_text_index():
    """Parse the host PS IR dump; returns {sha256: {'text','ninstr'}} or {}."""
    corpus_path = os.path.join(CORPUS, "corpus.json")
    ir_path = os.path.join(CORPUS, "ps_ir.bin")
    if not (os.path.isfile(corpus_path) and os.path.isfile(ir_path)):
        return {}
    corpus = _load(corpus_path)
    by_index = {}
    for row in corpus["rows"]:
        if row["kind"] == "ps":
            by_index.setdefault(row["index"], row["sha256"])
    with open(ir_path, "rb") as handle:
        blob = handle.read()
    if blob[:7] != b"SM3IR1\0":
        return {}
    count = struct.unpack_from("<I", blob, 7)[0]
    off = 11
    out = {}
    for _ in range(count):
        index, _status, ninstr, tlen = struct.unpack_from("<IIII", blob, off)
        off += 16
        text = blob[off:off + tlen].decode("utf-8", "replace")
        off += tlen
        sha = by_index.get(index)
        if sha:
            out.setdefault(sha, {"text": text, "ninstr": ninstr})
    return out


# ---------------------------------------------------------------------------
# Pack validator (pure, reused by the negative cases).

def validate_pack(manifest, plans_doc, refusals_doc, ir_index=None):
    errors = []
    schema = _load(SCHEMA)
    entries = plans_doc.get("entries", [])
    rows = {r[0]: {"kind": r[1], "class": r[2], "chunks": r[3]}
            for r in manifest["hashes"]}
    planned = {}
    for entry in entries:
        where = entry.get("id", "?")
        plan_verify.validate_entry(entry, errors, where)
        sha = entry.get("sha256")
        if sha in planned:
            errors.append("%s: duplicate hash" % where)
        planned[sha] = entry
        if entry.get("draw_enabled") not in (0, 1):
            errors.append("%s: bad draw_enabled" % where)
        if entry.get("draw_enabled") == 1 and entry.get("contract") != "full":
            errors.append("%s: draw_enabled=1 without a full contract" % where)
        if entry.get("runtime") not in (0, None):
            errors.append("%s: runtime=1 is not claimed by this pack" % where)
        if entry.get("quality") == "approx":
            if not entry.get("approximation"):
                errors.append("%s: approx row has no approximation text" % where)
        elif entry.get("quality") == "exact":
            if entry.get("contract") != "full":
                errors.append("%s: exact row needs contract=full" % where)
            if entry.get("approximation"):
                errors.append("%s: exact row carries approximation text" % where)
            if not entry.get("constant_bindings"):
                errors.append("%s: exact row needs constant_bindings" % where)
            if not entry.get("preconditions"):
                errors.append("%s: exact row needs preconditions" % where)
        else:
            errors.append("%s: quality must be exact or approx" % where)
        if entry.get("stages") != len(entry.get("stage_data", [])):
            errors.append("%s: stage count mismatch" % where)
        if not 1 <= entry.get("stages", 0) <= 3:
            errors.append("%s: pack limit is 1..3 stages" % where)
        for stage in entry.get("stage_data", []):
            if stage.get("const_reg") != 255 and stage["const_reg"] not in entry.get("constants", []):
                errors.append("%s: stage constant not declared" % where)
        for texture in entry.get("textures", []):
            if texture.get("sampler_type") not in (2, 3):
                errors.append("%s: unexpected sampler type" % where)

    refused = {}
    for row in refusals_doc.get("refusals", []):
        sha = row.get("sha256")
        if sha in refused:
            errors.append("%s: duplicate refusal" % sha[:16])
        refused[sha] = row
        if sha not in rows:
            errors.append("%s: refusal hash is not in the pack manifest" % sha[:16])
        if not row.get("detail") or not row.get("needs"):
            errors.append("%s: refusal lacks detail/needs" % sha[:16])
    for sha in rows:
        if sha in planned and sha in refused:
            errors.append("%s: both planned and refused" % sha[:16])
        if sha not in planned and sha not in refused:
            errors.append("%s: hash is neither planned nor refused" % sha[:16])
    coverage = refusals_doc.get("coverage", {})
    if coverage.get("translated") != len(planned):
        errors.append("coverage.translated disagrees with the catalog")
    if coverage.get("refused") != len(refused):
        errors.append("coverage.refused disagrees with the refusal table")
    if coverage.get("translated_chunks") != sum(rows[s]["chunks"] for s in planned if s in rows):
        errors.append("coverage.translated_chunks is wrong")
    if coverage.get("refused_chunks") != sum(rows[s]["chunks"] for s in refused if s in rows):
        errors.append("coverage.refused_chunks is wrong")
    if coverage.get("translated_chunks", 0) + coverage.get("refused_chunks", 0) != manifest["chunks"]:
        errors.append("coverage chunks do not add up to %d" % manifest["chunks"])

    if ir_index:
        for sha, entry in planned.items():
            ir = ir_index.get(sha)
            if not ir:
                errors.append("%s: no IR for hash" % entry["id"])
                continue
            if entry["instructions"] != ir["ninstr"]:
                errors.append("%s: instruction count %d != IR %d"
                              % (entry["id"], entry["instructions"], ir["ninstr"]))
            samplers = {}
            texcoords = set()
            for line in ir["text"].splitlines():
                m = re.search(r"dcl_(2d|cube)\s+s(\d+)", line)
                if m:
                    samplers[int(m.group(2))] = 2 if m.group(1) == "2d" else 3
                m = re.search(r"dcl_texcoord\d*\s+v(\d+)", line)
                if m:
                    texcoords.add(int(m.group(1)))
            for texture in entry["textures"]:
                reg = texture["sampler_reg"]
                if samplers.get(reg) != texture["sampler_type"]:
                    errors.append("%s: sampler s%d type mismatch with IR"
                                  % (entry["id"], reg))
                if texture["texcoord_reg"] not in texcoords:
                    errors.append("%s: texcoord v%d is not declared in IR"
                                  % (entry["id"], texture["texcoord_reg"]))
            if entry["quality"] == "exact":
                ir_out = _ir_outputs(ir["text"])
                tev_out = _plan_outputs(entry)
                for ch in range(4):
                    if _canon(ir_out[ch]) != _canon(tev_out[ch]):
                        errors.append("%s: channel %d expression differs from IR"
                                      % (entry["id"], ch))
    return errors


class SkyWaterPackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = _load(MANIFEST)
        cls.plans = _load(PLANS)
        cls.refusals = _load(REFUSALS)
        cls.ir = _ir_text_index()

    def test_pack_partition_and_coverage(self):
        self.assertEqual(len(self.manifest["hashes"]), 45)
        self.assertEqual(self.manifest["chunks"], 82)
        errors = validate_pack(self.manifest, self.plans, self.refusals)
        self.assertEqual(errors, [])
        self.assertEqual(len(self.plans["entries"]), 14)
        self.assertEqual(len(self.refusals["refusals"]), 31)
        self.assertEqual(self.plans["summary"]["exact"], 3)
        self.assertEqual(self.plans["summary"]["approx"], 11)

    def test_no_draw_or_runtime_claim(self):
        for entry in self.plans["entries"]:
            self.assertEqual(entry["draw_enabled"], 0, entry["id"])
            self.assertEqual(entry.get("runtime", 0), 0, entry["id"])
        self.assertEqual(self.refusals["coverage"]["draw_enabled"], 0)

    def test_plan_vs_ir(self):
        if not self.ir:
            self.skipTest("host IR dump absent")
        errors = validate_pack(self.manifest, self.plans, self.refusals, self.ir)
        self.assertEqual(errors, [])

    def test_every_exact_row_is_schematic(self):
        for entry in self.plans["entries"]:
            if entry["quality"] != "exact":
                continue
            self.assertTrue(entry["preconditions"], entry["id"])
            self.assertTrue(entry["constant_bindings"], entry["id"])
            self.assertLessEqual(entry["stages"], 2, entry["id"])

    # -- negative cases -----------------------------------------------------
    def test_negative_missing_approximation(self):
        doc = json.loads(json.dumps(self.plans))
        row = next(e for e in doc["entries"] if e["quality"] == "approx")
        row["approximation"] = ""
        errors = validate_pack(self.manifest, doc, self.refusals)
        self.assertTrue(any("approximation" in e for e in errors), errors)

    def test_negative_draw_enabled_without_contract(self):
        doc = json.loads(json.dumps(self.plans))
        row = next(e for e in doc["entries"] if e["quality"] == "approx")
        row["draw_enabled"] = 1
        errors = validate_pack(self.manifest, doc, self.refusals)
        self.assertTrue(any("full contract" in e for e in errors), errors)

    def test_negative_hash_in_both_tables(self):
        doc = json.loads(json.dumps(self.refusals))
        doc["refusals"].append({
            "sha256": self.plans["entries"][0]["sha256"], "kind": "ps",
            "chunks": 1, "class": "x", "reason": "x", "detail": "x", "needs": "x"})
        errors = validate_pack(self.manifest, self.plans, doc)
        self.assertTrue(any("both planned and refused" in e for e in errors), errors)

    def test_negative_exact_tamper(self):
        if not self.ir:
            self.skipTest("host IR dump absent")
        doc = json.loads(json.dumps(self.plans))
        row = next(e for e in doc["entries"] if e["id"] == "sky.tex_vcolor_bias.ps")
        row["stage_data"][0]["func_rgb"] = F_MODULATE
        errors = validate_pack(self.manifest, doc, self.refusals, self.ir)
        self.assertTrue(any("expression differs" in e for e in errors), errors)


if __name__ == "__main__":
    unittest.main()
