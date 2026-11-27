#!/usr/bin/env python3
"""Pack a D3D9 VS output contract into the PICA VS output budget.

PICA VS exposes **seven** output registers.  One is clip position; the other
six carry varyings, and picasso names their semantics::

    texcoord0, texcoord0w, texcoord1, texcoord2, color, view

so the portable varying budget is **24 components**.  The DSL contracts in
this pack declare up to nine varyings (32 components) and use texcoord3+,
color1 and normals in slots the PICA fragment stage cannot name.

``apply(plan, ir)`` rewrites ``plan["outputs"]`` to the physical PICA outputs
and records a component map in ``plan["packing"]``:

* contracts with <= 6 output vectors keep one whole vector per D3D output;
  the map is lossless and direct (same component index);
* contracts with > 6 vectors drop the second raster colour (a PICA fragment
  stage has one) and pack the remaining written components into the 24 lanes.
  Unwritten components are not carried; the packing is otherwise lossless.
* a block that does not fit a contiguous run in one physical register is
  *scattered* component by component; the emitter accumulates it in a free
  temporary and writes the lanes in the epilogue.  No quantisation is used.

If the written varying components exceed 24 after the sanctioned drop, the
caller gets a named physical refusal (``pica_vs_output_components``).
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
for _path in (HERE, REPO, os.path.join(REPO, "tools"),
              os.path.join(REPO, "tools", "shaders")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

from shaders import asm  # noqa: E402

#: (picasso semantic, emitted register name), in allocation order.
PHYSICAL = [
    ("texcoord0", "outtc0"),
    ("texcoord0w", "outtc0w"),
    ("texcoord1", "outtc1"),
    ("texcoord2", "outtc2"),
    ("color", "outclr"),
    ("view", "outview"),
]
CAPACITY = 4 * len(PHYSICAL)
#: Preferred semantic for a D3D usage, so committed representative programs
#: keep their exact bytes.
PREFERRED = {(5, 0): "texcoord0", (5, 1): "texcoord1", (5, 2): "texcoord2",
             (10, 0): "color", (10, 1): "view"}
OUTPUT_TYPES = (asm.REG_OUTPUT, asm.REG_RASTOUT, asm.REG_ATTROUT)


class PackingError(Exception):
    pass


def _written_masks(ir) -> dict:
    masks = {}
    for ins in ir.body:
        dst = ins.dst
        if dst is not None and dst.reg_type in OUTPUT_TYPES:
            masks[dst.index] = masks.get(dst.index, 0) | dst.mask
    return masks


def _physical_index(name: str) -> int:
    for index, (semantic, _out) in enumerate(PHYSICAL):
        if semantic == name:
            return index
    raise PackingError("unknown physical output %r" % name)


def apply(plan: dict, ir) -> dict:
    """Rewrite the plan in place; return a summary.  Raises PackingError."""
    declared = plan["outputs"]
    masks = _written_masks(ir)
    varyings = [d for d in declared if d["usage"] != 0]
    out_regs = [d for d in declared if d["usage"] == 0]
    if len(out_regs) != 1:
        raise PackingError("expected exactly one position output")

    # --- whole-vector path: <= 6 varying vectors ---------------------------
    if len(varyings) <= len(PHYSICAL):
        used = {"position"}
        mapping = {}
        physical = []
        pos = out_regs[0]
        physical.append({"reg_index": pos["reg_index"], "type": pos["type"],
                         "usage": 0, "usage_index": 0,
                         "usage_name": "position",
                         "physical": "outpos", "semantic": "position"})
        mapping[(pos["type"], pos["reg_index"])] = [
            {"src": c, "out": "outpos", "dst": c}
            for c in range(4) if masks.get(pos["reg_index"], 0) & (1 << c)]
        for item in varyings:
            key = (item["usage"], item["usage_index"])
            name = PREFERRED.get(key)
            if name is None or name in used:
                name = next((sem for sem, _o in PHYSICAL if sem not in used),
                            None)
            if name is None:
                raise PackingError("no free physical varying for %s%d"
                                   % (item["usage_name"], item["usage_index"]))
            used.add(name)
            out = next(o for sem, o in PHYSICAL if sem == name)
            physical.append({"reg_index": item["reg_index"],
                             "type": item["type"], "usage": item["usage"],
                             "usage_index": item["usage_index"],
                             "usage_name": item["usage_name"],
                             "physical": out, "semantic": name})
            mapping[(item["type"], item["reg_index"])] = [
                {"src": c, "out": out, "dst": c}
                for c in range(4)
                if masks.get(item["reg_index"], 0) & (1 << c)]
        return _commit(plan, declared, physical, mapping, [], "exact")

    # --- component path: drop color1, then pack 24 lanes -------------------
    dropped = [d for d in varyings if d["usage"] == 10
               and d["usage_index"] == 1]
    if not dropped:
        raise PackingError("over-budget contract has no second colour to drop")
    keep = [d for d in varyings if d not in dropped]
    blocks = []
    for item in keep:
        mask = masks.get(item["reg_index"], 0)
        comps = [c for c in range(4) if mask & (1 << c)]
        if comps:
            blocks.append((item, comps))
    total = sum(len(c) for _i, c in blocks) + sum(
        len([c for c in range(4) if masks.get(d["reg_index"], 0) & (1 << c)])
        for d in dropped)
    kept = sum(len(c) for _i, c in blocks)
    if kept > CAPACITY:
        raise PackingError("pica_vs_output_components: %d > %d"
                           % (kept, CAPACITY))

    # free[semantic] = 4-bit mask of free components.
    free = {sem: 0xF for sem, _o in PHYSICAL}
    mapping = {}
    scattered = []

    def place(item, comps):
        best = None
        for sem, _o in PHYSICAL:
            for start in range(0, 5 - len(comps)):
                want = ((1 << len(comps)) - 1) << start
                if free[sem] & want == want:
                    best = (sem, start)
                    break
            if best:
                break
        if best is None:
            return False
        sem, start = best
        free[sem] &= ~(((1 << len(comps)) - 1) << start)
        mapping[(item["type"], item["reg_index"])] = [
            {"src": src, "out": next(o for s, o in PHYSICAL if s == sem),
             "dst": start + i}
            for i, src in enumerate(comps)]
        return True

    # Largest blocks first, deterministic order for regeneration.
    pending = sorted(blocks, key=lambda b: (-len(b[1]), b[0]["reg_index"]))
    for item, comps in pending:
        if not place(item, comps):
            scattered.append((item, comps))

    # Scatter the leftovers into any free lane.
    for item, comps in scattered:
        entries = []
        for src in comps:
            lane = next(((sem, c) for sem, _o in PHYSICAL
                         for c in range(4) if free[sem] & (1 << c)), None)
            if lane is None:
                raise PackingError("pica_vs_output_components: no free lane")
            sem, comp = lane
            free[sem] &= ~(1 << comp)
            entries.append({"src": src,
                            "out": next(o for s, o in PHYSICAL if s == sem),
                            "dst": comp, "scatter": 1})
        mapping[(item["type"], item["reg_index"])] = entries

    physical = [{"reg_index": out_regs[0]["reg_index"],
                 "type": out_regs[0]["type"], "usage": 0,
                 "usage_index": 0, "usage_name": "position",
                 "physical": "outpos", "semantic": "position"}]
    for sem, out in PHYSICAL:
        if free[sem] == 0xF:
            continue
        physical.append({"reg_index": 0, "type": asm.REG_OUTPUT, "usage": 5,
                         "usage_index": _physical_index(sem),
                         "usage_name": "packed", "physical": out,
                         "semantic": sem})
    drop_info = [{"type": d["type"], "reg_index": d["reg_index"],
                  "usage": d["usage"], "usage_index": d["usage_index"],
                  "usage_name": d["usage_name"],
                  "components": [c for c in range(4)
                                 if masks.get(d["reg_index"], 0) & (1 << c)],
                  "reason": "second_raster_colour_has_no_pica_output"}
                 for d in dropped]
    return _commit(plan, declared, physical, mapping, drop_info, "approx",
                   kept=kept, total=total)


def _commit(plan, declared, physical, mapping, dropped, quality,
            kept=0, total=0):
    components = []
    for key in sorted(mapping):
        for entry in mapping[key]:
            row = dict(entry)
            row["reg_type"] = key[0]
            row["reg_index"] = key[1]
            components.append(row)
    plan["contract_outputs"] = list(declared)
    plan["outputs"] = physical
    plan["packing"] = {"components": components, "dropped": dropped,
                       "capacity": CAPACITY}
    if quality == "approx":
        plan["quality"] = "approx"
        plan["approximation"] = (
            "packed %d varying component(s) into the %d-component PICA VS "
            "output budget; second raster colour (color1) dropped at the "
            "sanctioned fallback because PICA has one fragment colour "
            "(declared %d components, kept %d)"
            % (kept, CAPACITY, total, kept))
    else:
        plan["quality"] = "exact"
    return {"components": len(components), "dropped": len(dropped),
            "physical_outputs": len(physical), "quality": quality,
            "kept": kept, "total": total}


__all__ = ["apply", "PackingError", "PHYSICAL", "CAPACITY"]
