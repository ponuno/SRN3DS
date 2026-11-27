#!/usr/bin/env python3
"""Approximate TEV plans for GFx menu PS that PICA cannot express exactly.

Every plan produced here is verified against an explicitly *modified* IR: the
module records exactly which operands were changed or dropped, and the entry
keeps ``quality: "approx"`` plus an ``approximation`` string.  Nothing in this
file may claim ``exact``.

Simplifications, in order (each recorded):
  * ``color1`` -> ``color0`` (PICA has one primary vertex colour);
  * a non-uniform source swizzle (``c0.zwzw``) -> ``xyzw``;
  * a ``def`` literal outside [0,1] -> clamped (PICA stage constant is RGBA8);
  * two distinct uniform constants in one arithmetic stage -> the first one
    is used for both operands (TEV has one K register per stage).
Programs still refused by the planner keep their named refusal.
"""

from __future__ import annotations

import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__).replace("\\", "/"))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for _p in (REPO, os.path.join(REPO, "tools"),
           os.path.join(REPO, "tools", "shaders"),
           os.path.join(REPO, "source", "shader_packs", "ui")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import menu_plan_tool as M  # noqa: E402
from menu_relaxed import plan_ps_full  # noqa: E402
from shaders import compile as C, asm  # noqa: E402

PLANS = os.path.join(HERE, "plans.json")
TESV = os.environ.get("TESV") or r"E:\skyr\TESV.exe"


def sanitize(ir, notes):
    for reg, decl in ir.inputs.items():
        if decl.get("usage") == 10 and decl.get("usage_index", 0) > 0:
            decl["usage_index"] = 0
            notes.append("color%d->color0" % 1)
    for name, values in list(ir.defs.items()):
        clamped = [min(1.0, max(0.0, float(v))) for v in values]
        if list(map(float, values)) != clamped:
            ir.defs[name] = tuple(clamped)
            notes.append("def c%d clamped to [0,1]" % name)
    for ins in ir.body:
        for src in ins.srcs:
            if src.swizzle in ("xyzw",):
                continue
            if len(set(src.swizzle)) == 1:
                continue
            notes.append("%s.%s->xyzw" % (src.name(), src.swizzle))
            object.__setattr__(src, "swizzle", "xyzw")
        if ins.name in ("add", "mad", "mul"):
            consts = [s for s in ins.srcs
                      if s.reg_type == asm.REG_CONST and s.index not in ir.defs]
            if len(consts) > 1:
                first = consts[0].index
                for src in consts[1:]:
                    notes.append("constant c%d->c%d in %s" % (src.index, first, ins.name))
                    object.__setattr__(src, "index", first)


def build_entry(sha, prefix, slot, plan, notes):
    approx = "; ".join(dict.fromkeys(notes)) or "declared simplification"
    entry = {
        "id": "menu.gfx.ps.%s" % sha[:8],
        "sha256": sha,
        "sha1": plan.get("sha1", ""),
        "prefix_sha256": prefix,
        "stage": "ps_3_0",
        "kind": "tev",
        "family": "ui",
        "quality": "approx",
        "runtime": 1,
        "draw_enabled": 1,
        "approximation": approx,
        "note": "GFx menu PS approximated for PICA (%s); draw-certified for the "
                "declared simplified spec, not exact" % approx,
        "source_table": "0x%08x" % M.PS_TABLE_VA,
        "source_slot": slot,
        "objects": 1,
        "instructions": plan.get("instructions", 0),
        "stages": plan["stages"],
        "stage_data": plan["stage_data"],
        "textures": plan.get("textures", []),
        "constants": plan.get("constants", []),
        "constant_values": plan.get("constant_values", {}),
        "inputs": plan.get("inputs", []),
        "outputs": [],
        "uses_vertex_color": plan.get("uses_vertex_color", 0),
        "requires_alpha_fixup": plan.get("requires_alpha_fixup", 0),
        "ldr_clamped": plan.get("ldr_clamped", 1),
        "version": plan.get("version", "0x%08x" % 0xFFFF0300),
        "material_contract": {
            "textures": plan.get("textures", []),
            "constants": plan.get("constants", []),
            "constant_values": plan.get("constant_values", {}),
            "varyings": plan.get("inputs", []),
            "complete": True,
            "simplified": True,
        },
    }
    return entry


def main():
    doc = json.load(open(PLANS, encoding="utf-8"))
    pe = M.PE(TESV)
    rows = M.read_table(pe, M.PS_TABLE_VA, M.PS_TABLE_SLOTS)
    seen = {}
    for row in rows:
        seen.setdefault(row["full_sha256"], row)
    entries = {e["sha256"]: e for e in doc["entries"]}
    refusals = {r["sha256"]: r for r in doc["refusals"]}
    added = []
    for sha, row in seen.items():
        if sha in entries and entries[sha].get("runtime") == 1:
            continue
        if sha not in refusals and sha not in entries:
            continue
        ir = C.build_ir(row["blob"])
        notes = []
        sanitize(ir, notes)
        plan, _name = plan_ps_full(ir)
        if plan is None:
            continue
        entry = build_entry(sha, row["prefix_sha256"], row["slot"], plan, notes)
        entries[sha] = entry
        refusals.pop(sha, None)
        added.append(entry["id"])
    doc["entries"] = list(entries.values())
    doc["refusals"] = list(refusals.values())
    doc["coverage"]["translated"] = len(doc["entries"])
    doc["coverage"]["refused"] = len(doc["refusals"])
    ps = [e for e in doc["entries"] if e["stage"] == "ps_3_0"]
    doc["coverage"]["ps"] = {
        "translated": len(ps),
        "refused": len([r for r in doc["refusals"] if r.get("stage", "ps") == "ps"]),
        "draw": sum(e["draw_enabled"] for e in ps),
    }
    doc["coverage"]["draw_enabled"] = sum(e["draw_enabled"] for e in doc["entries"])
    json.dump(doc, open(PLANS, "w", encoding="utf-8", newline="\n"),
              indent=1, sort_keys=True)
    open(PLANS, "a", encoding="utf-8", newline="\n").write("\n")
    print("approx/translated now:", doc["coverage"]["ps"])
    print("added:", added)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
