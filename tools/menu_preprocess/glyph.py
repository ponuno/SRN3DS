"""Host validator for the fixed straight-alpha Scaleform glyph path.

There is no .fxp for this path.  Acceptance is an explicit material/atlas/quad
contract matching ``ui.scaleform_glyph`` in ``tools/pipelines/registry.json``.
"""

from __future__ import annotations

import hashlib
import math
import os
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

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
    require_keys,
)
from .ddsio import decode_level_accounted, load_dds_source

SCHEMA_VERSION = 1
MANIFEST_KIND = "scaleform_glyph"
VERTEX_MAGIC = b"MGV1"
VERTEX_VERSION = 1
VERTEX_RECORD_BYTES = 28
VERTICES_PER_QUAD = 6
CORNER_ORDER = (0, 1, 2, 2, 1, 3)

ATLAS_CONTRACT = {
    "alpha": "straight",
    "rgb": "white_mask",
    "color_space": "linear",
}
VERTEX_CONTRACT = {
    "topology": "triangle_list",
    "vertices_per_quad": VERTICES_PER_QUAD,
    "position": "clip_xyzw",
    "texcoord0": "normalized_2d",
    "color0": "rgba8_unorm",
}
STATE_CONTRACT = {
    "texture_units": 1,
    "sampler": "2d",
    "filter": "bilinear",
    "blend": "src_alpha_one_minus_src_alpha",
    "depth": "test_only_no_write",
    "alpha_test": "none",
    "tev_stages": 1,
    "constants": 0,
}
VARIANTS_CONTRACT = {
    "sdf": False,
    "outline": False,
    "shadow": False,
    "color_layers": False,
    "subpixel": False,
}


@dataclass(frozen=True)
class GlyphPolicy:
    greyscale_tolerance: int = 4
    min_alpha_span: int = 8
    min_partial_mean_rgb: int = 192
    clip_tolerance: float = 1.0e-5

    def validate(self) -> None:
        if (isinstance(self.greyscale_tolerance, bool)
                or not isinstance(self.greyscale_tolerance, int)
                or not 0 <= self.greyscale_tolerance <= 255):
            raise ContractError("greyscale_tolerance must be an integer in [0, 255]")
        if (isinstance(self.min_alpha_span, bool)
                or not isinstance(self.min_alpha_span, int)
                or not 1 <= self.min_alpha_span <= 255):
            raise ContractError("min_alpha_span must be an integer in [1, 255]")
        if (isinstance(self.min_partial_mean_rgb, bool)
                or not isinstance(self.min_partial_mean_rgb, int)
                or not 0 <= self.min_partial_mean_rgb <= 255):
            raise ContractError("min_partial_mean_rgb must be an integer in [0, 255]")
        if (isinstance(self.clip_tolerance, bool)
                or not isinstance(self.clip_tolerance, (int, float))
                or not 0.0 <= self.clip_tolerance <= 1.0):
            raise ContractError("clip_tolerance must be in [0, 1]")

    def as_dict(self) -> dict:
        self.validate()
        return {
            "greyscale_tolerance": self.greyscale_tolerance,
            "min_alpha_span": self.min_alpha_span,
            "min_partial_mean_rgb": self.min_partial_mean_rgb,
            "clip_tolerance": self.clip_tolerance,
        }


