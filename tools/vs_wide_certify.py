#!/usr/bin/env python3
"""Certify the emitted-verified world VS that read TEXCOORD1..4.

The loader now matches a declaration's ``TEXCOORDn`` (``D9PicaVsAttr.
usage_index``), but a loader-capable program is not automatically a draw path:
the paired PS must sample the same channels and must itself be runtime-enabled.
This tool re-derives, per program:

* the emitted envelope (D3D varying set, rank map, components);
* the 256-sample IR-vs-PICA differential;
* every PS chunk in the retail shader package that shares the VS technique
  key, its sampled TEXCOORD/COLOR set and committed plan state;
* a verdict: ``ok`` only when a paired PS exists, is draw-enabled, samples a
  subset of the emitted channels, and reads no colour1 (PICA TEV has one
  primary colour);

Promotion is deliberately not done here.  A non-``ok`` verdict keeps
``draw_enabled: 0`` and is the exact reason recorded in
``source/shader_packs/wide_vs_certification.json``.

Run::

    python tools\\vs_wide_certify.py --data E:\\SteamLibrary\\steamapps\\common\\Skyrim\\Data
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
for _path in (REPO, os.path.join(REPO, "tools"),
              os.path.join(REPO, "tools", "shaders"),
              os.path.join(REPO, "source"), os.path.join(REPO, "source",
              "shader_packs", "skinning")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

from shaders import compile as C  # noqa: E402
import pica_vs_emit as S  # noqa: E402
import vs_contract_emit as V  # noqa: E402

TARGETS = (
    ("lighting", ("lit.vs.2b6fc858", "lit.vs.475db3a3", "lit.vs.5102e34f",
                  "lit.vs.5c915b8e", "lit.vs.7bf75173", "lit.vs.810e8155",
                  "lit.vs.91edbfd1", "lit.vs.b376c937", "lit.vs.d1c92b1a",
                  "lit.vs.d526784f", "lit.vs.ef6207ba")),
    ("skinning", ("skin.vs.7800effd", "skin.vs.80c76b45",
                  "skin.vs.8be5cabf", "skin.vs.e54c8f75")),
)
OUT = os.path.join(REPO, "source", "shader_packs",
                   "wide_vs_certification.json")


def _plan_index() -> dict:
    index = {}
    for pack in ("lighting", "skinning", "sky_water", "ui", "terrain",
                 "blit_post"):
        path = os.path.join(REPO, "source", "shader_packs", pack,
                            "plans.json")
        if not os.path.isfile(path):
            continue
        for entry in json.load(open(path, encoding="utf-8"))["entries"]:
            index.setdefault(entry["sha256"], entry)
    docs = os.path.join(REPO, "docs", "shader_plans.json")
    for entry in json.load(open(docs, encoding="utf-8"))["entries"]:
        index.setdefault(entry["sha256"], entry)
    return index


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default="")
    args = parser.parse_args(argv)
    from fxp_compile import _load_packages

    packages = _load_packages(V.find_data(args.data))
    vs_meta, ps_by_meta = {}, collections.defaultdict(dict)
    metas = {}
    for package in packages:
        for chunk in package.chunks:
            ir = C.build_ir(chunk.code)
            if ir.error:
                continue
            if chunk.kind == "vs":
                vs_meta[ir.sha256] = chunk.meta
            elif chunk.kind == "ps":
                ps_by_meta[chunk.meta][ir.sha256] = ir

    plans = _plan_index()
    out = []
    for pack, ids in TARGETS:
        doc = json.load(open(os.path.join(
            REPO, "source", "shader_packs", pack, "plans.json"),
            encoding="utf-8"))
        by_id = {e["id"]: e for e in doc["entries"]}
        for plan_id in ids:
            entry = by_id[plan_id]
            ir = V.load_irs({entry["sha256"]}, args.data)[entry["sha256"]][0]
            meta = vs_meta.get(entry["sha256"])
            outs = {(o["usage"], o["usage_index"]) for o in entry["outputs"]}
            tex = sorted(i for u, i in outs if u == 5)
            clr = sorted(i for u, i in outs if u == 10)
            # numeric differential with the committed lowering
            if entry.get("packing"):
                lp = dict(entry, id=plan_id)
                proof, _blob = S.verify(lp, ir, samples=256, bones=4,
                                        rel_tolerance=1e-6,
                                        magnitude_limit=1e6)
            else:
                base, _c, _w = C.plan_vs(ir)
                low = V.lower_varyings(base)
                body = [ins for ins in ir.body
                        if ins.name not in ("def", "nop")]
                relative = sorted({s.index for ins in body for s in ins.srcs
                                   if s.reg_type == 2 and s.relative})
                palette_rows = (max(relative) + 1) if relative else 0
                identity = palette_rows == 0
                lp = dict(base, id=plan_id, outputs=low["outputs"],
                          dropped_output_regs=[[o["type"], o["reg_index"]]
                                               for o in low["dropped"]])
                proof, _blob = S.verify(lp, ir, samples=256, bones=0,
                                        identity_consts=identity,
                                        rel_tolerance=1e-6,
                                        palette_rows=palette_rows,
                                        magnitude_limit=1e6)
            partners = []
            results = []
            loadable = all(i.get("usage") in (0, 1, 2, 5, 10)
                           for i in entry.get("inputs", []))
            for psha, pir in ps_by_meta.get(meta, {}).items():
                ins = {(x.usage, x.usage_index)
                       for x in pir.program.declarations if x.reg_type == 1}
                pt = sorted(i for u, i in ins if u == 5)
                pc = sorted(i for u, i in ins if u == 10)
                plan = plans.get(psha)
                textures = (plan or {}).get("textures") or []
                tcr = [t.get("texcoord_reg") for t in textures]
                kind, why = "ok", ""
                if plan is None or plan.get("draw_enabled") != 1:
                    kind, why = "ps_not_runtime", (
                        "paired PS %s has no draw-enabled runtime plan"
                        % psha[:8])
                elif 1 in pc:
                    kind, why = "ps_reads_color1", (
                        "paired PS %s samples COLOR1; PICA TEV has one "
                        "primary colour and the VS packs/drops it" % psha[:8])
                elif [i for i in pt if i not in tex]:
                    kind, why = "ps_texcoord_not_produced", (
                        "paired PS %s samples texcoord%s, which the VS does "
                        "not write" % (psha[:8], ",".join(
                            str(i) for i in pt if i not in tex)))
                elif len(tcr) > len(pt) or any(
                        tcr[i] != sorted(pt).index(pt[i])
                        for i in range(len(tcr))):
                    kind, why = "rank_mismatch", (
                        "paired PS %s plan textures %s do not follow the "
                        "ascending rank of its sampled texcoords %s"
                        % (psha[:8], tcr, pt))
                partners.append({
                    "sha256": psha, "plan": plan and plan.get("id"),
                    "draw_enabled": plan and plan.get("draw_enabled"),
                    "kind": plan and plan.get("kind"),
                    "texcoords": pt, "colors": pc,
                    "texcoord_reg": tcr, "verdict": kind,
                })
                results.append((kind, why))
            if not loadable:
                verdict, reason = "vs_input_not_loadable", (
                    "the program reads an input attribute the PICA runtime "
                    "enum does not carry (normal/tangent/binormal)")
            elif any(kind == "ok" for kind, _ in results):
                verdict, reason = "ok", next(
                    why for kind, why in results if kind == "ok")
            else:
                order = ["ps_reads_color1", "rank_mismatch",
                         "ps_texcoord_not_produced", "ps_not_runtime"]
                priority = {name: i for i, name in enumerate(order)}
                results.sort(key=lambda item: priority.get(item[0], 99))
                if results:
                    verdict, reason = results[0]
                else:
                    verdict, reason = "no_ps_partner", (
                        "no PS chunk shares the technique key %r" % meta)
            out.append({
                "pack": pack, "id": plan_id, "sha256": entry["sha256"],
                "technique_key": meta,
                "envelope": {"texcoords": tex, "colors": clr,
                             "varying_map": entry.get("varying_map")},
                "numeric": {
                    "samples": proof["samples"],
                    "max_abs_position": proof["max_abs_position"],
                    "max_abs_varying": proof["max_abs_varying"],
                    "picasso": proof["picasso"],
                    "status": proof["status"],
                },
                "ps_partners": partners,
                "verdict": verdict, "reason": reason,
                "draw_enabled": entry.get("draw_enabled"),
            })
    report = {
        "schema": "wide-vs-certification/1",
        "samples": 256,
        "programs": len(out),
        "ok": sum(1 for row in out if row["verdict"] == "ok"),
        "entries": out,
    }
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, indent=1, sort_keys=True)
        handle.write("\n")
    for row in out:
        print("%-22s %-28s %s" % (row["id"], row["verdict"], row["reason"]))
    print("certified %d programs, %d promotable" % (len(out), report["ok"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
