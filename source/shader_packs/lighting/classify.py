"""Classify the lighting pack by the real shader form, not by the census class.

The phase-1 manifest says 4,804 hashes are ``def_literal_out_of_range``.  That
is the *first* refusal of a single planner, not a shape.  This script groups the
same 4,991 hashes by what the bytecode actually is:

* ``base_lit``            p5/p1200005 diffuse+normal+directional light, no
                          point light, no shadow, no loop;
* ``base_lit_shadow``     the same with one projected shadow-mask sample;
* ``lit_specular``        SPECULAR constants present;
* ``lit_envmap``          ENV/ENVMASK constants present;
* ``lit_shadow``          shadow mask only (no loop/rcp/cmp);
* ``lit_subsurface``      SubSurfaceSampler (skin/SSS permutation);
* ``lit_backlight``       BackLightMaskSampler (flag permutation);
* ``lit_parallax``        MultiLayerParallax (multi-layer material);
* ``lit_projected``       ProjectedUV/Noise (deferred/decal pass);
* ``lit_glow_sparkle``    Glow/Sparkle particles;
* ``lit_pointlight``      no loop, but rcp/rsq attenuation;
* ``lit_loops``           rep/if/ifc control flow (the bulk);
* ``lit_other``           remaining pixel shaders;
* ``vs_*``                vertex shaders by the PICA VS envelope.

The output is derived metadata (hashes and counts); no game bytes are stored.
Run: ``python source/shader_packs/lighting/classify.py``.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tools.shaders import asm, compile as C  # noqa: E402
import lighting_plans as LP  # noqa: E402

CONTROL = {"rep", "endrep", "loop", "endloop", "if", "ifc", "else", "endif",
           "break", "breakc", "call", "callnz", "ret", "label"}
FEATURES = {
    "shadow": ("ShadowMaskSampler",),
    "envmap": ("EnvSampler", "EnvmapData", "EnvMaskSampler"),
    "specular": ("SpecularColor", "SpecularSampler"),
    "subsurface": ("SubSurfaceSampler",),
    "backlight": ("BackLightMaskSampler",),
    "parallax": ("MultiLayerParallaxData", "MultiLayerParallaxSampler"),
    "projected": ("ProjectedUVParams", "ProjectedUVParams2",
                  "ProjectedNoiseSampler"),
    "glow": ("GlowSampler", "SparkleSampler", "SparkleParams"),
    "point": ("PointLightPosition", "PointLightColor"),
}


def load_chunks(path):
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 0)[0]
    pos, out = 4, []
    for _ in range(count):
        size = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        out.append(data[pos:pos + size])
        pos += size
    return out


def ps_form(ir, plan_shadow):
    names = {asm.OPCODE_NAMES.get(i.opcode, "") for i in ir.body}
    ctab = {c.name for c in ir.program.constants}
    has = {key: bool(ctab & set(vals)) for key, vals in FEATURES.items()}
    if plan_shadow is not None:
        return "base_lit_shadow", has
    if LP.base_lit_plan(ir) is not None or C.menu_light_signature(ir):
        return "base_lit", has
    control = bool(names & CONTROL)
    if has["glow"] or has["projected"]:
        return "lit_glow_sparkle" if has["glow"] else "lit_projected", has
    if has["parallax"]:
        return "lit_parallax", has
    if has["backlight"]:
        return "lit_backlight", has
    if has["subsurface"]:
        return "lit_subsurface", has
    if has["envmap"]:
        return "lit_envmap", has
    if has["specular"]:
        return "lit_specular", has
    if control:
        return "lit_loops", has
    if has["point"] and ({"rcp", "rsq"} & names):
        return "lit_pointlight", has
    if has["shadow"]:
        return "lit_shadow", has
    if {"rcp", "rsq"} & names:
        return "lit_pointlight", has
    return "lit_other", has


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", default=r"E:\host_sm3")
    parser.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "coverage.json"))
    args = parser.parse_args(argv)
    with open(os.path.join(args.corpus, "corpus.json"), encoding="utf-8") as handle:
        corpus = json.load(handle)
    ps = load_chunks(os.path.join(args.corpus, "ps_chunks.bin"))
    vs = load_chunks(os.path.join(args.corpus, "vs_chunks.bin"))
    rows = [r for r in corpus["rows"]
            if r["family"] in ("lighting/deferred/shadows", "particles/effects")]
    counts = collections.Counter(r["sha256"] for r in rows)
    first = {}
    for row in rows:
        first.setdefault(row["sha256"], row)
    class_by_sha = {}
    manifest_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "manifest.json")
    if os.path.isfile(manifest_path):
        with open(manifest_path, encoding="utf-8") as handle:
            for row in json.load(handle)["hashes"]:
                class_by_sha[row[0]] = row[2]

    forms = collections.defaultdict(
        lambda: {"hashes": 0, "chunks": 0, "examples": [],
                 "refusal_classes": collections.Counter()})
    feature_counts = collections.Counter()
    feature_chunks = collections.Counter()
    for sha, row in first.items():
        if row["kind"] == "ps":
            ir = C.build_ir(ps[row["index"]])
            plan, shadow = LP.recognise(ir)
            form, has = ps_form(ir, plan if shadow else None)
            for key, value in has.items():
                if value:
                    feature_counts[key] += 1
                    feature_chunks[key] += counts[sha]
        else:
            ir = C.build_ir(vs[row["index"]])
            plan, cls, why = C.plan_vs(ir)
            if plan is not None:
                form = "vs_pica_contract"
            elif why and why.startswith("too_many_vs_varyings"):
                form = "vs_too_many_varyings"
            elif why and why.startswith("control_flow"):
                form = "vs_control_flow"
            else:
                form = "vs_other"
            cls = None
        entry = forms[form]
        entry["hashes"] += 1
        entry["chunks"] += counts[sha]
        if len(entry["examples"]) < 6:
            entry["examples"].append(sha)
        census_class = class_by_sha.get(sha)
        if census_class:
            entry["refusal_classes"][census_class] += 1

    ordered = []
    for form in sorted(forms, key=lambda f: (-forms[f]["chunks"], f)):
        row = forms[form]
        ordered.append({
            "form": form,
            "hashes": row["hashes"],
            "chunks": row["chunks"],
            "refusal_classes": dict(row["refusal_classes"].most_common()),
            "examples": row["examples"],
        })
    doc = {
        "schema": "lighting-coverage/1",
        "pack": "lighting",
        "source": args.corpus,
        "chunks": sum(counts.values()),
        "unique_hashes": len(counts),
        "ps_hashes": sum(1 for r in first.values() if r["kind"] == "ps"),
        "vs_hashes": sum(1 for r in first.values() if r["kind"] == "vs"),
        "forms": ordered,
        "features": {
            key: {"hashes": feature_counts[key], "chunks": feature_chunks[key]}
            for key in FEATURES
        },
        "note": ("Form labels are pack-local derived metadata.  "
                 "The census class column is the phase-1 planner's first "
                 "refusal, not a shape."),
    }
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(doc, handle, indent=1)
        handle.write("\n")
    print("lighting: %d hashes / %d chunks" % (doc["unique_hashes"],
                                               doc["chunks"]))
    for row in ordered:
        print("%-22s %4d hashes %5d chunks" % (
            row["form"], row["hashes"], row["chunks"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
