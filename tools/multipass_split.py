#!/usr/bin/env python3
"""Feasibility probe for multi-pass TEV plans on multi-texture lighting PS.

``d3d9_draw_plan_passes`` replays the same draw once per pass with the same
two device textures (``d->texture``/``d->texture1``) and only varies the TEV
descriptor, blend and depth state.  A pass therefore cannot rebind a third or
seventh sampler.  This tool records, per refused 10-chunk lighting hash:

* the distinct sampler registers the PS actually reads;
* the best two-texture scheduler plan and its measured IR-vs-TEV error;
* how many passes a texture split would need;
* whether the current runtime can execute it.

It deliberately does not change draw flags.  A plan whose only executable form
drops textures is not a runtime path.

Run::

    python tools/multipass_split.py --data E:\\SteamLibrary\\steamapps\\common\\Skyrim\\Data
"""

from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
for _path in (REPO, os.path.join(REPO, "tools"),
              os.path.join(REPO, "tools", "shaders")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

from shaders import compile as C  # noqa: E402
import tev_sched as T  # noqa: E402
import vs_contract_emit as V  # noqa: E402

PACK = "lighting"
OUT = os.path.join(REPO, "source", "shader_packs",
                   "multipass_split_report.json")
SAMPLES = 256
#: d9d9_pica_tev / d9d9_pica_citro3d bind two texture units.
DEVICE_TEXTURES = 2
#: A split plan is only meaningful if the two-texture approximation keeps the
#: LDR result close; the lighting approx tests accept 1.0 but 0.86 is a
#: different shader, so this probe uses a stricter review threshold.
MEANINGFUL_ERROR = 0.10


def find_irs(hashes, data_dir):
    from fxp_compile import _load_packages

    out = {}
    for package in _load_packages(V.find_data(data_dir)):
        for chunk in package.chunks:
            if chunk.kind != "ps":
                continue
            ir = C.build_ir(chunk.code)
            if ir.sha256 in hashes and ir.sha256 not in out:
                out[ir.sha256] = ir
    return out


def sampler_regs(ir):
    regs = set()
    for ins in ir.body:
        if ins.opcode == 0x42:  # texld
            regs.add(ins.srcs[1].index)
    return sorted(regs)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default="")
    args = parser.parse_args(argv)

    manifest = json.load(open(os.path.join(
        REPO, "source", "shader_packs", PACK, "manifest.json"),
        encoding="utf-8"))
    plans = json.load(open(os.path.join(
        REPO, "source", "shader_packs", PACK, "plans.json"), encoding="utf-8"))
    have = {e["sha256"] for e in plans["entries"]}
    rows = [row for row in manifest["hashes"]
            if row[1] == "ps" and row[2] == "def_literal_out_of_range"
            and row[3] == 10 and row[0] not in have]
    irs = find_irs({row[0] for row in rows}, args.data)
    report = []
    for row in rows:
        sha = row[0]
        ir = irs.get(sha)
        if ir is None:
            continue
        regs = sampler_regs(ir)
        entry = T.plan_shader(ir, family=PACK, approx_only=True,
                              samples=SAMPLES, max_textures=DEVICE_TEXTURES)
        error = None
        stages = textures = None
        if entry is not None:
            err = entry.get("approx_error") or entry.get("verification") or {}
            error = err.get("max_abs_error")
            stages = entry["stages"]
            textures = len(entry["textures"])
        passes_needed = (len(regs) + DEVICE_TEXTURES - 1) // DEVICE_TEXTURES
        executable = (len(regs) <= DEVICE_TEXTURES and error is not None
                      and error <= MEANINGFUL_ERROR and stages is not None
                      and stages <= 6)
        reasons = []
        if len(regs) > DEVICE_TEXTURES:
            reasons.append("d9d9_draw_plan_passes reuses d->texture/"
                           "d->texture1 for every pass; %d samplers cannot be "
                           "rebound per pass" % len(regs))
        if error is not None and error > MEANINGFUL_ERROR:
            reasons.append("best %d-texture plan error %.3f > %.2f"
                           % (DEVICE_TEXTURES, error, MEANINGFUL_ERROR))
        report.append({
            "id": "multipass.%s" % sha[:8], "sha256": sha,
            "chunks": row[3], "samplers": ["s%d" % r for r in regs],
            "sampler_count": len(regs),
            "device_textures": DEVICE_TEXTURES,
            "passes_needed": passes_needed,
            "two_texture_plan": {"stages": stages, "textures": textures,
                                 "max_abs_error": error,
                                 "samples": SAMPLES},
            "executable": executable,
            "reasons": reasons,
        })
    doc = {"schema": "multipass-split/1", "samples": SAMPLES,
           "meaningful_error": MEANINGFUL_ERROR,
           "device_textures": DEVICE_TEXTURES, "entries": report}
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(doc, handle, indent=1, sort_keys=True)
        handle.write("\n")
    for row in report:
        print("%-18s samplers=%s passes=%d err=%.3f executable=%s"
              % (row["id"], ",".join(row["samplers"]), row["passes_needed"],
                 row["two_texture_plan"]["max_abs_error"] or -1,
                 row["executable"]))
        for reason in row["reasons"]:
            print("    - %s" % reason)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