def _resolve(base: Path, value: Any, where: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ContractError("%s must be a non-empty path string" % where)
    path = Path(value)
    return path if path.is_absolute() else base / path


def _validate_atlas(path: Path, policy: GlyphPolicy,
                    limits: Limits) -> dict:
    source, meter = load_dds_source(path, limits, "glyph atlas")
    info = source.info
    if info.kind not in ("rgba8", "bgra8", "b5g5r5a1", "bc1", "bc2", "bc3"):
        raise ContractError(
            "glyph atlas format %s has no usable straight-alpha channel"
            % info.format_name
        )
    width, height, rgba = decode_level_accounted(
        source.data, info, 0, meter, "glyph atlas"
    )
    alpha_low, alpha_high = 255, 0
    alpha_sum = 0
    partial = 0
    zero = 0
    opaque = 0
    partial_rgb_sum = 0
    color_pixels = 0
    premultiplied_like = 0
    pixels = width * height
    for pos in range(0, len(rgba), 4):
        r, g, b, a = rgba[pos:pos + 4]
        alpha_low = min(alpha_low, a)
        alpha_high = max(alpha_high, a)
        alpha_sum += a
        if a == 0:
            zero += 1
        elif a == 255:
            opaque += 1
        else:
            partial += 1
            partial_rgb_sum += (r + g + b) / 3.0
            if max(r, g, b) <= a + policy.greyscale_tolerance:
                premultiplied_like += 1
        if max(r, g, b) - min(r, g, b) > policy.greyscale_tolerance:
            color_pixels += 1
    alpha_span = alpha_high - alpha_low
    if alpha_span < policy.min_alpha_span or alpha_low == alpha_high:
        raise ContractError(
            "glyph atlas alpha span %d is below min_alpha_span %d; missing "
            "or constant coverage" % (alpha_span, policy.min_alpha_span)
        )
    if alpha_low != 0 or alpha_high == 0:
        raise ContractError("glyph atlas must contain transparent coverage")
    if color_pixels:
        raise ContractError(
            "glyph atlas has %d non-greyscale pixels; colour-font layers are "
            "outside ui.scaleform_glyph" % color_pixels
        )
    partial_mean_rgb = partial_rgb_sum / partial if partial else 255.0
    if partial and partial_mean_rgb < policy.min_partial_mean_rgb:
        raise ContractError(
            "partial-alpha mean RGB %.3f is below %d; atlas looks "
            "premultiplied or shaded instead of a white straight-alpha mask"
            % (partial_mean_rgb, policy.min_partial_mean_rgb)
        )
    if partial and premultiplied_like * 100 >= partial * 95:
        raise ContractError(
            "glyph atlas partial-alpha RGB tracks alpha; premultiplied alpha "
            "is not accepted by the src-alpha registry contract"
        )
    meter.release("decoded_level")
    del rgba
    report = {
        "file": path.name,
        "sha256": hashlib.sha256(source.data).hexdigest(),
        "bytes": len(source.data),
        "width": width,
        "height": height,
        "mip_count": info.mip_count,
        "format": info.format_name,
        "linear_ldr": True,
        "alpha_min": alpha_low,
        "alpha_max": alpha_high,
        "alpha_mean": round(alpha_sum / pixels, 6),
        "transparent_pixels": zero,
        "partial_alpha_pixels": partial,
        "opaque_pixels": opaque,
        "partial_alpha_mean_rgb": round(partial_mean_rgb, 6),
        "non_greyscale_pixels": color_pixels,
        "premultiplied_like_partial_pixels": premultiplied_like,
        "memory": meter.as_dict(),
    }
    meter.release("source_dds")
    return report


def _clip_error(value: float, bound: float, tolerance: float) -> float:
    return max(0.0, -bound - value, value - bound) - tolerance


def _validate_vertices(path: Path, limits: Limits,
                       policy: GlyphPolicy) -> dict:
    try:
        size = path.stat().st_size
        with path.open("rb") as handle:
            header = handle.read(16)
    except OSError as exc:
        raise DataError("cannot read glyph vertices %s: %s" % (path, exc)) from exc
    if len(header) != 16:
        raise DataError("glyph vertex header is truncated")
    magic, version, count, stride = struct.unpack("<4sIII", header)
    if magic != VERTEX_MAGIC or version != VERTEX_VERSION:
        raise DataError("glyph vertices must be MGV1 version 1")
    if stride != VERTEX_RECORD_BYTES:
        raise DataError("glyph vertex stride must be %d" % VERTEX_RECORD_BYTES)
    if count == 0:
        raise ContractError("glyph vertex count must be positive")
    if count > limits.max_vertex_records:
        raise ContractError("glyph vertex count %d exceeds max_vertex_records %d"
                            % (count, limits.max_vertex_records))
    if count % VERTICES_PER_QUAD:
        raise ContractError("glyph vertex count must be divisible by 6")
    expected_size = 16 + count * stride
    if size != expected_size:
        raise DataError("glyph vertex file is %d bytes, expected exactly %d"
                        % (size, expected_size))
    if expected_size > limits.max_input_bytes:
        raise ContractError("glyph vertices exceed max_input_bytes %d"
                            % limits.max_input_bytes)

    meter = MemoryMeter(limit=limits.max_memory_bytes)
    digest = hashlib.sha256(header)
    chunk_quads = min(1024, count // VERTICES_PER_QUAD)
    chunk_size = chunk_quads * VERTICES_PER_QUAD * stride
    meter.acquire("glyph_vertex_chunk", chunk_size, "read glyph vertices")
    positive_winding = 0
    negative_winding = 0
    tint_alpha_zero_quads = 0
    try:
        with path.open("rb") as handle:
            handle.seek(16)
            remaining = count // VERTICES_PER_QUAD
            while remaining:
                quads = min(chunk_quads, remaining)
                chunk = handle.read(quads * VERTICES_PER_QUAD * stride)
                if len(chunk) != quads * VERTICES_PER_QUAD * stride:
                    raise DataError("glyph vertices became truncated while reading")
                digest.update(chunk)
                for base in range(0, len(chunk), VERTICES_PER_QUAD * stride):
                    records = []
                    for corner in range(VERTICES_PER_QUAD):
                        offset = base + corner * stride
                        x, y, z, w, u, v, r, g, b, a = struct.unpack_from(
                            "<4f2f4B", chunk, offset
                        )
                        if not all(math.isfinite(value) for value in (x, y, z, w, u, v)):
                            raise ContractError("glyph vertex contains a non-finite value")
                        if w <= 0.0:
                            raise ContractError("glyph clip-space w must be positive")
                        tolerance = policy.clip_tolerance * max(1.0, abs(w))
                        if (_clip_error(x, w, tolerance) > 0.0
                                or _clip_error(y, w, tolerance) > 0.0
                                or _clip_error(z, 0.0, tolerance) > 0.0
                                or _clip_error(z, w, tolerance) > 0.0):
                            raise ContractError("glyph position is outside the D3D clip volume")
                        if not (-tolerance <= u <= 1.0 + tolerance
                                and -tolerance <= v <= 1.0 + tolerance):
                            raise ContractError("glyph texcoord0 is outside [0,1]")
                        records.append(((x, y, z, w), (u, v), (r, g, b, a)))
                    corners = [None] * 4
                    for vertex, corner_index in enumerate(CORNER_ORDER):
                        record = records[vertex]
                        if corners[corner_index] is None:
                            corners[corner_index] = record
                        elif record != corners[corner_index]:
                            raise ContractError(
                                "glyph quad vertices do not share canonical "
                                "corner %d (expected order 0,1,2,2,1,3)"
                                % corner_index
                            )
                    p0, p1, p2, _p3 = [record[0] for record in corners]
                    uv0, uv1, uv2, _uv3 = [record[1] for record in corners]
                    area = ((p1[0] - p0[0]) * (p2[1] - p0[1])
                            - (p1[1] - p0[1]) * (p2[0] - p0[0]))
                    uv_area = ((uv1[0] - uv0[0]) * (uv2[1] - uv0[1])
                               - (uv1[1] - uv0[1]) * (uv2[0] - uv0[0]))
                    if abs(area) <= 1.0e-12 or abs(uv_area) <= 1.0e-12:
                        raise ContractError("glyph quad has zero position or UV area")
                    if area > 0:
                        positive_winding += 1
                    else:
                        negative_winding += 1
                    if corners[0][2][3] == 0:
                        tint_alpha_zero_quads += 1
                remaining -= quads
        chunk = b""
    finally:
        meter.release("glyph_vertex_chunk")
    return {
        "file": path.name,
        "sha256": digest.hexdigest(),
        "bytes": size,
        "format": "MGV1",
        "version": VERTEX_VERSION,
        "record_bytes": stride,
        "record_layout": "clip.xyzw float32, uv float2, tint RGBA8 (little-endian)",
        "vertices": count,
        "quads": count // VERTICES_PER_QUAD,
        "canonical_corner_order": list(CORNER_ORDER),
        "positive_winding_quads": positive_winding,
        "negative_winding_quads": negative_winding,
        "zero_tint_alpha_quads": tint_alpha_zero_quads,
        "memory": meter.as_dict(),
    }


def validate_glyph(
    descriptor_path: os.PathLike[str] | str,
    *,
    output_path: Optional[os.PathLike[str] | str] = None,
    policy: Optional[GlyphPolicy] = None,
    limits: Optional[Limits] = None,
) -> dict:
    """Validate material, atlas and quad data for ``ui.scaleform_glyph``."""

    limits = limits or Limits()
    limits.validate()
    policy = policy or GlyphPolicy()
    policy.validate()
    path = Path(descriptor_path)
    descriptor = load_json(path, limits, "glyph descriptor")
    require_keys(descriptor, [
        "schema_version", "material", "bytecode", "atlas", "atlas_contract",
        "vertices", "vertex_contract", "state", "variants",
    ], "glyph descriptor")
    require_exact(descriptor["schema_version"], SCHEMA_VERSION,
                  "glyph.schema_version")
    require_exact(descriptor["bytecode"], None,
                  "glyph.bytecode (no .fxp exists for this path)")
    material = descriptor["material"]
    require_keys(material, ["source", "kind"], "glyph.material")
    require_exact(material["source"], "fixed_scaleform", "glyph.material.source")
    require_exact(material["kind"], "scaleform_glyph_atlas",
                  "glyph.material.kind")
    for key, expected in (("atlas_contract", ATLAS_CONTRACT),
                          ("vertex_contract", VERTEX_CONTRACT),
                          ("state", STATE_CONTRACT),
                          ("variants", VARIANTS_CONTRACT)):
        row = descriptor[key]
        require_keys(row, sorted(expected), "glyph.%s" % key)
        for field, value in expected.items():
            require_exact(row[field], value, "glyph.%s.%s" % (key, field))

    registry = load_registry(limits)
    row = registry_pipeline(registry, "ui.scaleform_glyph")
    require_exact(row.get("source"), "fixed_scaleform", "registry.source")
    require_exact(row.get("fixed_material"), "scaleform_glyph_atlas",
                  "registry.fixed_material")
    features = row.get("features", {})
    for key, expected in (("min_textures", 1), ("max_textures", 1),
                          ("max_samplers", 1), ("max_tev_stages", 1),
                          ("max_color_inputs", 1), ("require_2d", True),
                          ("require_ldr", True)):
        require_exact(features.get(key), expected, "registry.features.%s" % key)

    atlas_path = _resolve(path.parent, descriptor["atlas"], "glyph.atlas")
    vertices_path = _resolve(path.parent, descriptor["vertices"], "glyph.vertices")
    atlas = _validate_atlas(atlas_path, policy, limits)
    vertices = _validate_vertices(vertices_path, limits, policy)
    report = {
        "schema_version": SCHEMA_VERSION,
        "kind": MANIFEST_KIND,
        "status": "pass",
        "pipeline": "ui.scaleform_glyph",
        "material": material,
        "bytecode": None,
        "atlas_contract": descriptor["atlas_contract"],
        "atlas": atlas,
        "vertex_contract": descriptor["vertex_contract"],
        "vertices": vertices,
        "state": descriptor["state"],
        "variants": descriptor["variants"],
        "fixed_pass": {
            "stages": 1,
            "textures": 1,
            "summary": "RGB/A = straight-alpha atlas * PRIMARY vertex tint",
            "culling": "disabled; both quad windings are accepted",
        },
        "policy": policy.as_dict(),
        "limits": limits.as_dict(),
        "registry_checked": True,
        "runtime_integrated": False,
    }
    if output_path is not None:
        atomic_write_json(output_path, report)
    return report


__all__ = [
    "SCHEMA_VERSION", "MANIFEST_KIND", "VERTEX_MAGIC", "VERTEX_VERSION",
    "VERTEX_RECORD_BYTES", "VERTICES_PER_QUAD", "CORNER_ORDER",
    "ATLAS_CONTRACT", "VERTEX_CONTRACT", "STATE_CONTRACT", "VARIANTS_CONTRACT",
    "GlyphPolicy", "validate_glyph",
]
