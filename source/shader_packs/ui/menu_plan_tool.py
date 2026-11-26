#!/usr/bin/env python3
"""Build and verify the StartMenu/GFx menu shader plans (pack ``ui``).

Why this exists
---------------
The 10 hashes in ``docs/MENU_SHADERS.md`` are *prefix* hashes: the usage log
hashes ``high16(second_token) * 4`` bytes from offset 0, which is the length of
the leading ``CTAB`` comment, not the shader.  Walking the token stream to
``END`` and hashing the whole blob gives the real identity; on the same run that
is **5 unique VS + 38 unique PS**, not 3 + 7.  Both hashes are recorded here so
the plan cannot silently claim a different program.

The real blobs live in ``TESV.exe`` in the GFx renderer tables (VA 0x12dfca8,
6 slots, and VA 0x12dfcd8, 45 slots).  This tool reads them in place, never
copies bytecode into the repository, and builds the packed IR with the same
host decoder as the rest of the project.

Commands::

    python source\\shader_packs\\ui\\menu_plan_tool.py build --tesv TESV.exe
    python source\\shader_packs\\ui\\menu_plan_tool.py check
    python source\\shader_packs\\ui\\menu_plan_tool.py check --tesv TESV.exe

``build`` writes ``menu_plans.json`` and ``menu_ir.json`` in this directory.
``check`` is the symbolic verifier: it replays each TEV descriptor against the
IR expression, checks the PICA VS envelope, and (with ``--tesv``) re-extracts
the blobs and re-derives the plan from the retail bytes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
if os.path.join(REPO, "tools") not in sys.path:
    sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import compile as C  # noqa: E402

PLANS_PATH = os.path.join(HERE, "menu_plans.json")
IR_PATH = os.path.join(HERE, "menu_ir.json")
#: Shared-catalog file discovered by tools/fxp_compile.py.  Entries here are
#: translated-only (`runtime` absent/0); nothing claims a runtime table slot.
CATALOG_PATH = os.path.join(HERE, "plans.json")

# GFx D3D9 renderer tables in TESV.exe (see docs/MENU_SHADERS.md section 2-3).
VS_TABLE_VA = 0x12DFCA8
VS_TABLE_SLOTS = 6
PS_TABLE_VA = 0x12DFCD8
PS_TABLE_SLOTS = 45

# The prefix hashes measured in docs/MENU_SHADERS.md.  Used only to prove the
# extraction selected the same objects the usage log saw.
EXPECTED_PREFIX = {
    "vs": [
        "77bc67a86e38ea6565c82f95313185a48420430c4f4c7afa6144d144ce075376",
        "4281bd82262c5a5a8d26588a60fea83d52a265f8ef08fcbf34ce2b0045eb826a",
        "ad97e55f922355acfd8f40e8d37f53ab122c8d3f7dbc5e2af1cc5e4adbe1f924",
    ],
    "ps": [
        "f031c9f821e5316df05109dd1464e2de244827ce39e7a597b12024765320e652",
        "01fd3eb5bf6aa39447bef056b2263e19e1199cf999c7a3aabe5d268b56ce4d6b",
        "2c4a728003938d0b599f535c0c000cd6f15e96b275254871847ed6db4f82025a",
        "8ced9de968aad07942ea3b454f127dfb00bd94761fa2c980f2c770ed504fa93d",
        "b527a0101301d171f291649401d9c45e64b82fb32e73e182f0725df71244275b",
        "1b279738cdd7559ca1bbd05e8b46c63d8f54f412f8bb8b45d2456278ab94f907",
        "cd1c433b3e73bd079e9d9449c0267a2e74f0c5311db04b2ac0920fae0e297918",
    ],
}

TEV_FUNC_NAMES = {
    0: "replace", 1: "modulate", 2: "add", 3: "subtract",
    4: "lerp", 5: "subtract", 6: "dot3_rgb", 7: "dot3_rgba",
    8: "multiply_add", 9: "add_signed",
}


# --------------------------------------------------------------------------
# PE / blob extraction

class PE:
    def __init__(self, path: str):
        self.data = open(path, "rb").read()
        self.base = 0x400000
        self.sections = []
        e = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[e:e + 4] != b"PE\0\0":
            raise SystemExit("not a PE file: %s" % path)
        nsec = struct.unpack_from("<H", self.data, e + 6)[0]
        opt = e + 24
        self.base = struct.unpack_from("<I", self.data, opt + 28)[0]
        sec = opt + struct.unpack_from("<H", self.data, e + 20)[0]
        for i in range(nsec):
            o = sec + i * 40
            name = self.data[o:o + 8].rstrip(b"\0").decode("latin-1")
            vsz, va, rsz, raw = struct.unpack_from("<IIII", self.data, o + 8)
            self.sections.append((name, va, vsz, raw, rsz))

    def offset(self, va: int) -> int:
        rva = va - self.base
        for _name, va0, vsz, raw, _rsz in self.sections:
            if va0 <= rva < va0 + vsz:
                return raw + (rva - va0)
        raise SystemExit("VA 0x%08x is not mapped" % va)

    def blob_size(self, va: int) -> int:
        pos = self.offset(va)
        start = pos
        while True:
            token = struct.unpack_from("<I", self.data, pos)[0]
            pos += 4
            if token == 0x0000FFFF:  # END
                return pos - start
            if (token & 0xFFFF) == 0xFFFE:  # comment
                pos += (token >> 16) * 4
            if pos - start > 0x10000:
                raise SystemExit("runaway shader at VA 0x%08x" % va)

    def blob(self, va: int) -> bytes:
        pos = self.offset(va)
        return self.data[pos:pos + self.blob_size(va)]


def read_table(pe: PE, table_va: int, slots: int):
    base = pe.offset(table_va)
    out = []
    for slot in range(slots):
        ptr = struct.unpack_from("<I", pe.data, base + slot * 4)[0]
        if ptr < 0x1000:
            continue
        pos = pe.offset(ptr)
        version = struct.unpack_from("<I", pe.data, pos)[0]
        if version not in (0xFFFE0300, 0xFFFF0300):
            continue
        blob = pe.blob(ptr)
        prefix_len = (struct.unpack_from("<I", pe.data, pos + 4)[0] >> 16) * 4
        out.append({
            "slot": slot,
            "table_va": table_va,
            "blob_va": ptr,
            "blob": blob,
            "prefix_len": prefix_len,
            "prefix_sha256": hashlib.sha256(blob[:prefix_len]).hexdigest(),
            "full_sha256": hashlib.sha256(blob).hexdigest(),
        })
    return out


# --------------------------------------------------------------------------
# Symbolic replay of a TEV descriptor (independent of the planner).

SRC = {"primary": 0x00, "frag_primary": 0x01, "frag_secondary": 0x02,
       "tex0": 0x03, "tex1": 0x04, "tex2": 0x05,
       "const": 0x0E, "prev": 0x0F}

# PICA per-source selectors: RGB uses 2-bit component steps, alpha is dense.
RGB_SELECT = {0x00: None, 0x02: 3, 0x04: 0, 0x08: 1, 0x0C: 2}
A_SELECT = {0x00: 3, 0x02: 0, 0x04: 1, 0x06: 2}


def _stage_symbol(stage: dict, prev, inputs):
    """Return the 4-channel symbolic result of one stage."""
    def leaf(src, comp):
        if src == SRC["prev"]:
            return prev[comp]
        if src in (SRC["tex0"], SRC["tex1"], SRC["tex2"]):
            return ("tex", src - SRC["tex0"], comp)
        if src == SRC["primary"]:
            return ("in", 1, comp)
        if src == SRC["frag_primary"]:
            return ("in", 2, comp)
        if src == SRC["const"]:
            reg = stage.get("const_reg", 255)
            if stage.get("const_is_literal"):
                return ("lit", stage.get("const_rgba8", 0), comp)
            return ("const", reg, comp)
        return ("src", src, comp)

    def comp_for(op, ch, alpha):
        table = A_SELECT if alpha else RGB_SELECT
        base = op & ~1
        value = table.get(base)
        if value is None:  # 0x00 colour, per-channel pass-through
            return ch
        if op & 1:  # 1 - component
            return ("one_minus", value)
        return value

    def combine(func, v):
        if func == 0:
            return v[0]
        if func == 1:
            return ("op", "mul", v[0], v[1], None)
        if func == 2:
            return ("op", "add", v[0], v[1], None)
        if func == 3:
            return ("op", "add_signed", v[0], v[1], None)
        if func == 4:
            return ("op", "lerp", v[0], v[1], v[2])
        if func == 5:
            return ("op", "sub", v[0], v[1], None)
        if func == 6:
            return ("dot3", v[0], v[1], None)
        if func == 7:
            return ("dot3", v[0], v[1], v[2])
        if func == 8:
            return ("op", "mad", v[0], v[1], v[2])
        if func == 9:
            return ("op", "add_mul", v[0], v[1], v[2])
        return ("func", func)

    def channel(ch, alpha):
        ops = stage["op_a"] if alpha else stage["op_rgb"]
        srcs = stage["src_a"] if alpha else stage["src_rgb"]
        vals = []
        for i in range(3):
            comp = comp_for(ops[i], ch, alpha)
            if isinstance(comp, tuple):
                vals.append(("op", "sub", ("lit", 0xFFFFFFFF, ch),
                             leaf(srcs[i], comp[1]), None))
            else:
                vals.append(leaf(srcs[i], comp))
        return combine(stage["func_a"] if alpha else stage["func_rgb"], vals)

    return tuple(channel(ch, False) for ch in range(3)) + (channel(3, True),)


def tev_expression(entry: dict):
    prev = (("undef", 0),) * 4
    for stage in entry["stage_data"]:
        prev = _stage_symbol(stage, prev, entry.get("inputs", []))
    return prev


# --------------------------------------------------------------------------
# Build

def _norm_expr(expr):
    if isinstance(expr, (list, tuple)):
        if expr and isinstance(expr[0], str):
            return [expr[0]] + [_norm_expr(x) for x in expr[1:]]
        return [_norm_expr(x) for x in expr]
    return expr


def build(tesv: str) -> dict:
    pe = PE(tesv)
    groups = {
        "vs": read_table(pe, VS_TABLE_VA, VS_TABLE_SLOTS),
        "ps": read_table(pe, PS_TABLE_VA, PS_TABLE_SLOTS),
    }
    for stage, want in EXPECTED_PREFIX.items():
        got = {row["prefix_sha256"] for row in groups[stage]}
        missing = sorted(set(want) - got)
        if missing:
            raise SystemExit("TESV does not contain the measured %s hashes: %s"
                             % (stage, missing))

    entries, refusals, ir_rows = [], [], []
    prefix_to_full = {}
    for stage in ("vs", "ps"):
        # One representative per MEASURED prefix hash; record the full-blob
        # count so the collapse is visible instead of hidden.
        by_prefix = {}
        for row in groups[stage]:
            by_prefix.setdefault(row["prefix_sha256"], []).append(row)
        for prefix in sorted(EXPECTED_PREFIX[stage]):
            rows = by_prefix[prefix]
            row = min(rows, key=lambda r: r["slot"])
            fulls = {r["full_sha256"] for r in rows}
            code = row["blob"]
            ir = C.build_ir(code)
            if ir.error:
                refusals.append({
                    "stage": stage, "prefix_sha256": prefix,
                    "class": "parse_error", "reason": ir.error,
                })
                continue
            prefix_to_full[prefix] = {
                "full_sha256": row["full_sha256"],
                "objects": len(rows),
                "distinct_full_hashes": len(fulls),
                "slot": row["slot"],
            }
            ir_rows.append({
                "prefix_sha256": prefix,
                "full_sha256": row["full_sha256"],
                "stage": stage,
                "instructions": len(ir.body),
                "version": ir.version_name,
                "inputs": ir.inputs,
                "samplers": {str(k): v for k, v in ir.samplers.items()},
                "outputs": ir.outputs,
                "defs": {str(k): list(v) for k, v in ir.defs.items()},
                "body": [{"opcode": ins.opcode, "name": ins.name,
                          "offset": ins.offset} for ins in ir.body],
            })
            if stage == "ps":
                plan, cls, why = C.plan_ps(ir)
                if plan is not None:
                    plan = dict(plan)
                    plan["id"] = "menu.gfx.ps.%s" % prefix[:8]
                    plan["sha256"] = row["full_sha256"]
                    plan["prefix_sha256"] = prefix
                    plan["stage"] = "ps_3_0"
                    plan["kind"] = "tev"
                    plan["draw_enabled"] = 0
                    plan["family"] = "ui"
                    plan["source_table"] = "0x%08x" % row["table_va"]
                    plan["source_slot"] = row["slot"]
                    plan["measurable_objects"] = len(rows)
                    plan["note"] = ("GFx menu PS translated to TEV; draw blocked "
                                    "until the c2/c3 material binding is measured")
                    plan["material_contract"] = {
                        "textures": plan.get("textures", []),
                        "constants": plan.get("constants", []),
                        "varyings": plan.get("inputs", []),
                        "runtime_binding_measured": False,
                        "complete": False,
                    }
                    entries.append(plan)
                else:
                    refusals.append({
                        "stage": stage,
                        "prefix_sha256": prefix,
                        "full_sha256": row["full_sha256"],
                        "class": cls or "unsupported",
                        "reason": why or "planner refused",
                        "objects": len(rows),
                        "source_slot": row["slot"],
                    })
            else:
                plan, cls, why = C.plan_vs(ir)
                if plan is not None:
                    plan = dict(plan)
                    plan["id"] = "menu.gfx.vs.%s" % prefix[:8]
                    plan["sha256"] = row["full_sha256"]
                    plan["prefix_sha256"] = prefix
                    plan["stage"] = "vs_3_0"
                    plan["kind"] = "pica_vs"
                    plan["draw_enabled"] = 0
                    plan["family"] = "ui"
                    plan["codegen"] = "pending"
                    plan["source_table"] = "0x%08x" % row["table_va"]
                    plan["source_slot"] = row["slot"]
                    plan["measurable_objects"] = len(rows)
                    plan["note"] = ("PICA VS descriptor for a GFx menu program; "
                                    "native codegen pending, draw disabled")
                    entries.append(plan)
                else:
                    refusals.append({
                        "stage": stage, "prefix_sha256": prefix,
                        "full_sha256": row["full_sha256"],
                        "class": cls or "unsupported",
                        "reason": why or "planner refused",
                        "objects": len(rows),
                        "source_slot": row["slot"],
                    })

    for entry in entries:
        if entry["kind"] == "tev":
            entry["plan_expression"] = _norm_expr(tev_expression(entry))
        ir_row = next(r for r in ir_rows if r["prefix_sha256"] == entry["prefix_sha256"])
        if entry["kind"] == "tev":
            _temps, final, err = C.build_value_graph(
                C.build_ir(by_prefix_code(groups["ps"], entry["prefix_sha256"])))
            ir_row["ir_expression"] = _norm_expr(final)
            ir_row["ir_value_graph_error"] = err

    doc = {
        "schema": "ui-menu-plans/1",
        "source": "TESV.exe GFx D3D9 renderer tables",
        "source_tables": {"vs": "0x%08x" % VS_TABLE_VA,
                          "ps": "0x%08x" % PS_TABLE_VA},
        "measurement": {
            "note": ("MENU_SHADERS hashes are CTAB-prefix hashes, not "
                     "full-blob hashes; the prefix is "
                     "high16(second_token)*4 bytes. Full-blob identity and the "
                     "object count are recorded per row."),
            "prefix_hash_defect": True,
            "measured_prefix_groups": {"vs": 3, "ps": 7},
            "distinct_full_blobs": {
                "vs": len({r["full_sha256"] for r in groups["vs"]}),
                "ps": len({r["full_sha256"] for r in groups["ps"]}),
            },
            "objects": {"vs": len(groups["vs"]), "ps": len(groups["ps"])},
        },
        "entries": entries,
        "refusals": refusals,
        "coverage": {
            "measured_prefix_hashes": len(EXPECTED_PREFIX["vs"]) + len(EXPECTED_PREFIX["ps"]),
            "translated": len(entries),
            "draw_enabled": sum(1 for e in entries if e["draw_enabled"] == 1),
            "refused": len(refusals),
        },
    }
    catalog = {
        "schema": "d9-shader-plan-v1",
        "archive": "TESV.exe (GFx renderer tables 0x%08x / 0x%08x)"
                   % (VS_TABLE_VA, PS_TABLE_VA),
        "note": ("Pack-owned StartMenu/GFx plans. Translated only: no entry is "
                 "draw-certified, so none sets runtime=1 and none enters the "
                 "16-slot runtime table. Hashes are the true full-blob "
                 "SHA-256; MENU_SHADERS prefix hashes are kept in "
                 "menu_plans.json."),
        "entries": [dict(entry, runtime=0) for entry in entries],
    }
    ir_doc = {
        "schema": "ui-menu-ir/1",
        "note": ("Derived IR metadata only: no bytecode. sha256 over the "
                 "canonical MojoShader d3d text is the IR fingerprint."),
        "rows": ir_rows,
        "prefix_to_full": prefix_to_full,
    }
    return doc, ir_doc, catalog


def by_prefix_code(rows, prefix):
    for row in rows:
        if row["prefix_sha256"] == prefix:
            return row["blob"]
    raise SystemExit("missing prefix %s" % prefix)


# --------------------------------------------------------------------------
# Check

def check(plans_path=PLANS_PATH, ir_path=IR_PATH, catalog_path=CATALOG_PATH,
          tesv="") -> list:
    problems = []
    with open(plans_path, encoding="utf-8") as handle:
        doc = json.load(handle)
    with open(ir_path, encoding="utf-8") as handle:
        ir_doc = json.load(handle)
    with open(catalog_path, encoding="utf-8") as handle:
        catalog = json.load(handle)
    if any(entry.get("runtime") == 1 for entry in catalog.get("entries", [])):
        problems.append("catalog: a translated menu plan claims a runtime slot")
    catalog_by_sha = {entry.get("sha256"): entry
                      for entry in catalog.get("entries", [])}
    expected_catalog = {entry["sha256"]: dict(entry, runtime=0)
                        for entry in doc.get("entries", [])}
    if catalog_by_sha != expected_catalog:
        problems.append("catalog: plans.json and menu_plans.json drifted")

    ids, shas = set(), set()
    expected = EXPECTED_PREFIX["vs"] + EXPECTED_PREFIX["ps"]
    seen_prefix = set()
    for entry in doc.get("entries", []):
        where = entry.get("id", "?")
        if entry.get("id") in ids:
            problems.append("%s: duplicate id" % where)
        ids.add(entry.get("id"))
        if entry.get("sha256") in shas:
            problems.append("%s: duplicate full sha256" % where)
        shas.add(entry.get("sha256"))
        if len(entry.get("sha256", "")) != 64:
            problems.append("%s: sha256 is not 64 hex" % where)
        if entry.get("draw_enabled") != 0:
            problems.append("%s: menu plan claims draw without certification" % where)
        prefix = entry.get("prefix_sha256")
        seen_prefix.add(prefix)
        if entry.get("stage") == "ps_3_0":
            if entry.get("kind") != "tev":
                problems.append("%s: PS is not a TEV plan" % where)
            if entry.get("stages") != len(entry.get("stage_data", [])):
                problems.append("%s: stages/stage_data mismatch" % where)
            ir_expr = None
            for row in ir_doc["rows"]:
                if row["prefix_sha256"] == prefix:
                    ir_expr = row.get("ir_expression")
            if ir_expr is None:
                problems.append("%s: no IR expression" % where)
            else:
                plan_expr = entry.get("plan_expression")
                if plan_expr != ir_expr:
                    problems.append("%s: plan expression != IR expression" % where)
            contract = entry.get("material_contract", {})
            if contract.get("complete") or contract.get("runtime_binding_measured"):
                problems.append("%s: material contract falsely complete" % where)
        elif entry.get("stage") == "vs_3_0":
            if entry.get("kind") != "pica_vs":
                problems.append("%s: VS is not a PICA descriptor" % where)
            if len(entry.get("outputs", [])) > 7:
                problems.append("%s: more than 7 PICA varyings" % where)
            if entry.get("uniform_registers", 0) > 96:
                problems.append("%s: uniform registers out of range" % where)
        else:
            problems.append("%s: bad stage %r" % (where, entry.get("stage")))

    for refusal in doc.get("refusals", []):
        if not refusal.get("class") or not refusal.get("reason"):
            problems.append("refusal %s: not named" % refusal.get("prefix_sha256"))
        seen_prefix.add(refusal.get("prefix_sha256"))
    if seen_prefix != set(expected):
        problems.append("coverage: missing %s" % sorted(set(expected) - seen_prefix))

    cov = doc.get("coverage", {})
    if cov.get("translated") != len(doc.get("entries", [])):
        problems.append("coverage: translated count drift")
    if cov.get("refused") != len(doc.get("refusals", [])):
        problems.append("coverage: refusal count drift")
    if len(doc.get("entries", [])) + len(doc.get("refusals", [])) != 10:
        problems.append("coverage: 10 measured hashes do not partition")

    # IR rows must carry the same full hashes as the plans/refusals.
    for row in ir_doc.get("rows", []):
        match = [e for e in doc["entries"] if e.get("prefix_sha256") == row["prefix_sha256"]]
        if match and match[0].get("sha256") != row["full_sha256"]:
            problems.append("%s: plan full hash != IR full hash"
                            % row["prefix_sha256"][:8])

    if tesv:
        fresh, fresh_ir, _fresh_catalog = build(tesv)
        old = {(e["prefix_sha256"]): e for e in doc["entries"]}
        new = {(e["prefix_sha256"]): e for e in fresh["entries"]}
        if set(old) != set(new):
            problems.append("symbolic: entry set differs from fresh extraction")
        for prefix, entry in old.items():
            other = new.get(prefix)
            if other is None:
                continue
            for key in ("sha256", "id", "stage", "kind", "draw_enabled",
                        "stages", "stage_data", "textures", "constants",
                        "outputs", "inputs", "uniform_registers",
                        "plan_expression"):
                if entry.get(key) != other.get(key):
                    problems.append("%s: %s differs from retail regeneration"
                                    % (entry["id"], key))
        if [r["full_sha256"] for r in fresh_ir["rows"]] != \
                [r["full_sha256"] for r in ir_doc["rows"]]:
            problems.append("symbolic: IR full hashes differ from retail")
    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_build = sub.add_parser("build")
    p_build.add_argument("--tesv", required=True)
    p_check = sub.add_parser("check")
    p_check.add_argument("--tesv", default="")
    args = parser.parse_args(argv)

    if args.cmd == "build":
        doc, ir_doc, catalog = build(args.tesv)
        with open(PLANS_PATH, "w", encoding="utf-8") as handle:
            json.dump(doc, handle, indent=1, sort_keys=True)
            handle.write("\n")
        with open(IR_PATH, "w", encoding="utf-8") as handle:
            json.dump(ir_doc, handle, indent=1, sort_keys=True)
            handle.write("\n")
        with open(CATALOG_PATH, "w", encoding="utf-8") as handle:
            json.dump(catalog, handle, indent=1, sort_keys=True)
            handle.write("\n")
        print("wrote %s, %s and %s" % (
            os.path.basename(PLANS_PATH), os.path.basename(IR_PATH),
            os.path.basename(CATALOG_PATH)))
        print(json.dumps(doc["coverage"], indent=2))
        return 0

    problems = check(tesv=args.tesv)
    print(json.dumps({"ok": not problems, "problems": problems}, indent=2))
    return 0 if not problems else 1


if __name__ == "__main__":
    raise SystemExit(main())
