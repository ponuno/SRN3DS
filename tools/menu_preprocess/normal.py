"""Menu-light normal-map validation and offline UNORM normalisation."""

from __future__ import annotations

import math
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional, Tuple

from .common import (
    ContractError,
    DataError,
    Limits,
    MenuPreprocessError,
    atomic_write_json,
    ensure_distinct,
    sha256_bytes,
    sha256_file,
)
from .ddsio import (
    decode_level_accounted,
    load_dds_source,
    rgba8_dds_header,
    rgba8_output_size,
)
from .signatures import (
    Signature,
    decode_linear_byte,
    decode_normal_components,
    normalize_signature,
)

SCHEMA_VERSION = 1
OUTPUT_KIND = "rgba8_xyz_opaque"
MANIFEST_KIND = "menu_light_normal"


@dataclass(frozen=True)
class NormalPolicy:
    """Statistical contradiction gates, not a proof of semantic axis labels."""

    source_layout: str = "auto"       # auto | rgba_xzyw | rgba_xyzw
    allow_layout_conversion: bool = False
    min_used_channel_span: int = 8
    max_mean_normal_length: Optional[float] = None
    max_zero_normal_fraction: float = 0.25

    def validate(self) -> None:
        if self.source_layout not in ("auto", "rgba_xzyw", "rgba_xyzw"):
            raise ContractError("normal source_layout must be auto, rgba_xzyw or rgba_xyzw")
        if not isinstance(self.allow_layout_conversion, bool):
            raise ContractError("allow_layout_conversion must be boolean")
        if (isinstance(self.min_used_channel_span, bool)
                or not isinstance(self.min_used_channel_span, int)
                or not 0 <= self.min_used_channel_span <= 255):
            raise ContractError("min_used_channel_span must be an integer in [0, 255]")
        if self.max_mean_normal_length is not None and (
                isinstance(self.max_mean_normal_length, bool)
                or not isinstance(self.max_mean_normal_length, (int, float))
                or not 0.0 < self.max_mean_normal_length <= 1.0):
            raise ContractError("max_mean_normal_length must be None or in (0, 1]")
        if (isinstance(self.max_zero_normal_fraction, bool)
                or not isinstance(self.max_zero_normal_fraction, (int, float))
                or not 0.0 <= self.max_zero_normal_fraction <= 1.0):
            raise ContractError("max_zero_normal_fraction must be in [0, 1]")

    def as_dict(self) -> dict:
        self.validate()
        return {
            "source_layout": self.source_layout,
            "allow_layout_conversion": self.allow_layout_conversion,
            "min_used_channel_span": self.min_used_channel_span,
            "max_mean_normal_length": self.max_mean_normal_length,
            "max_zero_normal_fraction": self.max_zero_normal_fraction,
        }


def _channel_stats(rgba: bytes) -> Dict[str, dict]:
    channels = []
    for component in range(4):
        low = 255
        high = 0
        total = 0
        for pos in range(component, len(rgba), 4):
            value = rgba[pos]
            low = min(low, value)
            high = max(high, value)
            total += value
        count = len(rgba) // 4
        channels.append({
            "min": low,
            "max": high,
            "mean": round(total / count, 6),
            "span": high - low,
        })
    return dict(zip(("r", "g", "b", "a"), channels))


def _detect_layout(stats: Dict[str, dict], policy: NormalPolicy) -> Tuple[str, dict]:
    alpha = stats["a"]["mean"]
    blue = stats["b"]["mean"]
    evidence = {
        "alpha_mean": alpha,
        "blue_mean": blue,
        "rule": (
            "retail xzyw uses A as normal Y; conventional xyzw uses A as "
            "opaque coverage. Only a clearly opaque A mean is classified xyzw. "
            "B is unused by retail xzyw and is not required to be neutral."
        ),
    }
    if alpha >= 224.0:
        evidence["detected"] = "rgba_xyzw"
        evidence["confidence"] = "high"
    else:
        evidence["detected"] = "rgba_xzyw"
        evidence["confidence"] = "high" if alpha < 192.0 else "medium"

    requested = policy.source_layout
    if requested == "auto":
        chosen = evidence["detected"]
    else:
        chosen = requested
        if chosen != evidence["detected"]:
            raise ContractError(
                "normal channel-order contradiction: declared %s but pixel "
                "statistics indicate %s" % (requested, evidence["detected"])
            )
    if chosen == "rgba_xyzw" and not policy.allow_layout_conversion:
        raise ContractError(
            "normal channel order is rgba_xyzw, but both retail menu-light "
            "signatures require rgba_xzyw (R=X, A=Y, G=Z); set "
            "allow_layout_conversion=true only for an intentional conversion"
        )
    evidence["selected"] = chosen
    evidence["retail_xzyw_required"] = True
    return chosen, evidence


