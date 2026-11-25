"""Optional read-only retail sample for the menu preprocessors."""

from __future__ import annotations

import os
import time
from pathlib import Path
from typing import Dict, Optional

try:  # PYTHONPATH=tools
    from skyrim import bsa, shaders
except ImportError:  # repository root
    from tools.skyrim import bsa, shaders  # type: ignore

from .common import ContractError, DataError, Limits, atomic_write_json
from .normal import preprocess_normal
from .signatures import probe_bytecode

DEFAULT_DATA = r"E:\SteamLibrary\steamapps\common\Skyrim\Data"
DEFAULT_OUT = "build_host/menu_preprocess_sample"
NORMAL_TEXTURE = r"textures\interface\objects\logo02_n.dds"
DIFFUSE_TEXTURE = r"textures\interface\objects\logo02_d.dds"
SHADER_ARCHIVE = "Skyrim - Shaders.bsa"
TEXTURE_ARCHIVE = "Skyrim - Textures.bsa"
INTERFACE_ARCHIVE = "Skyrim - Interface.bsa"
SIGNATURE_PACKAGES = (
    r"shadersfx\lighting\059\p5.fxp",
    r"shadersfx\lighting\059\p1200005.fxp",
)


def _read_entry(archive: bsa.BsaArchive, name: str, limits: Limits) -> bytes:
    size = archive.unpacked_size(name)
    if size > limits.max_input_bytes:
        raise ContractError(
            "%s unpacked size %d exceeds max_input_bytes %d"
            % (name, size, limits.max_input_bytes)
        )
    return archive.read_file(name)


