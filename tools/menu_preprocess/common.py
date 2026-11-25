"""Shared bounded I/O, validation and reporting helpers."""

from __future__ import annotations

import hashlib
import json
import math
import os
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, Mapping, Optional, Sequence, Tuple


class MenuPreprocessError(Exception):
    """Base class for deterministic host validation failures."""


class DataError(MenuPreprocessError):
    """Malformed or unsupported asset data."""


class ContractError(MenuPreprocessError):
    """Missing, unsupported or contradictory contract metadata."""


class BudgetError(MenuPreprocessError):
    """A documented host memory/input/output bound would be exceeded."""


@dataclass(frozen=True)
class Limits:
    """Hard host-side bounds.

    The memory meter counts large buffers owned by these tools.  It is a
    conservative lower bound on process RSS: interpreter overhead, allocator
    fragmentation and the standard library's small temporary objects are not
    included.
    """

    max_input_bytes: int = 16 * 1024 * 1024
    max_memory_bytes: int = 16 * 1024 * 1024
    max_json_bytes: int = 1024 * 1024
    max_output_bytes: int = 64 * 1024 * 1024
    max_pixels: int = 4 * 1024 * 1024
    max_vertex_records: int = 1_000_000
    max_registry_bytes: int = 2 * 1024 * 1024

    def validate(self) -> None:
        for name in (
            "max_input_bytes", "max_memory_bytes", "max_json_bytes",
            "max_output_bytes", "max_pixels", "max_vertex_records",
            "max_registry_bytes",
        ):
            value = getattr(self, name)
            if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
                raise ContractError("limits.%s must be a positive integer" % name)

    def as_dict(self) -> dict:
        self.validate()
        return {
            "max_input_bytes": self.max_input_bytes,
            "max_memory_bytes": self.max_memory_bytes,
            "max_json_bytes": self.max_json_bytes,
            "max_output_bytes": self.max_output_bytes,
            "max_pixels": self.max_pixels,
            "max_vertex_records": self.max_vertex_records,
            "max_registry_bytes": self.max_registry_bytes,
            "memory_scope": (
                "large tool-owned buffers; lower bound on process RSS "
                "(interpreter/allocator overhead excluded)"
            ),
        }


@dataclass
class MemoryMeter:
    """Account for live large host buffers and enforce a hard cap."""

    limit: int
    live: Dict[str, int] = field(default_factory=dict)
    peak: int = 0
    peak_live: Dict[str, int] = field(default_factory=dict)
    peak_event: str = ""
    total_allocated: int = 0

    def _check(self, event: str) -> None:
        total = sum(self.live.values())
        if total > self.peak:
            self.peak = total
            self.peak_live = dict(self.live)
            self.peak_event = event
        if total > self.limit:
            raise BudgetError(
                "%s: live host bytes %d exceed max_memory_bytes %d (%s)"
                % (event, total, self.limit,
                   ", ".join("%s=%d" % item for item in sorted(self.live.items())))
            )

    def acquire(self, tag: str, size: int, event: str) -> None:
        if size < 0:
            raise BudgetError("negative allocation for " + tag)
        self.live[tag] = self.live.get(tag, 0) + size
        self.total_allocated += size
        self._check(event)

    def release(self, tag: str) -> None:
        self.live.pop(tag, None)

    def as_dict(self) -> dict:
        return {
            "peak_bytes": self.peak,
            "peak_live": dict(sorted(self.peak_live.items())),
            "peak_event": self.peak_event,
            "total_allocated_bytes": self.total_allocated,
            "current_bytes": sum(self.live.values()),
            "limit_bytes": self.limit,
        }


def read_bounded(path: os.PathLike[str] | str, limit: int, label: str) -> bytes:
    """Read at most ``limit`` bytes, checking size before and after the read."""

    p = Path(path)
    try:
        size = p.stat().st_size
    except OSError as exc:
        raise DataError("%s: cannot stat %s: %s" % (label, p, exc)) from exc
    if size > limit:
        raise BudgetError(
            "%s: %s is %d bytes, exceeds %d-byte input limit"
            % (label, p, size, limit)
        )
    try:
        with p.open("rb") as handle:
            data = handle.read(limit + 1)
    except OSError as exc:
        raise DataError("%s: cannot read %s: %s" % (label, p, exc)) from exc
    if len(data) > limit:
        raise BudgetError(
            "%s: %s grew past the %d-byte input limit while reading"
            % (label, p, limit)
        )
    return data


def _no_duplicate_keys(pairs: Iterable[Tuple[str, Any]]) -> dict:
    out: Dict[str, Any] = {}
    for key, value in pairs:
        if key in out:
            raise ContractError("duplicate JSON key %r" % key)
        out[key] = value
    return out


def _reject_json_constant(label: str, token: str) -> None:
    raise ContractError("%s contains non-finite JSON number %s" % (label, token))


