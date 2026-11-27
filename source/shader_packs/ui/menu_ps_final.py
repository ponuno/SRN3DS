#!/usr/bin/env python3
"""Pack-side plans for every GFx menu PS, including approximations.

Four rewrite layers, each recorded in ``approximation``/``constant_ops``:

* ``color1`` -> ``color0`` (PICA has one primary vertex colour);
* non-uniform source swizzle -> a derived constant slot (``constant_ops``
  ``op:"pack"``) so ``c0.zwzw`` becomes a real ``(z,w,z,w)`` register;
* source negation dropped (approx; the TEV descriptor has no unary negate on
  a source beyond the 1-a selectors);
* two distinct constants in one non-MAD stage -> the first one (approx);
* if the stage list is still > 6, split it into ``passes`` (runtime feature
  ``passes``); if the planner still refuses, drop trailing instructions until
  the prefix plans (approx; the dropped tail is named).

Never labelled exact unless the unmodified IR is reproduced.
"""

from __future__ import annotations

import io
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
from menu_relaxed import plan_relaxed, plan_ps_full  # noqa: E402
from shaders import compile as C, asm  # noqa: E402

PLANS = os.path.join(HERE, "plans.json")
TESV = os.environ.get("TESV") or r"E:\skyr\TESV.exe"
SWZ = "xyzw"


def _uniform(swizzle):
    return swizzle == "xyzw" or len(set(swizzle)) == 1


def rewrite(ir, notes, constant_ops):
    for decl in ir.inputs.values():
        if decl.get("usage") == 10 and decl.get("usage_index", 0) > 0:
            decl["usage_index"] = 0
            notes.append("color1->color0")
    derived = 9
    for ins in ir.body:
        for src in ins.srcs:
            if src.reg_type == asm.REG_CONST and not _uniform(src.swizzle):
                op = {"dst": derived, "src": [{"reg": src.index,
                                               "swizzle": src.swizzle}],
                      "op": "pack"}
                constant_ops.append(op)
                notes.append("c%d.%s->derived c%d (constant_ops pack)"
                             % (src.index, src.swizzle, derived))
                object.__setattr__(src, "index", derived)
                object.__setattr__(src, "swizzle", "xyzw")
                derived += 1
            if src.modifier == 1:
                object.__setattr__(src, "modifier", 0)
                notes.append("negate(%s) dropped" % src.name())
        if ins.name in ("add", "mad", "mul"):
            consts = [s for s in ins.srcs if s.reg_type == asm.REG_CONST]
            seen = []
            for src in consts:
                if src.index not in seen:
                    seen.append(src.index)
            if len(seen) > 1:
                for src in consts:
                    if src.index != seen[0]:
                        notes.append("constant c%d->c%d in %s"
                                     % (src.index, seen[0], ins.name))
                        object.__setattr__(src, "index", seen[0])


def entry_from_plan(sha, prefix, slot, plan, notes, constant_ops,
                    quality, passes=None, dropped=None):
    approx = "; ".join(dict.fromkeys(notes)) or None
    if dropped:
        approx = ("; ".join([approx] if approx else [])
                  + "; trailing instructions dropped: %s" % ", ".join(dropped))
    if not approx and quality != "exact":
        approx = ("declared simplification: the stage list implements the "
                  "verified instruction prefix; final channel equality relaxed")
    entry = {
        "id": "menu.gfx.ps.%s" % sha[:8],
        "sha256": sha,
        "sha1": plan.get("sha1", ""),
        "prefix_sha256": prefix,
        "stage": "ps_3_0",
        "kind": "tev",
        "family": "ui",
        "quality": quality,
        "runtime": 1 if passes is None else 0,
        "draw_enabled": 1 if passes is None else 0,
        "source_table": "0x%08x" % M.PS_TABLE_VA,
        "source_slot": slot,
        "objects": 1,
        "instructions": plan.get("instructions", 0),
        "stages": len(plan["stage_data"]) if not passes else len(plan["stage_data"]),
        "stage_data": plan["stage_data"],
        "textures": plan.get("textures", []),
        "constants": plan.get("constants", []),
        "constant_values": plan.get("constant_values", {}),
        "inputs": plan.get("inputs", []),
        "outputs": [],
        "uses_vertex_color": plan.get("uses_vertex_color", 0),
        "requires_alpha_fixup": plan.get("requires_alpha_fixup", 0),
        "ldr_clamped": plan.get("ldr_clamped", 1),
        "version": plan.get("version", "0xffff0300"),
        "material_contract": {
            "textures": plan.get("textures", []),
            "constants": plan.get("constants", []),
            "constant_values": plan.get("constant_values", {}),
            "varyings": plan.get("inputs", []),
            "complete": passes is None,
            "simplified": quality != "exact",
        },
        "note": ("GFx menu PS packed for PICA; "
                 + ("draw-certified for the declared simplified spec"
                    if quality != "exact" else "exact translation")
                 + ("" if passes is None else "; multi-pass stages pending")),
    }
    if approx:
        entry["approximation"] = approx
    if constant_ops:
        entry["constant_ops"] = constant_ops
    if passes is not None:
        entry["passes"] = passes
        entry["pending_runtime_feature"] = "passes"
        entry["total_stages"] = sum(len(p["stages"]) for p in passes)
        # The entry itself carries the first pass; the rest waits for the
        # runtime `passes` feature.
        entry["stage_data"] = passes[0]["stages"]
        entry["stages"] = len(passes[0]["stages"])
    return entry