def _inside(child: Path, parent: Path) -> bool:
    try:
        child.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def run_sample(data_dir: os.PathLike[str] | str = DEFAULT_DATA,
               out_dir: os.PathLike[str] | str = DEFAULT_OUT,
               *, limits: Optional[Limits] = None) -> dict:
    """Read a few retail assets and write only derived build artefacts."""

    limits = limits or Limits()
    limits.validate()
    data = Path(data_dir)
    out = Path(out_dir)
    if not data.is_dir():
        raise DataError("retail Data directory not found: %s" % data)
    if _inside(out, data) or _inside(data, out):
        raise ContractError(
            "sample output must be outside the retail Data directory and its parents"
        )
    out.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    report = {
        "schema_version": 1,
        "kind": "menu_preprocess_real_sample",
        "status": "pass",
        "data_directory": str(data),
        "output_directory": str(out),
        "read_only_inputs": True,
        "game_files_modified": False,
        "limits": limits.as_dict(),
        "shader_signature_probe": {},
        "normal_map": {},
        "material_contract": {
            "status": "skipped",
            "reason": (
                "retail archives do not provide resolved runtime c0..c7 values "
                "or an extracted MVB1 tangent/bitangent/normal stream; the NIF "
                "inventory parser intentionally does not materialise vertices"
            ),
        },
        "scaleform_glyph": {},
        "runtime_integrated": False,
    }

    shader_path = data / SHADER_ARCHIVE
    texture_path = data / TEXTURE_ARCHIVE
    interface_path = data / INTERFACE_ARCHIVE
    for path in (shader_path, texture_path):
        if not path.is_file():
            raise DataError("required retail archive not found: %s" % path)

    matches = []
    package_rows = []
    with bsa.BsaArchive(shader_path) as archive:
        for package_name in SIGNATURE_PACKAGES:
            package_data = _read_entry(archive, package_name, limits)
            package = shaders.parse_package(package_name, package_data)
            counts: Dict[str, int] = {}
            for chunk in package.chunks:
                if chunk.kind != "ps":
                    continue
                try:
                    probe = probe_bytecode(chunk.code, require_ctab=True)
                except DataError:
                    counts["rejected"] = counts.get("rejected", 0) + 1
                    continue
                name = probe["signature"]
                counts[name] = counts.get(name, 0) + 1
                matches.append({
                    "package": package_name,
                    "chunk_offset": chunk.offset,
                    "meta": "0x%08x" % chunk.meta,
                    "bytecode_bytes": len(chunk.code),
                    "signature": name,
                    "instructions": probe["instructions"],
                    "c8": probe["c8"],
                    "ctab_bindings": probe["ctab"]["bindings"],
                })
            package_rows.append({
                "package": package_name,
                "file_bytes": len(package_data),
                "chunks": len(package.chunks),
                "counts": counts,
            })
    report["shader_signature_probe"] = {
        "status": "pass" if matches else "fail",
        "scope": (
            "partial host probe only; the C structural matcher remains authoritative"
        ),
        "packages": package_rows,
        "matches": matches,
        "match_count": len(matches),
        "signature_counts": {
            name: sum(1 for row in matches if row["signature"] == name)
            for name in ("p5_MAD_NORMAL", "p1200005_ADD_NORMAL")
        },
    }
    if not matches or any(
            sum(1 for row in matches if row["signature"] == name) == 0
            for name in ("p5_MAD_NORMAL", "p1200005_ADD_NORMAL")):
        report["status"] = "fail"
        report["errors"] = ["retail sample did not find both menu-light signatures"]
        atomic_write_json(out / "sample_report.json", report)
        raise DataError("retail sample did not find both menu-light signatures")

    with bsa.BsaArchive(texture_path) as archive:
        normal_data = _read_entry(archive, NORMAL_TEXTURE, limits)
        diffuse_data = _read_entry(archive, DIFFUSE_TEXTURE, limits)
    normal_rows = {}
    normal_hashes = []
    for signature in ("p5_MAD_NORMAL", "p1200005_ADD_NORMAL"):
        stem = "p5" if signature == "p5_MAD_NORMAL" else "p1200005"
        output = out / ("logo02_n.%s.normal.dds" % stem)
        manifest = out / ("logo02_n.%s.normal.json" % stem)
        row = preprocess_normal(
            normal_data,
            signature=signature,
            source_name=NORMAL_TEXTURE,
            output_path=output,
            report_path=manifest,
            limits=limits,
        )
        normal_hashes.append(row["output"]["sha256"])
        normal_rows[signature] = {
            "manifest": manifest.name,
            "output": output.name,
            "output_bytes": row["output"]["bytes"],
            "output_sha256": row["output"]["sha256"],
            "width": row["output"]["width"],
            "height": row["output"]["height"],
            "mip_count": row["output"]["mip_count"],
            "memory_peak_bytes": row["memory"]["peak_bytes"],
            "channel_order": row["validation"]["channel_order"],
            "source_channel_stats_top_mip":
                row["validation"]["source_channel_stats_top_mip"],
            "decoded_normal_stats_top_mip":
                row["validation"]["decoded_normal_stats_top_mip"],
            "warnings": row["validation"]["warnings"],
        }
    report["normal_map"] = {
        "status": "pass",
        "source": NORMAL_TEXTURE,
        "source_unpacked_bytes": len(normal_data),
        "diffuse_source": DIFFUSE_TEXTURE,
        "diffuse_unpacked_bytes": len(diffuse_data),
        "signature_outputs_identical": normal_hashes[0] == normal_hashes[1],
        "equivalence_note": (
            "for 8-bit UNORM, xzyw*2-1 and (xzyw-0.5)*2 followed by UNORM8 "
            "round-to-nearest produce identical XYZ bytes"
        ),
        "outputs": normal_rows,
    }

    if interface_path.is_file():
        with bsa.BsaArchive(interface_path) as archive:
            entries = archive.list_files()
            swf_fonts = [entry.path for entry in entries
                         if entry.path.lower().endswith(".swf")
                         and "font" in entry.path.lower()]
            exported_dds = sum(1 for entry in entries
                               if entry.extension == "dds"
                               and "exported" in entry.path.lower())
        report["scaleform_glyph"] = {
            "status": "skipped",
            "archive": INTERFACE_ARCHIVE,
            "files": len(entries),
            "font_swf_files": swf_fonts,
            "exported_icon_dds_files": exported_dds,
            "reason": (
                "Interface.bsa contains SWF fonts and exported controller-icon "
                "DDS files, but no verified Scaleform glyph-atlas descriptor or "
                "MGV1 quad stream. This tool does not parse SWF glyph programs; "
                "synthetic fixtures validate the fixed contract instead."
            ),
        }
    else:
        report["scaleform_glyph"] = {
            "status": "skipped",
            "reason": "Skyrim - Interface.bsa is not present",
        }

    report["elapsed_seconds"] = round(time.perf_counter() - started, 6)
    atomic_write_json(out / "sample_report.json", report)
    return report


__all__ = [
    "DEFAULT_DATA", "DEFAULT_OUT", "NORMAL_TEXTURE", "DIFFUSE_TEXTURE",
    "SHADER_ARCHIVE", "TEXTURE_ARCHIVE", "INTERFACE_ARCHIVE", "run_sample",
]