def parse_json(data: bytes, label: str, limit: int) -> dict:
    if len(data) > limit:
        raise BudgetError(
            "%s is %d bytes, exceeds the %d-byte JSON limit" % (label, len(data), limit)
        )
    try:
        value = json.loads(
            data.decode("utf-8"),
            object_pairs_hook=_no_duplicate_keys,
            parse_constant=lambda token: _reject_json_constant(label, token),
        )
    except ContractError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise DataError("%s: invalid JSON: %s" % (label, exc)) from exc
    if not isinstance(value, dict):
        raise ContractError("%s must contain a JSON object" % label)
    return value


def load_json(path: os.PathLike[str] | str, limits: Limits, label: str) -> dict:
    limits.validate()
    return parse_json(read_bounded(path, limits.max_json_bytes, label), label,
                      limits.max_json_bytes)


def require_keys(value: Mapping[str, Any], required: Sequence[str],
                 where: str, *, optional: Sequence[str] = ()) -> None:
    if not isinstance(value, Mapping):
        raise ContractError("%s must be an object" % where)
    required_set = set(required)
    allowed = required_set | set(optional)
    missing = sorted(required_set - set(value))
    extra = sorted(set(value) - allowed)
    if missing:
        raise ContractError("%s is missing %s" % (where, ", ".join(missing)))
    if extra:
        raise ContractError("%s has unsupported/contradictory keys: %s"
                            % (where, ", ".join(extra)))


def require_exact(value: Any, expected: Any, where: str) -> None:
    if type(value) is not type(expected) or value != expected:
        raise ContractError("%s must be %r, got %r" % (where, expected, value))


def require_int(value: Any, minimum: int, maximum: int, where: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ContractError("%s must be an integer" % where)
    if value < minimum or value > maximum:
        raise ContractError("%s must be in [%d, %d]" % (where, minimum, maximum))
    return value


def number(value: Any, where: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ContractError("%s must be a finite number" % where)
    result = float(value)
    if not math.isfinite(result):
        raise ContractError("%s must be a finite number" % where)
    return result


def vector(value: Any, count: int, where: str) -> Tuple[float, ...]:
    if not isinstance(value, (list, tuple)) or len(value) != count:
        raise ContractError("%s must be an array of %d numbers" % (where, count))
    return tuple(number(item, "%s[%d]" % (where, i)) for i, item in enumerate(value))


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: os.PathLike[str] | str, *, meter: Optional[MemoryMeter] = None,
                label: str = "hash_read") -> str:
    digest = hashlib.sha256()
    chunk_size = 64 * 1024
    if meter:
        meter.acquire(label, chunk_size, "sha256")
    try:
        with Path(path).open("rb") as handle:
            while True:
                chunk = handle.read(chunk_size)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError as exc:
        raise DataError("cannot hash %s: %s" % (path, exc)) from exc
    finally:
        if meter:
            meter.release(label)
    return digest.hexdigest()


def ensure_distinct(paths: Iterable[os.PathLike[str] | str], label: str) -> None:
    resolved: Dict[str, str] = {}
    for path in paths:
        if path is None:
            continue
        key = os.path.normcase(str(Path(path).resolve()))
        if key in resolved:
            raise ContractError(
                "%s paths must be distinct (%s and %s)"
                % (label, resolved[key], path)
            )
        resolved[key] = str(path)


def json_text(value: Any) -> str:
    try:
        return json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n"
    except (TypeError, ValueError) as exc:
        raise DataError("cannot serialize report: %s" % exc) from exc


def atomic_write_bytes(path: os.PathLike[str] | str, data: bytes) -> int:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".%s." % p.name, suffix=".tmp",
                                     dir=str(p.parent))
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, p)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise
    return len(data)


def atomic_write_json(path: os.PathLike[str] | str, value: dict) -> int:
    return atomic_write_bytes(path, json_text(value).encode("utf-8"))


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def load_registry(limits: Limits) -> dict:
    path = repository_root() / "tools" / "pipelines" / "registry.json"
    registry = parse_json(
        read_bounded(path, limits.max_registry_bytes, "pipeline registry"),
        "pipeline registry", limits.max_registry_bytes,
    )
    require_exact(registry.get("schema_version"), 1, "registry.schema_version")
    pipelines = registry.get("pipelines")
    if not isinstance(pipelines, list):
        raise DataError("registry.pipelines must be an array")
    return registry


def registry_pipeline(registry: Mapping[str, Any], pipeline_id: str) -> dict:
    for row in registry.get("pipelines", []):
        if isinstance(row, dict) and row.get("id") == pipeline_id:
            return row
    raise DataError("pipeline registry has no %s descriptor" % pipeline_id)


def display_path(path: os.PathLike[str] | str) -> str:
    """Use a portable slash-separated spelling in derived reports."""

    return str(path).replace("\\", "/")


__all__ = [
    "MenuPreprocessError", "DataError", "ContractError", "BudgetError",
    "Limits", "MemoryMeter", "read_bounded", "parse_json", "load_json",
    "require_keys", "require_exact", "require_int", "number", "vector",
    "sha256_bytes", "sha256_file", "ensure_distinct", "json_text",
    "atomic_write_bytes", "atomic_write_json", "repository_root",
    "load_registry", "registry_pipeline", "display_path",
]