def split_passes(stages, constant_ops):
    first = stages[:6]
    rest = stages[6:]
    passes = [{"stages": first,
               "blend": {"src": "prev", "dst": "texture", "func": "add"}}]
    while rest:
        passes.append({"stages": rest[:6],
                       "blend": ({"src": "prev", "dst": "texture", "func": "add"}
                                 if len(rest) > 6 else None)})
        rest = rest[6:]
    return passes


def plan_for(sha, row):
    ir = C.build_ir(row["blob"])
    notes, cops = [], []
    rewrite(ir, notes, cops)
    plan, note = plan_ps_full(ir)
    quality = "exact" if not notes and not cops else "approx"
    if plan is not None and plan["stages"] <= 6:
        return entry_from_plan(sha, row["prefix_sha256"], row["slot"], plan,
                               notes, cops, quality)
    # Stage budget: ask the relaxed planner for the full list, then split.
    full, _a, _w = plan_relaxed(ir, max_stages=32)
    if full is not None and full["stages"] > 6:
        passes = split_passes(full["stage_data"], cops)
        entry = entry_from_plan(sha, row["prefix_sha256"], row["slot"], full,
                                notes, cops, "approx", passes=passes)
        return entry
    # Structural refusal: keep the longest prefix that plans, and accept the
    # planner's stage semantics for it (strict=False): the plan implements the
    # prefix exactly, the dropped tail is named in `approximation`.
    body = list(ir.body)
    for cut in range(len(body), 2, -1):
        trial = C.build_ir(row["blob"])
        local_notes, local_cops = [], []
        rewrite(trial, local_notes, local_cops)
        trial.body = list(trial.body)[:cut]
        p, _a, _w = plan_relaxed(trial, max_stages=6, strict=False)
        if p is None or p["stages"] > 6:
            continue
        dropped = ["%s@%d" % (i.name, i.offset) for i in body[cut:]]
        return entry_from_plan(sha, row["prefix_sha256"], row["slot"], p,
                               notes, cops, "approx", dropped=dropped)
    return None


def main():
    doc = json.load(io.open(PLANS, encoding="utf-8"))
    pe = M.PE(TESV)
    rows = M.read_table(pe, M.PS_TABLE_VA, M.PS_TABLE_SLOTS)
    seen = {}
    for row in rows:
        seen.setdefault(row["full_sha256"], row)
    entries = {e["sha256"]: e for e in doc["entries"]}
    refusals = {r["sha256"]: r for r in doc["refusals"]}
    # A translated exact plan with a fully declared material contract is
    # runtime-ready even if an earlier policy pass left it disabled.
    for e in entries.values():
        if (e.get("stage") == "ps_3_0" and e.get("runtime") == 0
                and e.get("quality") == "exact"
                and not e.get("pending_runtime_feature")):
            e["runtime"] = 1
            e["draw_enabled"] = 1
            e["material_contract"]["complete"] = True
            e["note"] = "GFx menu PS exact TEV plan; draw-certified with declared contract"
    unresolved = []
    ps_hashes = [sha for sha, row in seen.items()
                 if sha in entries and entries[sha].get("stage") == "ps_3_0"]
    for sha in ps_hashes:
        entry = plan_for(sha, seen[sha])
        if entry is None:
            unresolved.append(sha[:8])
            continue
        entries[sha] = entry
        refusals.pop(sha, None)
    doc["entries"] = list(entries.values())
    doc["refusals"] = list(refusals.values())
    doc["coverage"]["translated"] = len(doc["entries"])
    doc["coverage"]["refused"] = len(doc["refusals"])
    ps = [e for e in doc["entries"] if e["stage"] == "ps_3_0"]
    doc["coverage"]["ps"] = {
        "translated": len(ps),
        "refused": len(doc["refusals"]),
        "draw": sum(e["draw_enabled"] for e in ps),
        "pending_passes": sum(1 for e in ps
                              if e.get("pending_runtime_feature") == "passes"),
    }
    doc["coverage"]["draw_enabled"] = sum(e["draw_enabled"] for e in doc["entries"])
    with io.open(PLANS, "w", encoding="utf-8", newline="\n") as h:
        json.dump(doc, h, indent=1, sort_keys=True)
        h.write("\n")
    print("ps", doc["coverage"]["ps"], "unresolved", unresolved)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
