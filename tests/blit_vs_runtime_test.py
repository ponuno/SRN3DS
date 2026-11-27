"""Runtime-path gate for the ``blit_post`` fullscreen-quad PICA VS family.

The six promoted programs are the exact ``vs_3_0`` hashes
``d9_pica_vs_blit_table.inc`` carries.  This test is deliberately narrower than
``world_shader_audit_test``: it checks the committed *bytes* (DVLB, DVLE,
DVLP, pinned SHA-256 and main word count) and, when the retail archive is
present, re-runs the emitter's numeric differential against the original IR.

A missing archive is an explicit SKIP for the numeric layer only; the static
byte checks always run.
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

PACK = os.path.join(REPO, "source", "shader_packs", "blit_post")
PLANS = os.path.join(PACK, "plans.json")
PROMOTED = {
    "blit.vs.31d368ce": "31d368cecad5b4d215c4c1fe1580d9ab0606e09af4ee3d2fa4ce8c4fcae9359f",
    "blit.vs.57e51db9": "57e51db9dfcc5ad7014b12d9d9b819a0ec1a98c064ed657b0a8f829631396dfb",
    "blit.vs.c7ef0cd4": "c7ef0cd44146251737cf27b21bf2909f74c54d7fbf297ca681d53dc02458485f",
    "blit.vs.ce758c79": "ce758c790d9684dc42916fbdb343d7661a600210ff5c299697742f5b373b8e33",
    "blit.vs.d48c8642": "d48c8642ffa0bc04ec33de070c52b2dfa3f36db3ec6b050e0881627e0f306920",
    "blit.vs.d6896494": "d68964943e9560d499b369bd47969ca7e621cb09da8470fc56739b412974132c",
}
OFFLINE = {"blit.vs.4d155cd6", "blit.vs.c43418bb"}
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

    doc = json.load(open(PLANS, encoding="utf-8"))
    entries = {e["id"]: e for e in doc["entries"] if e.get("kind") == "pica_vs"}
    catalog = runtime_audit.parse_vs_catalog()

    for plan_id, sha in PROMOTED.items():
        entry = entries.get(plan_id)
        check(entry is not None, "%s is missing from the pack catalog" % plan_id)
        if not entry:
            continue
        check(entry.get("draw_enabled") == 1 and entry.get("runtime") == 1,
              "%s is not draw-enabled/runtime" % plan_id)
        check(entry.get("sha256") == sha, "%s hash drifted" % plan_id)
        emitter = entry.get("emitter") or {}
        check(emitter.get("picasso") == "pass", "%s has no picasso proof" % plan_id)
        check(emitter.get("samples", 0) >= 256,
              "%s has no 256-sample numeric proof" % plan_id)
        check(emitter.get("max_abs_position", 1.0) <= 1e-4,
              "%s position differential exceeds 1e-4" % plan_id)
        check(emitter.get("max_abs_varying", 1.0) <= 1e-4,
              "%s varying differential exceeds 1e-4" % plan_id)

        program = catalog.get(sha)
        check(program is not None, "%s has no blit table row" % plan_id)
        if not program:
            continue
        data = program["shbin"]
        check(data[:4] == b"DVLB", "%s .shbin is not DVLB" % plan_id)
        check(int.from_bytes(data[4:8], "little") == 1,
              "%s .shbin does not have exactly one DVLE" % plan_id)
        check(data[0x0C:0x10] == b"DVLP", "%s .shbin has no DVLP" % plan_id)
        words = int.from_bytes(data[0x18:0x1C], "little")
        check(words == program["instructions"],
              "%s main word count %d != table %d"
              % (plan_id, words, program["instructions"]))
        check(len(data) == program["shbin_size"],
              "%s declared shbin length drifted" % plan_id)
        check(len(data) == emitter.get("shbin_bytes"),
              "%s emitter/table length mismatch" % plan_id)
        check(runtime_audit.validate_vs_program(program, (PLANS, entry)) == [],
              "%s fails runtime_audit.validate_vs_program" % plan_id)

    for plan_id in OFFLINE:
        entry = entries.get(plan_id)
        check(entry is not None, "%s is missing from the pack catalog" % plan_id)
        if not entry:
            continue
        check(entry.get("draw_enabled") == 0 and entry.get("runtime") == 0,
              "%s claims a runtime path it does not have" % plan_id)
        check("draw disabled:" in str(entry.get("note") or ""),
              "%s is offline without a reason in its note" % plan_id)

    # Numeric layer: re-run the real IR -> PICA differential when the retail
    # archive is mounted.  Absent archive is a named skip, never a pass.
    data_dir = next((path for path in DATA_CANDIDATES
                     if os.path.exists(os.path.join(path, ARCHIVE))), "")
    if data_dir:
        import pica_vs_emit as S
        import vs_contract_emit as V
        hashes = set(PROMOTED.values())
        irs = V.load_irs(hashes, data_dir)
        for plan_id, sha in sorted(PROMOTED.items()):
            entry = entries[plan_id]
            ir = irs[sha][0]
            plan = {
                "id": plan_id, "sha256": sha, "kind": "pica_vs",
                "inputs": entry["inputs"], "outputs": entry["outputs"],
                "instructions": entry.get("instructions", 0),
                "uniform_registers": entry.get("uniform_registers", 0),
                "dropped_output_regs": entry.get("dropped_output_regs", []),
            }
            proof, shbin = S.verify(plan, ir, samples=64, bones=0,
                                    identity_consts=True, rel_tolerance=1e-6,
                                    palette_rows=0, magnitude_limit=1e6)
            check(proof["status"] == "pass",
                  "%s re-verification failed (%s)"
                  % (plan_id, proof.get("picasso")))
            check(shbin == catalog[sha]["shbin"],
                  "%s re-emitted .shbin differs from the committed bytes"
                  % plan_id)
        print("blit_vs_runtime_test: numeric differential re-run against %s"
              % data_dir)
    else:
        print("blit_vs_runtime_test: SKIP numeric differential (retail archive "
              "not mounted)")

    print("blit_vs_runtime_test: %d checks, %d failures (%d promoted, %d "
          "offline)"
          % (checks, len(failures), len(PROMOTED), len(OFFLINE)))
    for text in failures[:40]:
        print("FAIL: %s" % text)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
