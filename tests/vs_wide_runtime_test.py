"""Runtime gate for the two wide-varying world PICA VS programs.

``lit.vs.b3a9babe`` and ``skin.vs.8513aaac`` write D3D TEXCOORD2/TEXCOORD3.
They are exact only under the paired-PS rank contract: D3D indices are packed
in ascending order into PICA texcoord slots.  This test checks the committed
bytes and, statically, that the retail PS family that samples {t2,t3} already
carries the same rank (``ui.ps.a5712176`` -> ``texcoord_reg = 1``).
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
for _path in (os.path.join(REPO, "tools"),
              os.path.join(REPO, "tools", "shaders"),
              os.path.join(REPO, "source", "shader_packs", "skinning")):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import runtime_audit  # noqa: E402

TARGETS = {
    "lit.vs.b3a9babe":
        ("lighting", "b3a9babe7949ac6f984060bae6edb5eb0898e6bc40474e60d1555d5d420852d6"),
    "skin.vs.8513aaac":
        ("skinning", "8513aaac88efadcd52639d9b1b87692f1242e6d21d7f4383f7133762e104d3a6"),
}
PAIRED_PS = "a571217696aef0d29e2f704b46e0246e4712d735a3c5d1cbd6ed3d2acef33538"
ARCHIVE = "Skyrim - Shaders.bsa"
DATA_CANDIDATES = [r"F:\Skyrim\Data",
                   r"E:\SteamLibrary\steamapps\common\Skyrim\Data"]


def main() -> int:
    checks = 0
    failures = []

    def check(condition, text):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(text)

    catalog = runtime_audit.parse_vs_catalog()
    signed = {}
    for plan_id, (pack, sha) in TARGETS.items():
        path = os.path.join(REPO, "source", "shader_packs", pack, "plans.json")
        doc = json.load(open(path, encoding="utf-8"))
        entry = next((e for e in doc["entries"] if e["id"] == plan_id), None)
        check(entry is not None, "%s is missing" % plan_id)
        if not entry:
            continue
        signed[plan_id] = entry
        check(entry.get("draw_enabled") == 1 and entry.get("runtime") == 1,
              "%s is not draw-enabled/runtime" % plan_id)
        check(entry.get("quality") == "exact", "%s is not exact" % plan_id)
        check(any("paired_ps_texcoord_rank_map" in str(p)
                  for p in entry.get("preconditions", [])),
              "%s does not name the paired-PS contract" % plan_id)
        emitter = entry.get("emitter") or {}
        check(emitter.get("samples", 0) >= 256,
              "%s has no 256-sample proof" % plan_id)
        check(emitter.get("max_abs_position", 1.0) <= 1e-4,
              "%s position differential" % plan_id)
        check(emitter.get("max_abs_varying", 1.0) <= 1e-4,
              "%s varying differential" % plan_id)
        program = catalog.get(sha)
        check(program is not None, "%s has no wide table row" % plan_id)
        if not program:
            continue
        data = program["shbin"]
        check(data[:4] == b"DVLB" and data[0x0C:0x10] == b"DVLP",
              "%s is not a DVLB/DVLP image" % plan_id)
        check(len(data) == emitter.get("shbin_bytes"),
              "%s emitter/table length mismatch" % plan_id)
        check(runtime_audit.validate_vs_program(program, (path, entry)) == [],
              "%s fails validate_vs_program" % plan_id)
        # Rank map: D3D TEXCOORD index N -> PICA slot rank(N) in ascending
        # order.  The plans must agree with that rule.
        varying = sorted((o["d3d_usage_index"], o["pica_name"])
                         for o in entry["varying_map"] if o["d3d_usage"] == 5)
        for rank, (index, name) in enumerate(varying):
            check(name == "outtc%d" % rank,
                  "%s rank map drifted: texcoord%d -> %r"
                  % (plan_id, index, name))

    # Paired PS: the retail PS that samples {t2,t3} already uses slot 1 for
    # t3, i.e. the same ascending rank map.
    docs = json.load(open(os.path.join(REPO, "docs", "shader_plans.json"),
                          encoding="utf-8"))
    ps = next((e for e in docs["entries"] if e["sha256"] == PAIRED_PS), None)
    check(ps is not None, "paired PS %s has no committed plan" % PAIRED_PS[:8])
    if ps:
        textures = ps.get("textures") or []
        check(textures and textures[0].get("texcoord_reg") == 1,
              "paired PS does not sample the ranked texcoord1 slot")

    if "skin.vs.8513aaac" in signed:
        check(signed["skin.vs.8513aaac"]["emitter"]["bones"] == 4,
              "skinned wide VS lost its bone window")

    data_dir = next((p for p in DATA_CANDIDATES
                     if os.path.exists(os.path.join(p, ARCHIVE))), "")
    if data_dir:
        import pica_vs_emit as S
        import vs_contract_emit as V
        from shaders import asm
        irs = V.load_irs({sha for _p, sha in TARGETS.values()}, data_dir)
        for plan_id, (pack, sha) in TARGETS.items():
            entry = signed.get(plan_id)
            if not entry:
                continue
            ir = irs[sha][0]
            plan, _c, _w = V.C.plan_vs(ir)
            low = V.lower_varyings(plan)
            bones = entry["emitter"]["bones"]
            plan = dict(plan, id=plan_id, outputs=low["outputs"],
                        dropped_output_regs=[[o["type"], o["reg_index"]]
                                             for o in low["dropped"]])
            proof, shbin = S.verify(plan, ir, samples=64, bones=bones,
                                    identity_consts=bones == 0,
                                    rel_tolerance=1e-6, magnitude_limit=1e6)
            check(proof["status"] == "pass", "%s re-verify failed" % plan_id)
            check(shbin == catalog[sha]["shbin"],
                  "%s re-emitted .shbin differs" % plan_id)
        print("vs_wide_runtime_test: numeric differential against %s"
              % data_dir)
    else:
        print("vs_wide_runtime_test: SKIP numeric differential")

    print("vs_wide_runtime_test: %d checks, %d failures (%d wide programs)"
          % (checks, len(failures), len(TARGETS)))
    for text in failures[:40]:
        print("FAIL: %s" % text)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
