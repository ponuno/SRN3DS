#!/usr/bin/env python3
"""Measure specialised-pipeline coverage from real Skyrim chunk metadata.

The retail archive is read-only. Game bytecode is written only to a temporary
local dump for tests/pipeline_real.c and is never committed. The resulting
coverage JSON stores derived counts, package/meta/offset/trailer metadata and
instruction-stream identifiers for deduplication -- hashes are not acceptance
keys.

    .venv\\Scripts\\python.exe tools\\pipelines\\inventory.py measure ^
        --exe <pipeline_real.exe> --output tools\\pipelines\\coverage_base.json
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.shaders import packages  # noqa: E402

DEFAULT_DATA = r"E:\SteamLibrary\steamapps\common\Skyrim\Data"
REGISTRY_JSON = Path(__file__).with_name("registry.json")
DEFAULT_OUTPUT = Path(__file__).with_name("coverage_base.json")

REFERENCE_FAMILIES = {
    "interface_text": ["ui/interface", "blit/copy", "post-process/imagespace"],
    "sky": ["sky"],
    "landscape": ["terrain/landscape"],
    "static_geometry": ["lighting/deferred/shadows"],
    "characters_animation": ["skinning/characters"],
    "vegetation": ["grass/trees"],
    "water": ["water"],
    "particles": ["particles/effects"],
}
SCOPE_NOTES = {
    "interface_text": (
        "The .fxp utility inventory is ui/interface plus blit/copy; the three "
        "imagespace copies are cross-family structural reuse. Scaleform glyphs "
        "have no .fxp and therefore contribute zero inventory matches."
    ),
    "sky": "Only sky-classified PS chunks are referenced; no implementation is claimed.",
    "landscape": "All terrain/landscape PS chunks are referenced; multi-pass planning is not implementation.",
    "static_geometry": (
        "Semantic subset of lighting/deferred/shadows, not a classifier family. "
        "Only structurally matched base-lit chunks count as static coverage."
    ),
    "characters_animation": "All skinning/characters chunks are referenced; no character plan is implemented.",
    "vegetation": (
        "grass/trees is genuinely zero shipped chunks. Vegetation selection must "
        "come from material/mesh metadata; no bytecode coverage is invented."
    ),
    "water": "All water PS chunks are referenced; reflection/refraction plans are not implemented.",
    "particles": (
        "particles/effects is referenced. BSEffectShaderProperty has no .fxp; "
        "fixed billboard coverage cannot be measured from this archive."
    ),
}


def write_dump(records: Iterable[packages.ChunkRecord], path: Path) -> int:
    records = list(records)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(b"D9PIPE01")
        handle.write(struct.pack("<I", len(records)))
        for record in records:
            package = record.package.path.encode("ascii")
            family = record.family.encode("ascii")
            program_hash = record.program_sha1.encode("ascii")
            code = record.chunk.code
            handle.write(struct.pack(
                "<IIIIHHB3x", len(code), record.chunk.meta, record.chunk.offset,
                record.chunk.trailer_size, len(package), len(family),
                len(program_hash),
            ))
            handle.write(package)
            handle.write(family)
            handle.write(program_hash)
            handle.write(code)
    return len(records)


def _empty_descriptor_row(pipeline: dict[str, Any]) -> dict[str, Any]:
    return {
        "support": pipeline["support"],
        "selected_chunks": 0,
        "selected_ps": 0,
        "selected_distinct_instruction_streams": 0,
        "host_plan_builds_with_all_contracts": 0,
        "host_plan_builds_without_preconditions": 0,
        "selected_by_source_family": {},
        "examples": [],
    }


def build_coverage(registry: dict[str, Any], family_summary: dict[str, Any],
                   dedup: dict[str, Any], measurement: str,
                   dataset: dict[str, Any]) -> dict[str, Any]:
    source_counts: dict[str, dict[str, int]] = defaultdict(
        lambda: {"chunks": 0, "ps": 0, "vs": 0})
    descriptor_rows = {
        pipeline["id"]: _empty_descriptor_row(pipeline)
        for pipeline in registry["pipelines"]
    }
    selected_hashes: dict[str, set[str]] = defaultdict(set)
    selected_source_counts: dict[str, Counter[str]] = defaultdict(Counter)
    selected_ps: Counter[str] = Counter()
    seen_records: set[tuple[str, int, int, str]] = set()
    parse_failures: list[dict[str, Any]] = []
    summary: dict[str, int] | None = None

    for line in measurement.splitlines():
        fields = line.split("\t")
        if fields[0] == "SOURCE_ROW" and len(fields) == 8:
            _, family, chunks, ps, vs, _meta, _offset, _hash = fields
            row = source_counts[family]
            row["chunks"] += int(chunks)
            row["ps"] += int(ps)
            row["vs"] += int(vs)
        elif fields[0] == "MATCH" and len(fields) == 15:
            (_, pipeline_id, pipeline_family, source_family, package, meta,
             offset, size, trailer, instructions, textures, constants,
             built, built_without_pre, program_hash) = fields
            key = (source_family, package, int(meta), int(offset), program_hash)
            if key in seen_records:
                raise ValueError(f"duplicate selected record in measurement: {key}")
            seen_records.add(key)
            if pipeline_id not in descriptor_rows:
                raise ValueError(f"measurement names unknown descriptor {pipeline_id}")
            row = descriptor_rows[pipeline_id]
            row["selected_chunks"] += 1
            row["selected_ps"] += 1
            row["host_plan_builds_with_all_contracts"] += int(built)
            row["host_plan_builds_without_preconditions"] += int(built_without_pre)
            source = row["selected_by_source_family"]
            source[source_family] = source.get(source_family, 0) + 1
            selected_hashes[pipeline_family].add(program_hash)
            selected_source_counts[pipeline_family][source_family] += 1
            selected_ps[pipeline_family] += 1
            if len(row["examples"]) < 3:
                row["examples"].append({
                    "package": package,
                    "meta": f"0x{int(meta):08x}",
                    "offset": int(offset),
                    "bytecode_bytes": int(size),
                    "reflection_trailer_bytes": int(trailer),
                    "instructions": int(instructions),
                    "texture_samples": int(textures),
                    "ctab_names": int(constants),
                })
        elif fields[0] == "PARSE_FAIL" and len(fields) == 5:
            parse_failures.append({
                "source_family": fields[1],
                "meta": f"0x{int(fields[2]):08x}",
                "offset": int(fields[3]),
                "reason": fields[4],
            })
        elif fields[0] == "DATASET_SUMMARY" and len(fields) == 5:
            summary = {
                "chunks": int(fields[1]), "ps": int(fields[2]),
                "vs": int(fields[3]), "parse_failures": int(fields[4]),
            }
        elif fields and fields[0] not in {"DATASET", "DATASET_SUMMARY"}:
            raise ValueError(f"unexpected measurement row: {line}")

    if summary is None:
        raise ValueError("measurement has no DATASET_SUMMARY")
    if summary["chunks"] != dedup["chunks"]:
        raise ValueError("dataset chunk count does not match shader inventory")
    expected_ps = sum(row["pixel_chunks"] for row in family_summary.values())
    expected_vs = sum(row["vertex_chunks"] for row in family_summary.values())
    if summary["ps"] != expected_ps or summary["vs"] != expected_vs:
        raise ValueError("dataset PS/VS split does not match shader inventory")
    if summary["parse_failures"] != len(parse_failures):
        raise ValueError("parse-failure count does not match measurement rows")
    for family, counts in source_counts.items():
        expected = family_summary[family]
        if (counts["chunks"] != expected["chunks"] or
                counts["ps"] != expected["pixel_chunks"] or
                counts["vs"] != expected["vertex_chunks"]):
            raise ValueError(f"source-family count mismatch for {family}")

    for row in descriptor_rows.values():
        row["selected_distinct_instruction_streams"] = 0  # filled per descriptor below

    descriptor_hashes: dict[str, set[str]] = defaultdict(set)
    for line in measurement.splitlines():
        fields = line.split("\t")
        if fields[0] == "MATCH" and len(fields) == 15:
            descriptor_hashes[fields[1]].add(fields[14])
    for pipeline_id, hashes in descriptor_hashes.items():
        descriptor_rows[pipeline_id]["selected_distinct_instruction_streams"] = len(hashes)

    pipeline_families: dict[str, Any] = {}
    for family in registry["families"]:
        name = family["name"]
        descriptors = [row for row in registry["pipelines"] if row["family"] == name]
        pipeline_families[name] = {
            "title": family["title"],
            "inventory_note": family["inventory_note"],
            "inventory_scope_note": SCOPE_NOTES[name],
            "reference_source_families": REFERENCE_FAMILIES[name],
            "reference_inventory": {
                source: {
                    "chunks": family_summary[source]["chunks"],
                    "ps": family_summary[source]["pixel_chunks"],
                    "vs": family_summary[source]["vertex_chunks"],
                    "distinct_instruction_streams": family_summary[source]["distinct_instruction_streams"],
                    "packages": family_summary[source]["packages"],
                }
                for source in REFERENCE_FAMILIES[name]
            },
            "registered_descriptors": [row["id"] for row in descriptors],
            "registered_support": {
                row["id"]: row["support"] for row in descriptors
            },
            "selected_chunks": selected_ps[name],
            "selected_ps": selected_ps[name],
            "selected_distinct_instruction_streams": len(selected_hashes[name]),
            "selected_by_source_family": dict(sorted(selected_source_counts[name].items())),
            "descriptors": {
                row["id"]: descriptor_rows[row["id"]] for row in descriptors
            },
        }

    return {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "dataset": dataset,
        "inventory_totals": {
            "packages_effective": dataset["packages_effective"],
            "chunks": dedup["chunks"],
            "ps": expected_ps,
            "vs": expected_vs,
            "distinct_instruction_streams": dedup["distinct_instruction_streams"],
            "distinct_opcode_sequences": dedup["distinct_opcode_sequences"],
        },
        "source_families": {
            family: {
                "chunks": row["chunks"],
                "ps": row["pixel_chunks"],
                "vs": row["vertex_chunks"],
                "distinct_instruction_streams": row["distinct_instruction_streams"],
                "distinct_opcode_sequences": row["distinct_opcode_sequences"],
                "packages": row["packages"],
            }
            for family, row in family_summary.items()
        },
        "pipeline_families": pipeline_families,
        "measurement": {
            "selection": "highest-priority implemented structural match across the registry",
            "plan_contracts": "all preprocessing bits supplied for host-plan capability",
            "hashes_used_for": "distinct instruction-stream counting only; never acceptance",
            "parse_failures": summary["parse_failures"],
            "parse_failure_examples": parse_failures[:10],
        },
        "status_note": (
            "Descriptor/plan coverage only. No D3D9 COM hook, GPU upload, asset "
            "preprocessing implementation or complete menu/world frame is claimed."
        ),
    }


def measure(data_dir: str, include_update: bool, exe: Path,
            output: Path, dump_path: Path | None) -> int:
    all_packages = packages.load_packages(data_dir, include_update=include_update)
    effective = packages.effective_packages(all_packages)
    records = packages.build_records(effective)
    summary = packages.family_summary(records)
    dedup = packages.dedup_summary(records)
    registry = json.loads(REGISTRY_JSON.read_text(encoding="utf-8"))

    temporary = False
    if dump_path is None:
        temp_root = Path(os.environ.get("TEMP", str(ROOT))) / "opencode"
        temp_root.mkdir(parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(prefix="pipeline_chunks_", suffix=".bin", dir=temp_root)
        os.close(fd)
        dump_path = Path(name)
        temporary = True
    try:
        count = write_dump(records, dump_path)
        result = subprocess.run(
            [str(exe), str(dump_path)], cwd=ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        dataset = {
            "archive": "Skyrim - Shaders.bsa",
            "include_update": include_update,
            "packages_read": len(all_packages),
            "packages_effective": len(effective),
            "dump_chunks": count,
            "dump_note": "temporary local bytecode; not committed",
        }
        coverage = build_coverage(registry, summary, dedup, result.stdout, dataset)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(coverage, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {output}")
        for name, row in coverage["pipeline_families"].items():
            print(f"{name:24s} selected={row['selected_chunks']:3d} "
                  f"streams={row['selected_distinct_instruction_streams']:3d} "
                  f"descriptors={','.join(row['registered_descriptors'])}")
        return result.returncode
    finally:
        if temporary:
            dump_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["measure", "dump"])
    parser.add_argument("--data", default=DEFAULT_DATA)
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--dump", type=Path)
    args = parser.parse_args()
    if args.command == "dump":
        if not args.dump:
            parser.error("dump requires --dump")
        records = packages.build_records(packages.effective_packages(
            packages.load_packages(args.data, include_update=args.update)))
        count = write_dump(records, args.dump)
        print(f"wrote {count} temporary chunk records to {args.dump}")
        return 0
    if not args.exe:
        parser.error("measure requires --exe (build tests/pipeline_real.c first)")
    return measure(args.data, args.update, args.exe, args.output, args.dump)


if __name__ == "__main__":
    raise SystemExit(main())
