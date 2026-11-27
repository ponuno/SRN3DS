"""Per-family shader packs and the phase-1 coverage report.

Six packs own the census so six agents never edit the same files:

| pack | census families |
|---|---|
| ``ui`` | ``ui/interface`` |
| ``blit_post`` | ``blit/copy``, ``post-process/imagespace`` |
| ``sky_water`` | ``sky``, ``water`` |
| ``terrain`` | ``terrain/landscape``, ``grass/trees`` |
| ``skinning`` | ``skinning/characters`` |
| ``lighting`` | ``lighting/deferred/shadows``, ``particles/effects`` |

Each pack gets ``source/shader_packs/<pack>/manifest.json`` (counts, refusal
classes, one row per bytecode SHA-256) and a short ``README.md`` skeleton.
``source/shader_packs/index.json`` is the cross-pack index.  The generated C
table ``source/d3d9_shader_pack_table.inc`` is **append-only**: an existing
pack row is never rewritten, only new packs are appended.

The manifests are derived metadata (hashes and counts), not game bytes.  They
are committed so the tests can check the partition without the retail archive:

    chunks: 10,475   unique hashes: 6,659   packs: 6 (disjoint, exhaustive)

Usage::

    python tools\\shader_packs.py manifest --corpus E:\\host_sm3
    python tools\\shader_packs.py coverage --out docs\\shader_coverage.json
    python tools\\shader_packs.py check
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter, defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

PACKS_DIR = os.path.join(REPO, "source", "shader_packs")
INDEX_PATH = os.path.join(PACKS_DIR, "index.json")
TABLE_PATH = os.path.join(REPO, "source", "d3d9_shader_pack_table.inc")
PLANS_PATH = os.path.join(REPO, "docs", "shader_plans.json")
SCHEDULER_PATH = os.path.join(REPO, "docs", "scheduler_plans.json")
CENSUS_PATH = os.path.join(REPO, "docs", "SHADER_CENSUS.json")

DEFAULT_CORPUS = r"E:\host_sm3"
DEFAULT_CENSUS = r"E:\Skyrim_N3DS_build\assets\shaders\fxp_census.json"

PACKS = {
    "ui": {
        "owner": "sm3-agent-ui",
        "families": ["ui/interface"],
        "description": ("menu/UI pixel and vertex shaders: A8 atlas copies, "
                        "text tint, simple colour math"),
        "first_step": "re-plug the certified A8 text plan; keep draw blocked",
    },
    "blit_post": {
        "owner": "sm3-agent-blit-post",
        "families": ["blit/copy", "post-process/imagespace"],
        "description": ("copy/blit and image-space post effects (bloom, "
                        "adaptation, colour grading)"),
        "first_step": "certify one-stage copy plans, then full-screen passes",
    },
    "sky_water": {
        "owner": "sm3-agent-sky-water",
        "families": ["sky", "water"],
        "description": "sky dome/atmosphere and water surfaces",
        "first_step": "map water refraction/reflection samplers to TEV stages",
    },
    "terrain": {
        "owner": "sm3-agent-terrain",
        "families": ["terrain/landscape", "grass/trees"],
        "description": "landscape multitexture, LOD and world-map overlay",
        "first_step": "reduce 6-way multitexture to the 6-stage TEV budget",
    },
    "skinning": {
        "owner": "sm3-agent-skinning",
        "families": ["skinning/characters"],
        "description": "character/normal-map/tint-mask shaders and skinned VS",
        "first_step": "land the VS skinning contract before any PS work",
    },
    "lighting": {
        "owner": "sm3-agent-lighting",
        "families": ["lighting/deferred/shadows", "particles/effects"],
        "description": ("lit material shaders, deferred/shadow passes and "
                        "particle/effect shaders"),
        "first_step": "split the normal/basis dp4 prebake out of the PS plan",
    },
}

FAMILY_TO_PACK = {family: pack for pack, spec in PACKS.items()
                  for family in spec["families"]}


def _load_json(path: str):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def _plan_index(path: str = PLANS_PATH, include_scheduler: bool = False) -> dict:
    """Runtime plans plus every pack-owned catalog.

    A pack catalog (``source/shader_packs/<pack>/plans.json``) is where an
    agent's translations live; only entries marked ``"runtime": 1`` are also
    mirrored into the shared runtime table.  ``docs/scheduler_plans.json`` is
    an additional cross-pack catalog; it is included for coverage but kept out
    of the per-pack manifests so those stay owned by their generators.
    """
    documents = []
    if os.path.exists(path):
        documents.append((path, _load_json(path)))
    if os.path.isdir(PACKS_DIR):
        for name in sorted(os.listdir(PACKS_DIR)):
            extra = os.path.join(PACKS_DIR, name, "plans.json")
            if os.path.isfile(extra):
                documents.append((extra, _load_json(extra)))
    if include_scheduler and os.path.isfile(SCHEDULER_PATH):
        documents.append((SCHEDULER_PATH, _load_json(SCHEDULER_PATH)))
    out = {}
    for origin, doc in documents:
        for entry in doc.get("entries", []):
            status = ("draw-enabled" if entry.get("draw_enabled") == 1
                      else "translated")
            row = {"id": entry["id"], "status": status,
                   "kind": entry["kind"], "family": entry.get("family", "")}
            seen = out.get(entry["sha256"])
            if seen is None:
                out[entry["sha256"]] = row
            elif seen != row:
                raise SystemExit("%s: plan %r disagrees with the catalog for %s"
                                 % (origin, entry["id"], entry["sha256"][:16]))
    return out


def build(corpus_dir: str, census_path: str) -> dict:
    corpus = _load_json(os.path.join(corpus_dir, "corpus.json"))
    classes = {}
    if os.path.exists(census_path):
        for row in _load_json(census_path)["hashes"]:
            classes[row["sha256"]] = row.get("class", "")
    plans = _plan_index()

    packs = {name: {"chunks": 0, "kinds": Counter(), "hashes": {},
                    "classes": Counter()} for name in PACKS}
    for row in corpus["rows"]:
        pack = FAMILY_TO_PACK.get(row["family"])
        if pack is None:
            raise SystemExit("family %r has no pack" % row["family"])
        bucket = packs[pack]
        bucket["chunks"] += 1
        bucket["kinds"][row["kind"]] += 1
        sha = row["sha256"]
        entry = bucket["hashes"].setdefault(sha, {
            "sha256": sha, "kind": row["kind"],
            "class": classes.get(sha, ""), "chunks": 0})
        if entry["chunks"] == 0:
            bucket["classes"][entry["class"] or "unclassified"] += 1
        entry["chunks"] += 1
    return {"corpus": corpus, "packs": packs, "plans": plans}


def _coverage(hashes: dict, plans: dict) -> dict:
    counts = {"translated": 0, "draw-enabled": 0, "refused": 0,
              "contract_only": 0}
    for sha, row in hashes.items():
        plan = plans.get(sha)
        counts[plan["status"] if plan else "refused"] += 1
        if plan is None and row.get("class") == "pica_vs":
            counts["contract_only"] += 1
    return counts


def write_manifests(built: dict) -> dict:
    plans = built["plans"]
    index = {"schema": "shader-pack-index/1", "packs": {},
             "chunks": 0, "unique_hashes": 0}
    seen = set()
    for name, spec in PACKS.items():
        bucket = built["packs"][name]
        hashes = [[row["sha256"], row["kind"], row["class"], row["chunks"]]
                  for sha in sorted(bucket["hashes"])
                  for row in (bucket["hashes"][sha],)]
        for row in hashes:
            if row[0] in seen:
                raise SystemExit("hash %s is in two packs" % row[0])
            seen.add(row[0])
        coverage = _coverage(bucket["hashes"], plans)
        manifest = {
            "schema": "shader-pack/1",
            "pack": name,
            "owner": spec["owner"],
            "census_families": spec["families"],
            "description": spec["description"],
            "first_step": spec["first_step"],
            "chunks": bucket["chunks"],
            "unique_hashes": len(hashes),
            "chunk_kinds": dict(sorted(bucket["kinds"].items())),
            "coverage": coverage,
            "refusal_classes": dict(sorted(bucket["classes"].items())),
            "hash_columns": ["sha256", "kind", "class", "chunks"],
            "plans": [dict(plan, sha256=sha) for sha, plan in sorted(plans.items())
                      if sha in bucket["hashes"]],
            "hashes": hashes,
        }
        pack_dir = os.path.join(PACKS_DIR, name)
        os.makedirs(pack_dir, exist_ok=True)
        with open(os.path.join(pack_dir, "manifest.json"), "w",
                  encoding="utf-8", newline="\r\n") as handle:
            json.dump(manifest, handle, indent=1)
            handle.write("\n")
        _write_readme(pack_dir, manifest)
        index["packs"][name] = {
            "owner": spec["owner"],
            "chunks": bucket["chunks"],
            "unique_hashes": len(hashes),
            "coverage": coverage,
        }
        index["chunks"] += bucket["chunks"]
        index["unique_hashes"] += len(hashes)
    os.makedirs(PACKS_DIR, exist_ok=True)
    with open(INDEX_PATH, "w", encoding="utf-8", newline="\r\n") as handle:
        json.dump(index, handle, indent=1)
        handle.write("\n")
    return index


README = """# Shader pack `{pack}`