def _normal_stats(rgba: bytes, signature: Signature, layout: str,
                  pixels: int) -> dict:
    sums = [0.0, 0.0, 0.0]
    square_sums = [0.0, 0.0, 0.0]
    low = [1.0, 1.0, 1.0]
    high = [-1.0, -1.0, -1.0]
    zero = 0
    for pos in range(0, len(rgba), 4):
        r, g, b, a = rgba[pos:pos + 4]
        if layout == "rgba_xzyw":
            x, y, z = decode_linear_byte(signature, r, g, a)
        else:
            raw_x, raw_y, raw_z = r / 255.0, g / 255.0, b / 255.0
            if signature.name == "p5_MAD_NORMAL":
                x, y, z = (raw_x * 2.0 - 1.0, raw_y * 2.0 - 1.0,
                           raw_z * 2.0 - 1.0)
            else:
                x, y, z = ((raw_x - 0.5) * 2.0, (raw_y - 0.5) * 2.0,
                           (raw_z - 0.5) * 2.0)
        value = (x, y, z)
        for axis in range(3):
            sums[axis] += value[axis]
            square_sums[axis] += value[axis] * value[axis]
            low[axis] = min(low[axis], value[axis])
            high[axis] = max(high[axis], value[axis])
        if x * x + y * y + z * z <= 1.0e-12:
            zero += 1
    means = [total / pixels for total in sums]
    rms = [math.sqrt(total / pixels) for total in square_sums]
    return {
        "scope": "top_mip",
        "pixels": pixels,
        "mean_xyz": [round(v, 9) for v in means],
        "rms_xyz": [round(v, 9) for v in rms],
        "min_xyz": [round(v, 9) for v in low],
        "max_xyz": [round(v, 9) for v in high],
        "mean_vector_length": round(math.sqrt(sum(v * v for v in means)), 9),
        "zero_vector_fraction": round(zero / pixels, 9),
    }


def _validate_range(stats: Dict[str, dict], normal: dict,
                    policy: NormalPolicy, layout: str) -> Tuple[list, list]:
    errors = []
    warnings = []
    used = ("r", "g", "a") if layout == "rgba_xzyw" else ("r", "g", "b")
    constant_channels = []
    for channel in used:
        if stats[channel]["span"] < policy.min_used_channel_span:
            constant_channels.append(channel)
    if len(constant_channels) == len(used):
        errors.append(
            "all used normal channels (%s) are constant; wrong-range or "
            "degenerate normal data" % ", ".join(constant_channels)
        )
    elif constant_channels:
        warnings.append(
            "constant normal component(s) %s; allowed for axis-aligned flats, "
            "but inspect provenance" % ", ".join(constant_channels)
        )
    if policy.max_mean_normal_length is not None:
        if normal["mean_vector_length"] > policy.max_mean_normal_length:
            errors.append(
                "decoded mean normal length %.6f exceeds strict-range limit "
                "%.6f; likely signed/rebased or scaled range mismatch"
                % (normal["mean_vector_length"], policy.max_mean_normal_length)
            )
        elif normal["mean_vector_length"] > 0.35:
            warnings.append(
                "decoded mean normal length %.6f shows a strong directional bias"
                % normal["mean_vector_length"]
            )
    elif normal["mean_vector_length"] > 0.35:
        warnings.append(
            "decoded mean normal length %.6f shows a strong directional bias; "
            "use strict-range to make this a hard failure"
            % normal["mean_vector_length"]
        )
    if normal["zero_vector_fraction"] > policy.max_zero_normal_fraction:
        errors.append(
            "zero-vector fraction %.6f exceeds %.6f; normal range/neutral "
            "encoding is contradictory"
            % (normal["zero_vector_fraction"], policy.max_zero_normal_fraction)
        )
    elif normal["zero_vector_fraction"] > 0.05:
        warnings.append(
            "zero-vector fraction %.6f is high; inspect neutral texels"
            % normal["zero_vector_fraction"]
        )
    return errors, warnings


