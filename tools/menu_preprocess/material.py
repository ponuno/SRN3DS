"""Tangent-basis and c0..c7 material contract for the menu-light path."""

from __future__ import annotations

import hashlib
import math
import os
import struct
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

from .common import (
    ContractError,
    DataError,
    Limits,
    MemoryMeter,
    atomic_write_json,
    load_json,
    load_registry,
    registry_pipeline,
    require_exact,
    require_int,
    require_keys,
    sha256_file,
    vector,
)
from .ddsio import inspect_dds_file
from .normal import (
    MANIFEST_KIND as NORMAL_MANIFEST_KIND,
    OUTPUT_KIND,
    SCHEMA_VERSION as NORMAL_SCHEMA_VERSION,
)
from .signatures import Signature, normalize_signature

SCHEMA_VERSION = 1
MANIFEST_KIND = "menu_light_material"
BASIS_MAGIC = b"MVB1"
BASIS_VERSION = 1
BASIS_RECORD_BYTES = 36
BASIS_TOLERANCE = 2.0e-4

VERTEX_DECLARATION = {
    "position": {"components": 4, "space": "clip"},
    "texcoord0": {"components": 2},
    "color0": {"components": 4, "encoding": "rgba8_unorm"},
    "color1": {"components": 4, "encoding": "rgba8_unorm",
               "rgb": "ambient_plus_emissive"},
    "normal": {"components": 3, "space": "lighting"},
    "tangent": {"components": 3, "space": "lighting"},
    "bitangent": {"components": 3, "space": "lighting", "source": "stored"},
}

PRECONDITIONS = {
    "normal_decoded": True,
    "basis_prebaked": True,
    "secondary_rgb": "ambient_plus_emissive",
    "material_alpha_register": "c4",
    "fog_rgb_register": "c7",
    "fog_factor_component": "a",
}

CTAB_BINDINGS = {
    "DiffuseSampler": (3, 0),
    "NormalSampler": (3, 1),
    "DirLightDirection": (2, 3),
    "DirLightColor": (2, 4),
    "MaterialData": (2, 5),
    "EmitColor": (2, 6),
    "FogColor": (2, 7),
    "DirectionalAmbient": (2, 0),
}


