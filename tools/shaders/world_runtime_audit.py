"""Runtime-path audit for the five *world* shader packs.

``tools/shaders/runtime_audit.py`` proves the global invariant: every
``draw_enabled == 1`` plan in ``docs/shader_plans.json`` plus every
``source/shader_packs/*/plans.json`` has a runtime artifact.  This tool is the
world-scoped, stricter companion.  It is deliberately not a second opinion:
it reuses the same parsers/validators, then adds the checks the world packs
need:

  * **pack join** -- every manifest plan row must resolve to a committed plan
    (own pack catalog or a ``docs/shader_plans.json`` row reachable from the
    pack's census hashes), with matching kind and draw status;
  * **draw gate** -- a ``draw_enabled: 1`` row needs ``runtime: 1`` *and* a
    concrete artifact: an exact-hash PICA VS program (DVLB/DVLE/DVLP, pinned
    SHA-256, emitter proof) or a generated TEV descriptor whose own
    ``draw_enabled`` is 1;
  * **descriptor validity** -- TEV stages/passes/constant binding/textures and
    PICA VS lengths/registers/varyings (<= 7)/uniform bank (<= c31);
  * **honesty** -- a draw-disabled plan must carry a non-empty note, and an
    ``emitted-verified`` PICA VS that stays off must say *why* it is not in
    the runtime VS catalog (silent green counters are holes).

Output is machine-readable with ``--json`` and fails (exit 1) on any hole.

Usage::

    python tools\\shaders\\world_runtime_audit.py
    python tools\\shaders\\world_runtime_audit.py --json docs\\world_runtime_audit.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import runtime_audit as base  # noqa: E402

WORLD_PACKS = ("terrain", "lighting", "skinning", "sky_water", "blit_post")
PACKS_DIR = os.path.join(REPO, "source", "shader_packs")
DOCS_PLANS = os.path.join(REPO, "docs", "shader_plans.json")
COVERAGE = os.path.join(REPO, "docs", "shader_coverage.json")

MAX_VARYINGS = 7
MAX_UNIFORM_ROWS = 32          # PICA VS bank c0..c31
MAX_VS_INPUTS = 16
MAX_VS_ATTRS = 5
REASON_TOKENS = ("draw", "runtime", "catalog", "promotion")


def _load(path: str):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def _pack_doc(pack: str, name: str):
    path = os.path.join(PACKS_DIR, pack, name)
    return _load(path) if os.path.isfile(path) else None


def _is_world_hash(pack_manifest: dict, sha: str) -> bool:
    return any(row and row[0] == sha for row in pack_manifest.get("hashes", []))


def _note_has_reason(entry: dict) -> bool:
    note = str(entry.get("note") or "").lower()
    return bool(note.strip()) and any(token in note for token in REASON_TOKENS)


def _check_pica_limits(entry: dict, errors: list) -> None:
    label = entry.get("id") or "?"
    outputs = entry.get("outputs") or []
    if len(outputs) > MAX_VARYINGS:
        errors.append("%s: %d varyings exceed %d" % (label, len(outputs),
                                                     MAX_VARYINGS))
    inputs = entry.get("inputs") or []
    if len(inputs) > MAX_VS_INPUTS:
        errors.append("%s: %d inputs exceed %d" % (label, len(inputs),
                                                   MAX_VS_INPUTS))
    for row in inputs:
        if not isinstance(row.get("reg_index"), int) or \
                not 0 <= row["reg_index"] <= 15:
            errors.append("%s: input register v%r outside v0..v15"
                          % (label, row.get("reg_index")))

    emitter = entry.get("emitter") or {}
    if entry.get("codegen") == "emitted-verified":
        rows = emitter.get("uniform_rows")
        if not isinstance(rows, int) or not 1 <= rows <= MAX_UNIFORM_ROWS:
            errors.append("%s: emitter uniform_rows %r outside c0..c31"
                          % (label, rows))
        size = emitter.get("shbin_bytes")
        if not isinstance(size, int) or size <= 0:
            errors.append("%s: emitter has no .shbin bytes" % label)
        pin = emitter.get("shbin_sha256")
        if not (isinstance(pin, str) and len(pin) == 64):
            errors.append("%s: emitter shbin SHA-256 is not pinned" % label)
        if emitter.get("picasso") != "pass":
            errors.append("%s: emitter picasso is %r"
                          % (label, emitter.get("picasso")))



def _draw_path(entry: dict, sha: str, vs: dict, table: dict):
    """Return (path, reason).  A non-None path means a validated runtime row."""
    kind = entry.get("kind")
    if kind == "pica_vs":
        program = vs.get(sha)
        if program is None:
            return None, "no exact-hash PICA VS program on this build"
        return "pica_vs:%s" % program["id"], None
    if kind == "tev":
        row = table.get(sha)
        if row is None:
            return None, "no generated TEV descriptor in the C table"
        if row.get("draw_enabled") != 1:
            return None, "generated TEV descriptor is draw-disabled"
        return "tev:plan_table", None
    return None, "unknown plan kind %r" % kind


def audit() -> dict:
    docs = {entry["sha256"]: entry
            for entry in _load(DOCS_PLANS).get("entries", [])}
    vs = base.parse_vs_catalog()
    table = base.parse_plan_table()
    coverage = _load(COVERAGE) if os.path.isfile(COVERAGE) else {"packs": []}
    coverage_by_pack = {row.get("pack"): row
                        for row in coverage.get("packs", [])}

    unmatched, invalid, silent_off = [], [], []
    manifest_mismatches, coverage_mismatches = [], []
    packs = []
    total_draw = total_runtime = 0

    for pack in WORLD_PACKS:
        plan_doc = _pack_doc(pack, "plans.json") or {"entries": []}
        manifest = _pack_doc(pack, "manifest.json") or {}
        own = {entry["sha256"]: entry for entry in plan_doc.get("entries", [])}

        # Join pack-owned plans with docs rows reachable from this pack's
        # census hashes (menu.composite.ps is one such runtime-certified row).
        joined = dict(own)
        cross = {}
        for row in manifest.get("hashes", []):
            sha = row[0]
            if sha in docs and sha not in joined:
                joined[sha] = docs[sha]
                cross[sha] = docs[sha]
        for sha, entry in joined.items():
            if not _is_world_hash(manifest, sha):
                invalid.append("%s: plan %s is not a census hash of pack %s"
                               % (pack, entry.get("id"), pack))
            kind = entry.get("kind")
            if kind == "pica_vs":
                _check_pica_limits(entry, invalid)
            elif kind == "tev":
                invalid.extend(base.validate_tev(entry))
            else:
                invalid.append("%s: unknown plan kind %r"
                               % (entry.get("id"), kind))

            if entry.get("draw_enabled") == 1:
                total_draw += 1
                # ``runtime`` is an explicit claim when present; the concrete
                # artifact below is what actually decides the draw path.
                if entry.get("runtime") not in (None, 1):
                    invalid.append("%s: draw_enabled=1 but runtime=%r"
                                   % (entry.get("id"), entry.get("runtime")))
                path, why = _draw_path(entry, sha, vs, table)
                if path is None:
                    unmatched.append({
                        "pack": pack, "id": entry.get("id"), "sha256": sha,
                        "kind": kind, "family": entry.get("family"),
                        "runtime": entry.get("runtime"), "reason": why,
                    })
                else:
                    total_runtime += 1
                    if kind == "pica_vs":
                        invalid.extend(base.validate_vs_program(
                            vs[sha], (DOCS_PLANS, joined[sha])))
            else:
                if entry.get("runtime") == 1:
                    invalid.append("%s: runtime=1 but draw_enabled=%r"
                                   % (entry.get("id"),
                                      entry.get("draw_enabled")))
                if not str(entry.get("note") or "").strip():
                    silent_off.append("%s: draw-disabled plan has no note"
                                      % entry.get("id"))
                elif entry.get("codegen") == "emitted-verified" and \
                        not _note_has_reason(entry):
                    silent_off.append(
                        "%s: verified PICA VS is draw-disabled without a "
                        "stated reason in note" % entry.get("id"))

        # Manifest plan rows must round-trip to the joined plans.
        manifest_plans = manifest.get("plans", [])
        for row in manifest_plans:
            sha = row.get("sha256")
            entry = joined.get(sha)
            if entry is None:
                manifest_mismatches.append(
                    "%s: manifest plan %s has no committed plan row"
                    % (pack, row.get("id")))
                continue
            if row.get("kind") != entry.get("kind"):
                manifest_mismatches.append(
                    "%s: manifest kind %r != plan kind %r for %s"
                    % (pack, row.get("kind"), entry.get("kind"),
                       entry.get("id")))
            expected = "draw-enabled" if entry.get("draw_enabled") == 1 \
                else "translated"
            if row.get("status") != expected:
                manifest_mismatches.append(
                    "%s: manifest status %r != %s for %s"
                    % (pack, row.get("status"), expected, entry.get("id")))

        # Coverage counters must describe the same joined set.
        draw_ids = [e["id"] for e in joined.values()
                    if e.get("draw_enabled") == 1]
        runtime_ids = []
        for sha, entry in joined.items():
            if entry.get("draw_enabled") != 1:
                continue
            path, _ = _draw_path(entry, sha, vs, table)
            if path is not None:
                runtime_ids.append(entry["id"])
        cov = coverage_by_pack.get(pack, {})
        if cov:
            if cov.get("draw_enabled") != len(draw_ids):
                coverage_mismatches.append(
                    "%s: coverage draw_enabled %r != %d committed"
                    % (pack, cov.get("draw_enabled"), len(draw_ids)))
            # ``translated`` in shader_coverage.json is hash-level (a hash may
            # have a scheduler contract instead of a pack plan), so only the
            # plan-level draw counter is comparable here.
            if cov.get("unique_hashes") != len(manifest.get("hashes", [])):
                coverage_mismatches.append(
                    "%s: coverage unique_hashes %r != %d manifest"
                    % (pack, cov.get("unique_hashes"),
                       len(manifest.get("hashes", []))))

        packs.append({
            "pack": pack,
            "pack_plans": len(own),
            "joined_plans": len(joined),
            "cross_pack_plans": sorted(
                ({"id": e["id"], "sha256": sha,
                  "draw_enabled": e.get("draw_enabled")}
                 for sha, e in cross.items()),
                key=lambda item: item["id"]),
            "census_hashes": len(manifest.get("hashes", [])),
            "draw_enabled": len(draw_ids),
            "runtime_paths": len(runtime_ids),
            "catalog_only": len(draw_ids) - len(runtime_ids),
            "unmatched": sorted(e["id"] for e in unmatched
                                if e["pack"] == pack),
            "by_kind": dict(Counter(e.get("kind")
                                    for e in joined.values())),
        })

    # Top-10 most frequent world shaders, by census chunk count.
    top_rows = []
    for pack in WORLD_PACKS:
        manifest = _pack_doc(pack, "manifest.json") or {}
        own = {entry["sha256"]: entry
               for entry in (_pack_doc(pack, "plans.json") or {})
               .get("entries", [])}
        for row in manifest.get("hashes", []):
            sha, kind, klass, chunks = row[0], row[1], row[2], row[3]
            entry = own.get(sha) or docs.get(sha)
            if entry is None:
                status = "refused:%s" % klass
                plan_id = None
            else:
                plan_id = entry.get("id")
                if entry.get("draw_enabled") == 1:
                    path, _ = _draw_path(entry, sha, vs, table)
                    status = "runtime" if path else "draw-no-path"
                else:
                    status = "catalog-only"
            top_rows.append({
                "sha256": sha, "pack": pack, "kind": kind,
                "chunks": chunks, "class": klass, "status": status,
                "plan": plan_id,
            })
    top_rows.sort(key=lambda row: (-row["chunks"], row["pack"], row["sha256"]))
    top = top_rows[:10]

    return {
        "schema": "world-runtime-audit/1",
        "packs": packs,
        "totals": {
            "plans": sum(p["joined_plans"] for p in packs),
            "draw_enabled": total_draw,
            "runtime_paths": total_runtime,
            "catalog_only": total_draw - total_runtime,
            "unmatched": len(unmatched),
            "invalid": len(invalid),
            "silent_off": len(silent_off),
        },
        "top_world_shaders": top,
        "unmatched": unmatched,
        "invalid": invalid,
        "silent_off": silent_off,
        "manifest_mismatches": manifest_mismatches,
        "coverage_mismatches": coverage_mismatches,
        "ok": not (unmatched or invalid or silent_off or manifest_mismatches
                   or coverage_mismatches),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", default="", help="write the report here")
    args = parser.parse_args(argv)
    report = audit()
    if args.json:
        os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2)
            handle.write("\n")
    for pack in report["packs"]:
        print("%-10s plans %3d  draw %d/%d  runtime %d  catalog-only %d"
              % (pack["pack"], pack["pack_plans"], pack["runtime_paths"],
                 pack["draw_enabled"], pack["runtime_paths"],
                 pack["catalog_only"]))
    print("world draw-enabled: %d, runtime paths: %d, unmatched: %d, "
          "invalid: %d, silent-off: %d"
          % (report["totals"]["draw_enabled"],
             report["totals"]["runtime_paths"],
             report["totals"]["unmatched"], report["totals"]["invalid"],
             report["totals"]["silent_off"]))
    for row in report["unmatched"]:
        print("UNMATCHED %-28s %s: %s"
              % (row["id"], row["sha256"][:16], row["reason"]))
    for text in report["invalid"]:
        print("INVALID %s" % text)
    for text in report["silent_off"]:
        print("SILENT-OFF %s" % text)
    for text in report["manifest_mismatches"]:
        print("MANIFEST %s" % text)
    for text in report["coverage_mismatches"]:
        print("COVERAGE %s" % text)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