def _normalize_in_place(rgba: bytearray, signature: Signature, layout: str) -> None:
    for pos in range(0, len(rgba), 4):
        r, g, b, a = rgba[pos:pos + 4]
        if layout == "rgba_xzyw":
            out = decode_normal_components(signature, r, a, g)
        else:
            out = decode_normal_components(signature, r, g, b)
        rgba[pos:pos + 4] = bytes(out)


def _failure_report(signature: Signature, source_name: str,
                    exc: Exception, limits: Limits) -> dict:
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": MANIFEST_KIND,
        "status": "fail",
        "signature": signature.as_dict(),
        "source": {"name": source_name},
        "output": None,
        "errors": [str(exc)],
        "warnings": [],
        "limits": limits.as_dict(),
        "runtime_integrated": False,
    }


def preprocess_normal(
    source: bytes | os.PathLike[str] | str,
    *,
    signature: str | Signature,
    output_path: Optional[os.PathLike[str] | str] = None,
    report_path: Optional[os.PathLike[str] | str] = None,
    source_name: Optional[str] = None,
    policy: Optional[NormalPolicy] = None,
    limits: Optional[Limits] = None,
) -> dict:
    """Validate and normalise a DDS normal map one mip at a time.

    Output is a linear RGBA8 DDS with R=X, G=Y, B=Z, A=255.  No runtime decode
    or ``xzyw`` swizzle remains.  With ``output_path=None`` this is validation
    only.  Contract failures raise :class:`ContractError`/:class:`DataError`
    before an output file is committed.
    """

    sig = signature if isinstance(signature, Signature) else normalize_signature(signature)
    policy = policy or NormalPolicy()
    policy.validate()
    limits = limits or Limits()
    limits.validate()
    if isinstance(source, (bytes, bytearray, memoryview)):
        source_path = None
    else:
        source_path = Path(source)
    ensure_distinct((source_path, output_path, report_path), "normal preprocessing")

    name = source_name or (str(source_path) if source_path else "<memory>")
    temporary = None
    try:
        dds_source, meter = load_dds_source(source, limits, "normal map")
        info = dds_source.info
        max_level_bytes = max(level.width * level.height * 4
                              for level in info.levels)
        output_bytes = rgba8_output_size(info)
        if output_bytes > limits.max_output_bytes:
            raise ContractError(
                "normal output would be %d bytes, exceeds max_output_bytes %d"
                % (output_bytes, limits.max_output_bytes)
            )
        estimated_peak = len(dds_source.data) + 2 * max_level_bytes + 64 * 1024
        if estimated_peak > limits.max_memory_bytes:
            raise ContractError(
                "normal preprocessing estimated peak %d bytes exceeds "
                "max_memory_bytes %d" % (estimated_peak, limits.max_memory_bytes)
            )

        width, height, top = decode_level_accounted(
            dds_source.data, info, 0, meter, "normal map"
        )
        channels = _channel_stats(top)
        layout, layout_evidence = _detect_layout(channels, policy)
        normal_stats = _normal_stats(top, sig, layout, width * height)
        errors, warnings = _validate_range(channels, normal_stats, policy, layout)
        if errors:
            raise ContractError("; ".join(errors))

        output_info = None
        if output_path is not None:
            destination = Path(output_path)
            destination.parent.mkdir(parents=True, exist_ok=True)
            fd, temporary = tempfile.mkstemp(
                prefix=".%s." % destination.name, suffix=".tmp",
                dir=str(destination.parent),
            )
            meter.acquire("write_buffer", 64 * 1024, "open normal output")
            with os.fdopen(fd, "wb") as handle:
                handle.write(rgba8_dds_header(width, height, info.mip_count))
                for level in range(info.mip_count):
                    if level == 0:
                        rgba = top
                    else:
                        meter.release("decoded_level")
                        _w, _h, rgba = decode_level_accounted(
                            dds_source.data, info, level, meter, "normal map"
                        )
                    normalized = bytearray(rgba)
                    meter.acquire("normalized_level", len(normalized),
                                  "normalize mip %d" % level)
                    meter.release("decoded_level")
                    _normalize_in_place(normalized, sig, layout)
                    stride = info.levels[level].width * 4
                    for row in range(0, len(normalized), stride):
                        handle.write(memoryview(normalized)[row:row + stride])
                    meter.release("normalized_level")
                    del normalized
                    del rgba
                    if level == 0:
                        top = b""
                handle.flush()
                os.fsync(handle.fileno())
            meter.release("write_buffer")
            actual_size = os.path.getsize(temporary)
            if actual_size != output_bytes:
                raise DataError(
                    "normal output size %d does not match predicted %d"
                    % (actual_size, output_bytes)
                )
            output_hash = sha256_file(temporary, meter=meter,
                                      label="output_hash_read")
            os.replace(temporary, destination)
            temporary = None
            output_info = {
                "file": destination.name,
                "bytes": actual_size,
                "sha256": output_hash,
                "width": width,
                "height": height,
                "mip_count": info.mip_count,
                "dds_format": "RGBA8",
                "kind": OUTPUT_KIND,
                "channel_order": "xyz",
                "alpha": 255,
                "linear": True,
                "already_decoded": True,
                "levels": [
                    {"level": index, "width": level.width,
                     "height": level.height, "bytes": level.width * level.height * 4}
                    for index, level in enumerate(info.levels)
                ],
            }
        else:
            meter.release("decoded_level")
            del top

        report = {
            "schema_version": SCHEMA_VERSION,
            "kind": MANIFEST_KIND,
            "status": "pass",
            "signature": sig.as_dict(),
            "source": dict(dds_source.describe(), name=name,
                           sha256=sha256_bytes(dds_source.data)),
            "decode": {
                "source_layout": layout,
                "retail_source_layout": "rgba_xzyw",
                "layout_conversion": layout == "rgba_xyzw",
                "selected_components": (
                    {"x": "r", "y": "a", "z": "g"}
                    if layout == "rgba_xzyw"
                    else {"x": "r", "y": "g", "z": "b"}
                ),
                "formula": sig.formula,
                "output_kind": OUTPUT_KIND,
                "quantization": "UNORM8 round-to-nearest, ties upward",
                "mip_policy": "transform every complete stored source mip; no generated mips",
            },
            "validation": {
                "errors": [],
                "warnings": warnings,
                "channel_order": layout_evidence,
                "source_channel_stats_top_mip": channels,
                "decoded_normal_stats_top_mip": normal_stats,
                "semantic_axis_limit": (
                    "statistics can detect layout/range contradictions but "
                    "cannot prove arbitrary internal channel swaps; source "
                    "provenance remains required"
                ),
            },
            "output": output_info,
            "memory": meter.as_dict(),
            "limits": limits.as_dict(),
            "runtime_integrated": False,
        }
        meter.release("source_dds")
        if report_path is not None:
            atomic_write_json(report_path, report)
        return report
    except (MenuPreprocessError, OSError) as exc:
        if temporary:
            try:
                os.unlink(temporary)
            except OSError:
                pass
        failure = _failure_report(sig, name, exc, limits)
        if report_path is not None:
            try:
                atomic_write_json(report_path, failure)
            except OSError:
                pass
        raise


__all__ = [
    "SCHEMA_VERSION", "MANIFEST_KIND", "OUTPUT_KIND", "NormalPolicy",
    "preprocess_normal",
]