def _resolve(base: Path, value: Any, where: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ContractError("%s must be a non-empty path string" % where)
    path = Path(value)
    return path if path.is_absolute() else base / path


def _dot(a: Tuple[float, ...], b: Tuple[float, ...]) -> float:
    return sum(x * y for x, y in zip(a, b))


def _cross(a: Tuple[float, ...], b: Tuple[float, ...]) -> Tuple[float, float, float]:
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _length(a: Tuple[float, ...]) -> float:
    return math.sqrt(_dot(a, a))


def _validate_basis_rows(rows: Tuple[Tuple[float, ...], ...], where: str) -> dict:
    for index, row in enumerate(rows):
        if len(row) == 4 and abs(row[3]) > 1.0e-6:
            raise ContractError("%s.c%d.w must be zero for a vector basis row"
                                % (where, index))
        if abs(_length(row[:3]) - 1.0) > BASIS_TOLERANCE:
            raise ContractError("%s.c%d.xyz is not unit length" % (where, index))
    dots = (_dot(rows[0][:3], rows[1][:3]),
            _dot(rows[0][:3], rows[2][:3]),
            _dot(rows[1][:3], rows[2][:3]))
    if any(abs(value) > BASIS_TOLERANCE for value in dots):
        raise ContractError("%s tangent/bitangent/normal rows are not orthogonal"
                            % where)
    determinant = _dot(rows[0][:3], _cross(rows[1][:3], rows[2][:3]))
    if abs(determinant - 1.0) > BASIS_TOLERANCE:
        raise ContractError(
            "%s basis determinant %.9f is not +1; expected right-handed "
            "bitangent = normal x tangent" % (where, determinant)
        )
    return {
        "orthogonal_dots": [round(value, 9) for value in dots],
        "determinant": round(determinant, 9),
        "handedness": "right",
        "tolerance": BASIS_TOLERANCE,
    }


def _validate_constants(value: Any) -> Dict[str, Tuple[float, ...]]:
    require_keys(value, ["c%d" % i for i in range(8)], "material.constants")
    constants = {name: vector(value[name], 4, "material.constants.%s" % name)
                 for name in ("c%d" % i for i in range(8))}
    rows = (constants["c0"], constants["c1"], constants["c2"])
    basis = _validate_basis_rows(rows, "material.constants")

    direction = constants["c3"]
    if abs(_length(direction[:3]) - 1.0) > BASIS_TOLERANCE:
        raise ContractError("material.constants.c3.xyz must be a unit light direction")
    if any(not 0.0 <= v <= 1.0 for v in direction):
        raise ContractError(
            "material.constants.c3 must be representable as positive RGBA8 UNORM "
            "for the pre-baked TEV plan"
        )
    for name in ("c4", "c6", "c7"):
        if any(not 0.0 <= v <= 1.0 for v in constants[name]):
            raise ContractError("material.constants.%s must be LDR [0,1]" % name)
    if not 0.0 <= constants["c5"][2] <= 1.0:
        raise ContractError("material.constants.c5.z material-alpha scale must be [0,1]")
    return {"constants": constants, "basis": basis}


def _validate_ctab(value: Any) -> dict:
    require_keys(value, sorted(CTAB_BINDINGS), "material.ctab")
    out = {}
    for name, expected in CTAB_BINDINGS.items():
        row = value[name]
        require_keys(row, ["set", "index"], "material.ctab.%s" % name,
                     optional=(["disposition"] if name == "DirectionalAmbient" else []))
        register_set = require_int(row["set"], 0, 65535,
                                   "material.ctab.%s.set" % name)
        index = require_int(row["index"], 0, 65535,
                            "material.ctab.%s.index" % name)
        if (register_set, index) != expected:
            raise ContractError(
                "material.ctab.%s binds set %d index %d, expected set %d index %d"
                % (name, register_set, index, expected[0], expected[1])
            )
        disposition = row.get("disposition")
        if name == "DirectionalAmbient":
            require_exact(disposition, "unused_shadowed_by_basis",
                          "material.ctab.DirectionalAmbient.disposition")
        elif "disposition" in row:
            raise ContractError("material.ctab.%s has contradictory disposition" % name)
        out[name] = {"set": register_set, "index": index}
        if disposition is not None:
            out[name]["disposition"] = disposition
    return out


def _validate_vertex_declaration(value: Any) -> None:
    require_keys(value, sorted(VERTEX_DECLARATION), "material.vertex_declaration")
    for name, expected in VERTEX_DECLARATION.items():
        row = value[name]
        require_keys(row, sorted(expected), "material.vertex_declaration.%s" % name)
        for key, expected_value in expected.items():
            require_exact(row[key], expected_value,
                          "material.vertex_declaration.%s.%s" % (name, key))


def _validate_preconditions(value: Any) -> None:
    require_keys(value, sorted(PRECONDITIONS), "material.preconditions")
    for key, expected in PRECONDITIONS.items():
        require_exact(value[key], expected, "material.preconditions.%s" % key)


def _validate_basis_file(path: Path, rows: Tuple[Tuple[float, ...], ...],
                         limits: Limits) -> dict:
    try:
        size = path.stat().st_size
        with path.open("rb") as handle:
            header = handle.read(16)
    except OSError as exc:
        raise DataError("cannot read vertex basis %s: %s" % (path, exc)) from exc
    if len(header) != 16:
        raise DataError("vertex basis header is truncated")
    magic, version, count, stride = struct.unpack("<4sIII", header)
    if magic != BASIS_MAGIC or version != BASIS_VERSION:
        raise DataError("vertex basis must be MVB1 version 1")
    if stride != BASIS_RECORD_BYTES:
        raise DataError("vertex basis stride must be %d" % BASIS_RECORD_BYTES)
    if count == 0:
        raise ContractError("vertex basis contains no records")
    if count > limits.max_vertex_records:
        raise ContractError(
            "vertex basis count %d exceeds max_vertex_records %d"
            % (count, limits.max_vertex_records)
        )
    expected_size = 16 + count * stride
    if size != expected_size:
        raise DataError(
            "vertex basis is %d bytes, expected exactly %d for %d records"
            % (size, expected_size, count)
        )
    if expected_size > limits.max_input_bytes:
        raise ContractError("vertex basis exceeds max_input_bytes %d"
                            % limits.max_input_bytes)

    meter = MemoryMeter(limit=limits.max_memory_bytes)
    digest = hashlib.sha256(header)
    chunk_records = min(4096, count)
    chunk_size = chunk_records * stride
    meter.acquire("vertex_basis_chunk", chunk_size, "read vertex basis")
    maximum_row_error = 0.0
    try:
        with path.open("rb") as handle:
            handle.seek(16)
            remaining = count
            while remaining:
                take = min(chunk_records, remaining)
                chunk = handle.read(take * stride)
                if len(chunk) != take * stride:
                    raise DataError("vertex basis became truncated while reading")
                digest.update(chunk)
                for offset in range(0, len(chunk), stride):
                    values = struct.unpack_from("<9f", chunk, offset)
                    if not all(math.isfinite(v) for v in values):
                        raise ContractError("vertex basis contains a non-finite vector")
                    tangent = tuple(values[0:3])
                    bitangent = tuple(values[3:6])
                    normal = tuple(values[6:9])
                    _validate_basis_rows((tangent, bitangent, normal),
                                         "vertex basis record")
                    for axis, vertex_row in enumerate((tangent, bitangent, normal)):
                        error = max(abs(vertex_row[i] - rows[axis][i])
                                    for i in range(3))
                        maximum_row_error = max(maximum_row_error, error)
                        if error > BASIS_TOLERANCE:
                            raise ContractError(
                                "vertex basis record contradicts constants.c%d "
                                "(max component error %.9f > %.9f); varying "
                                "per-vertex basis cannot use one pre-baked normal map"
                                % (axis, error, BASIS_TOLERANCE)
                            )
                remaining -= take
        chunk = b""
    finally:
        meter.release("vertex_basis_chunk")
    return {
        "format": "MVB1",
        "version": BASIS_VERSION,
        "record_bytes": stride,
        "record_layout": "tangent.xyz, bitangent.xyz, normal.xyz (float32 LE)",
        "records": count,
        "sha256": digest.hexdigest(),
        "maximum_constant_row_error": round(maximum_row_error, 9),
        "memory": meter.as_dict(),
    }


def _validate_normal_manifest(path: Path, signature: Signature,
                              limits: Limits) -> dict:
    manifest = load_json(path, limits, "normal manifest")
    require_exact(manifest.get("schema_version"), NORMAL_SCHEMA_VERSION,
                  "normal manifest.schema_version")
    require_exact(manifest.get("kind"), NORMAL_MANIFEST_KIND, "normal manifest.kind")
    require_exact(manifest.get("status"), "pass", "normal manifest.status")
    sig = manifest.get("signature")
    if not isinstance(sig, dict):
        raise ContractError("normal manifest.signature must be an object")
    require_exact(sig.get("name"), signature.name,
                  "normal manifest.signature.name")
    output = manifest.get("output")
    if not isinstance(output, dict):
        raise ContractError("normal manifest.output is required")
    require_exact(output.get("kind"), OUTPUT_KIND, "normal manifest.output.kind")
    require_exact(output.get("channel_order"), "xyz", "normal manifest.output.channel_order")
    require_exact(output.get("alpha"), 255, "normal manifest.output.alpha")
    require_exact(output.get("linear"), True, "normal manifest.output.linear")
    require_exact(output.get("already_decoded"), True,
                  "normal manifest.output.already_decoded")
    output_path = _resolve(path.parent, output.get("file"), "normal manifest.output.file")
    info, size = inspect_dds_file(output_path, limits, "normal output",
                                  linear_only=True)
    if info.kind != "rgba8":
        raise ContractError("normal output DDS must be uncompressed RGBA8")
    for key in ("width", "height", "mip_count", "bytes"):
        require_exact(output.get(key), {"width": info.width,
                                        "height": info.height,
                                        "mip_count": info.mip_count,
                                        "bytes": size}[key],
                      "normal manifest.output.%s" % key)
    digest = sha256_file(output_path)
    require_exact(output.get("sha256"), digest, "normal manifest.output.sha256")
    return {
        "manifest_file": path.name,
        "manifest_sha256": sha256_file(path),
        "signature": signature.name,
        "output_file": output_path.name,
        "output_sha256": digest,
        "width": info.width,
        "height": info.height,
        "mip_count": info.mip_count,
        "bytes": size,
    }


def _pack_unorm(value: float) -> int:
    return max(0, min(255, int(value * 255.0 + 0.5)))


def validate_material(
    descriptor_path: os.PathLike[str] | str,
    *,
    output_path: Optional[os.PathLike[str] | str] = None,
    limits: Optional[Limits] = None,
) -> dict:
    """Validate a complete menu-light material hand-off descriptor."""

    limits = limits or Limits()
    limits.validate()
    path = Path(descriptor_path)
    descriptor = load_json(path, limits, "material descriptor")
    require_keys(descriptor, [
        "schema_version", "pipeline", "signature", "constants", "ctab",
        "ambient_rgb", "normal_map", "diffuse_texture", "vertex_declaration",
        "basis", "preconditions",
    ], "material descriptor")
    require_exact(descriptor["schema_version"], SCHEMA_VERSION,
                  "material.schema_version")
    signature = normalize_signature(descriptor["signature"])
    require_exact(descriptor["pipeline"], signature.pipeline_id,
                  "material.pipeline")

    registry = load_registry(limits)
    row = registry_pipeline(registry, signature.pipeline_id)
    require_exact(row.get("menu_signature"),
                  {"p5_MAD_NORMAL": "p5_mad_normal",
                   "p1200005_ADD_NORMAL": "p1200005_add_normal"}[signature.name],
                  "registry.menu_signature")
    required_preconditions = {
        "normal_prebaked", "tangent_basis_prebaked",
        "ambient_emissive_varying", "material_alpha_c4",
    }
    if set(row.get("preconditions", [])) != required_preconditions:
        raise DataError("registry menu-light preconditions changed unexpectedly")

    checked = _validate_constants(descriptor["constants"])
    constants = checked["constants"]
    ctab = _validate_ctab(descriptor["ctab"])
    _validate_vertex_declaration(descriptor["vertex_declaration"])
    _validate_preconditions(descriptor["preconditions"])
    ambient = vector(descriptor["ambient_rgb"], 3, "material.ambient_rgb")
    if any(not 0.0 <= v <= 1.0 for v in ambient):
        raise ContractError("material.ambient_rgb must be LDR [0,1]")

    basis = descriptor["basis"]
    require_keys(basis, ["scope", "handedness", "records"], "material.basis")
    require_exact(basis["scope"], "constant", "material.basis.scope")
    require_exact(basis["handedness"], "right", "material.basis.handedness")
    basis_path = _resolve(path.parent, basis["records"], "material.basis.records")
    basis_report = _validate_basis_file(
        basis_path, (constants["c0"], constants["c1"], constants["c2"]), limits
    )

    normal_path = _resolve(path.parent, descriptor["normal_map"],
                           "material.normal_map")
    normal_report = _validate_normal_manifest(normal_path, signature, limits)
    diffuse_path = _resolve(path.parent, descriptor["diffuse_texture"],
                            "material.diffuse_texture")
    diffuse_info, diffuse_size = inspect_dds_file(
        diffuse_path, limits, "diffuse texture", linear_only=False
    )
    if (diffuse_info.width, diffuse_info.height) != (
            normal_report["width"], normal_report["height"]):
        raise ContractError(
            "diffuse texture %dx%d does not match normal output %dx%d"
            % (diffuse_info.width, diffuse_info.height,
               normal_report["width"], normal_report["height"])
        )

    plan_constants = {
        "c3": [_pack_unorm(v) for v in constants["c3"]],
        "c4": [_pack_unorm(v) for v in constants["c4"][:3]]
              + [_pack_unorm(constants["c5"][2])],
        "c7": [_pack_unorm(v) for v in constants["c7"]],
    }
    secondary_rgb_raw = [ambient[i] + constants["c6"][i] for i in range(3)]
    secondary_rgb = [min(1.0, value) for value in secondary_rgb_raw]
    report = {
        "schema_version": SCHEMA_VERSION,
        "kind": MANIFEST_KIND,
        "status": "pass",
        "pipeline": signature.pipeline_id,
        "signature": signature.name,
        "source_constants": {name: list(value)
                             for name, value in constants.items()},
        "ctab": ctab,
        "ctab_note": (
            "DirectionalAmbient's measured c0 binding is vocabulary only here; "
            "the accepted PS consumes c0..c2 as basis rows. Ambient RGB is "
            "supplied explicitly and folded into color1, never substituted for c0."
        ),
        "vertex_declaration": descriptor["vertex_declaration"],
        "vertex_basis": basis_report,
        "basis_validation": checked["basis"],
        "normal_map": normal_report,
        "diffuse_texture": {
            "file": diffuse_path.name,
            "sha256": sha256_file(diffuse_path),
            "bytes": diffuse_size,
            "width": diffuse_info.width,
            "height": diffuse_info.height,
            "mip_count": diffuse_info.mip_count,
            "format": diffuse_info.format_name,
        },
        "preconditions": descriptor["preconditions"],
        "future_loader_inputs": {
            "normal_texture": "already-decoded RGBA8 XYZ; sample directly, no xzyw/decode",
            "color1_rgb": [round(v, 9) for v in secondary_rgb],
            "color1_rgb_unclamped": [round(v, 9) for v in secondary_rgb_raw],
            "color1_rgb_clamped": secondary_rgb != secondary_rgb_raw,
            "color1_rgb_source": "clamp01(material.ambient_rgb + c6.rgb)",
            "material_alpha": {
                "source": "c5.z",
                "target": "c4.a",
                "value": constants["c5"][2],
            },
            "tev_constants_rgba8": plan_constants,
            "basis_rows": {
                "c0": "tangent", "c1": "bitangent", "c2": "normal",
                "consumed_offline": True,
            },
        },
        "registry_checked": True,
        "runtime_integrated": False,
    }
    if output_path is not None:
        atomic_write_json(output_path, report)
    return report


__all__ = [
    "SCHEMA_VERSION", "MANIFEST_KIND", "BASIS_MAGIC", "BASIS_VERSION",
    "BASIS_RECORD_BYTES", "VERTEX_DECLARATION", "PRECONDITIONS",
    "CTAB_BINDINGS", "validate_material",
]