Owner: `{owner}`

{description}

* census families: {families}
* chunks: **{chunks}** / unique hashes: **{unique}**
* coverage: {translated} translated, {draw} draw-enabled, {refused} refused
* first step: {first_step}

`manifest.json` lists every bytecode SHA-256 in this pack together with its
kind and census class.  Do not edit hashes by hand: regenerate with
`python tools\\shader_packs.py manifest`.

Only this directory is yours.  Shared files (`docs/shader_plans.json`,
`source/d3d9_shader_plan_table.inc`, `docs/shader_coverage.json`) are generated
and append-only; add entries, never rewrite another pack's rows.
"""


def _write_readme(pack_dir: str, manifest: dict) -> None:
    coverage = manifest["coverage"]
    with open(os.path.join(pack_dir, "README.md"), "w", encoding="utf-8",
                  newline="\r\n") as handle:
        handle.write(README.format(
            pack=manifest["pack"], owner=manifest["owner"],
            description=manifest["description"],
            families=", ".join("`%s`" % f for f in manifest["census_families"]),
            chunks=manifest["chunks"], unique=manifest["unique_hashes"],
            translated=coverage["translated"],
            draw=coverage["draw-enabled"], refused=coverage["refused"],
            first_step=manifest["first_step"]))


def write_table(index: dict) -> None:
    """Append-only C table: existing rows are preserved verbatim."""
    header = ("/* Generated by tools/shader_packs.py. Append-only: never edit a\n"
              " * committed row; new packs are appended. No bytecode is stored. */\n"
              "static const D9ShaderPackRow d9_shader_pack_rows[] = {\n")
    existing = []
    if os.path.exists(TABLE_PATH):
        with open(TABLE_PATH, encoding="utf-8") as handle:
            for line in handle:
                if '{"' in line:
                    existing.append(line.rstrip("\n"))
    known = {}
    for line in existing:
        for name in PACKS:
            if line.startswith('    {"%s"' % name):
                known[name] = line
    rows = []
    for name in PACKS:
        if name in known:
            rows.append(known[name])
            continue
        data = index["packs"][name]
        coverage = data["coverage"]
        rows.append('    {"%s", "%s", %du, %du, %du, %du, %du},'
                    % (name, PACKS[name]["owner"], data["chunks"],
                       data["unique_hashes"], coverage["translated"],
                       coverage["draw-enabled"], coverage["refused"]))
    with open(TABLE_PATH, "w", encoding="utf-8") as handle:
        handle.write(header)
        handle.write("\n".join(rows))
        handle.write("\n};\n#define D9_SHADER_PACK_ROW_COUNT "
                     "(sizeof d9_shader_pack_rows / sizeof d9_shader_pack_rows[0])\n")


def check() -> list:
    errors = []
    if not os.path.exists(INDEX_PATH):
        return ["%s is missing" % INDEX_PATH]
    index = _load_json(INDEX_PATH)
    seen = set()
    total_chunks = 0
    total_hashes = 0
    census = _load_json(CENSUS_PATH)
    plans = _plan_index()
    for name in PACKS:
        path = os.path.join(PACKS_DIR, name, "manifest.json")
        if not os.path.exists(path):
            errors.append("%s: manifest missing" % name)
            continue
        manifest = _load_json(path)
        if manifest.get("schema") != "shader-pack/1":
            errors.append("%s: bad schema tag" % name)
        rows = manifest.get("hashes", [])
        hashes = [row[0] for row in rows]
        if len(set(hashes)) != len(hashes):
            errors.append("%s: duplicate hash" % name)
        if any(row[0] in seen for row in rows):
            errors.append("%s: hash shared with another pack" % name)
        seen.update(hashes)
        if manifest["unique_hashes"] != len(hashes):
            errors.append("%s: unique_hashes disagrees with the hash list" % name)
        if manifest["chunks"] != sum(row[3] for row in rows):
            errors.append("%s: chunks disagrees with per-hash counts" % name)
        if manifest["chunks"] != sum(manifest["chunk_kinds"].values()):
            errors.append("%s: chunk_kinds total disagrees with chunks" % name)
        derived = _coverage({row[0]: {"class": row[2]} for row in rows}, plans)
        if derived != manifest["coverage"]:
            errors.append("%s: coverage %r disagrees with plans/refusals %r"
                          % (name, manifest["coverage"], derived))
        for plan in manifest["plans"]:
            if plans.get(plan["sha256"], {}).get("status") != plan["status"]:
                errors.append("%s: plan %s status disagrees with the manifest"
                              % (name, plan["id"]))
        total_chunks += manifest["chunks"]
        total_hashes += manifest["unique_hashes"]
    if total_chunks != census["chunks"]:
        errors.append("packs cover %d chunks, census says %d"
                      % (total_chunks, census["chunks"]))
    if total_hashes != census["distinct_hashes"]:
        errors.append("packs cover %d hashes, census says %d"
                      % (total_hashes, census["distinct_hashes"]))
    if index.get("chunks") != total_chunks:
        errors.append("index chunks disagrees with the packs")
    if os.path.isfile(SCHEDULER_PATH):
        try:
            from shaders import plan_verify
            scheduler = _load_json(SCHEDULER_PATH)
            seen_sha = set()
            for entry in scheduler.get("entries", []):
                plan_verify.validate_entry(entry, errors,
                                           "scheduler %s" % entry.get("id"))
                if entry.get("sha256") in seen_sha:
                    errors.append("scheduler: duplicate hash %s"
                                  % entry.get("sha256"))
                seen_sha.add(entry.get("sha256"))
                if entry.get("sha256") not in seen:
                    errors.append("scheduler: hash %s is not in the census"
                                  % entry.get("sha256"))
        except Exception as exc:  # pragma: no cover - defensive import guard
            errors.append("scheduler catalog unreadable: %s" % exc)
    return errors


def _extra_plans(index: dict) -> dict:
    """Plans whose hashes are not in the census.

    GFx's built-in menu programs live in `TESV.exe` tables, not in the shader
    BSA, so the census partition cannot see them.  They still consume runtime
    slots and can be draw-enabled; report them separately instead of letting
    the census total claim there is one draw plan when there are four.
    """
    census_hashes = set()
    for name in index["packs"]:
        manifest = _load_json(os.path.join(PACKS_DIR, name, "manifest.json"))
        census_hashes.update(row[0] for row in manifest["hashes"])
    by_pack = {}
    for name in PACKS:
        path = os.path.join(PACKS_DIR, name, "plans.json")
        if not os.path.isfile(path):
            continue
        entries = [entry for entry in _load_json(path).get("entries", [])
                   if entry.get("sha256") not in census_hashes]
        if entries:
            by_pack[name] = {
                "plans": len(entries),
                "draw_enabled": sum(1 for entry in entries
                                    if entry.get("draw_enabled") == 1),
                "exact": sum(1 for entry in entries
                             if entry.get("quality") == "exact"),
                "approx": sum(1 for entry in entries
                              if entry.get("quality") == "approx"),
            }
    return {
        "plans": sum(row["plans"] for row in by_pack.values()),
        "draw_enabled": sum(row["draw_enabled"] for row in by_pack.values()),
        "by_pack": by_pack,
    }


def coverage_report(built_or_none=None) -> dict:
    index = _load_json(INDEX_PATH)
    merged_plans = _plan_index(include_scheduler=True)
    census = _load_json(CENSUS_PATH)
    rows = []
    total = Counter()
    for name, data in index["packs"].items():
        manifest = _load_json(os.path.join(PACKS_DIR, name, "manifest.json"))
        queues = Counter()
        derived = Counter()
        for row in manifest["hashes"]:
            plan = merged_plans.get(row[0])
            if plan is None:
                queues[row[2] or "unclassified"] += 1
                derived["refused"] += 1
                if row[2] == "pica_vs":
                    derived["contract_only"] += 1
            else:
                derived[plan["status"]] += 1
        row = {
            "pack": name,
            "owner": data["owner"],
            "chunks": data["chunks"],
            "unique_hashes": data["unique_hashes"],
            "translated": derived["translated"],
            "draw_enabled": derived["draw-enabled"],
            "refused": derived["refused"],
            "contract_only": derived["contract_only"],
            "refused_classes": dict(sorted(queues.items(), key=lambda kv: -kv[1])),
            "needs": _needs({"refusal_classes": dict(queues)}),
        }
        rows.append(row)
        total.update({k: derived[k] for k in
                      ("translated", "draw-enabled", "refused")})
        total["chunks"] += data["chunks"]
        total["unique_hashes"] += data["unique_hashes"]
    return {
        "schema": "shader-coverage/1",
        "census_chunks": census["chunks"],
        "census_hashes": census["distinct_hashes"],
        "packs": rows,
        "extra_plans": _extra_plans(index),
        "total": dict(total),
        "note": ("translated = offline plan/contract exists; draw-enabled = "
                 "certified runtime plan; refused = named software refusal; "
                 "contract_only = a pica_vs envelope hash with no committed "
                 "descriptor. Scheduler catalog rows count as translated."),
    }


def plans_has(sha: str, doc: dict) -> bool:
    return any(entry["sha256"] == sha for entry in doc.get("entries", []))


def _needs(manifest: dict) -> list:
    out = []
    for cls, count in sorted(manifest["refusal_classes"].items(),
                             key=lambda kv: -kv[1]):
        if cls in ("", "pica_vs"):
            continue
        if cls.startswith("def_literal_out_of_range"):
            out.append("literal range rewrite (def outside RGBA8 [0,1])")
        elif cls == "unsupported_opcode":
            out.append("opcode lowering (cmp/if/loop/rcp/derivatives)")
        elif cls.startswith("dataflow") or cls.startswith("temp_not"):
            out.append("multi-temp dataflow -> scheduler or software fallback")
        elif cls.startswith("non_uniform_swizzle"):
            out.append("per-channel TEV selectors")
        elif cls.startswith("too_many_vs_varyings"):
            out.append("varying packing or multi-pass VS")
        elif cls.startswith("multiple_output_writes"):
            out.append("channel-carry plan")
        elif cls.startswith("input_not_a_colour"):
            out.append("varying semantic mapping")
    return out


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p_manifest = sub.add_parser("manifest")
    p_manifest.add_argument("--corpus", default=DEFAULT_CORPUS)
    p_manifest.add_argument("--census", default=DEFAULT_CENSUS)
    p_cov = sub.add_parser("coverage")
    p_cov.add_argument("--out", default=os.path.join(
        REPO, "docs", "shader_coverage.json"))
    sub.add_parser("check")
    args = parser.parse_args(argv)

    if args.command == "manifest":
        built = build(args.corpus, args.census)
        index = write_manifests(built)
        write_table(index)
        print(json.dumps(index, indent=1))
        return 0
    if args.command == "coverage":
        report = coverage_report()
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\r\n") as handle:
            json.dump(report, handle, indent=1)
            handle.write("\n")
        print(json.dumps(report["total"], indent=1))
        return 0
    errors = check()
    for error in errors:
        print("FAIL", error)
    print("packs: %d, chunks %d, hashes %d, %s"
          % (len(PACKS), _load_json(INDEX_PATH)["chunks"],
             _load_json(INDEX_PATH)["unique_hashes"],
             "OK" if not errors else "%d errors" % len(errors)))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
