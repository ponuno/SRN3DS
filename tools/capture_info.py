#!/usr/bin/env python3
"""Text/JSON overview of d9_capture first-draw snapshots.

Reads every *firstdraw*.txt below a directory (recursively by default), parses
the key=value text files written by source/d3d9_capture.c and prints a compact
table or a JSON document.  Nothing is modified: captures are opened read-only
and the paired .bin file is only checked for existence.

Usage:
    python tools/capture_info.py -Dir E:\\menu_capture\\run_tesv_slice_8260
    python tools/capture_info.py E:\\menu_draw\\ --json
    python tools/capture_info.py -Dir E:\\menu_draw --no-recursive

The primitive/cull/blend names follow source/d3d9_core.h and the conventions
used by tools/capture_raster.py (D9_TRIANGLELIST=4, STRIP=5, FAN=6).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any

PATTERN = "*firstdraw*.txt"

PRIMITIVE_NAMES = {
    0: "POINTLIST",
    1: "LINELIST",
    2: "LINESTRIP",
    3: "TRIANGLELIST_LEGACY",
    4: "TRIANGLELIST",
    5: "TRIANGLESTRIP",
    6: "TRIANGLEFAN",
}
CULL_NAMES = {1: "NONE", 2: "CW", 3: "CCW"}
BLEND_NAMES = {
    0: "ZERO", 1: "ONE", 2: "SRCCOLOR", 3: "INVSRCCOLOR",
    4: "SRCALPHA", 5: "INVSRCALPHA", 6: "DESTALPHA", 7: "INVDESTALPHA",
    8: "DESTCOLOR", 9: "INVDESTCOLOR", 10: "SRCALPHASAT",
    11: "BOTHSRCALPHA", 12: "BOTHINVSRCALPHA", 13: "BLENDFACTOR",
    14: "INVBLENDFACTOR",
}
BLEND_OP_NAMES = {1: "ADD", 2: "SUB", 3: "REVSUB", 4: "MIN", 5: "MAX"}
CMP_NAMES = {
    1: "NEVER", 2: "LESS", 3: "EQUAL", 4: "LEQUAL", 5: "GREATER",
    6: "NOTEQUAL", 7: "GEQUAL", 8: "ALWAYS",
}
FORMAT_NAMES = {
    21: "A8R8G8B8", 22: "X8R8G8B8", 28: "A8",
    0x31545844: "DXT1", 0x33545844: "DXT3", 0x35545844: "DXT5",
    0x31495441: "ATI1", 0x32495441: "ATI2",
    0x55344342: "BC4U", 0x55354342: "BC5U", 0x37434244: "BC7",
}

# Text fields factually present in the v3 snapshot; missing fields stay None.
INT_FIELDS = (
    "version", "frame.index", "draw.index", "draw.type", "draw.start",
    "truncation", "draw.primitives", "draw.indexed", "draw.base_vertex", "draw.min_vertex",
    "draw.vertex_count", "draw.start_index", "viewport.x", "viewport.y",
    "viewport.width", "viewport.height", "viewport.minz", "viewport.maxz",
    "state.z_enable", "state.z_write", "state.z_func", "state.cull",
    "state.alpha_test", "state.alpha_func", "state.alpha_ref", "state.blend",
    "state.src_blend", "state.dst_blend", "state.blend_op",
    "state.color_write", "stream_count", "vertices.stride", "vertices.count",
    "vertices.captured", "vertices.bytes", "indices.size", "indices.count",
    "indices.captured", "texture_count", "clear.valid", "clear.flags",
    "clear.rect_count", "present.present", "present.frame_index", "present.swap_chain",
    "target.handle", "target.implicit", "target.width", "target.height",
    "target.format", "target.usage", "pixels.captured",
)


def parse_kv(text: str) -> dict[str, str]:
    """Parse 'key=value' lines, ignoring comments and blank lines."""
    result: dict[str, str] = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.strip()] = value.strip()
    return result


def to_int(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value, 0) if value.lower().startswith("0x") else int(value, 10)
    except ValueError:
        return None


def named(names: dict[int, str], value: int | None) -> str:
    if value is None:
        return "?"
    return names.get(value, str(value))


def format_name(value: int | None) -> str:
    if value is None:
        return "?"
    return FORMAT_NAMES.get(value, f"0x{value:08x}")


def state_labels(fields: dict[str, int | None]) -> dict[str, str]:
    """Short human-readable z/blend/cull/alpha labels for the table."""
    z = fields["state.z_enable"]
    z_label = "on" if z else "off"
    if z is None:
        z_label = "?"
    z_label += f"/w{fields['state.z_write']}/f{named(CMP_NAMES, fields['state.z_func'])}"

    blend = fields["state.blend"]
    blend_label = "off" if blend == 0 else ("on" if blend else "?")
    if blend:
        blend_label += " {}/{} {}".format(
            named(BLEND_NAMES, fields["state.src_blend"]),
            named(BLEND_NAMES, fields["state.dst_blend"]),
            named(BLEND_OP_NAMES, fields["state.blend_op"]),
        )

    cull_label = named(CULL_NAMES, fields["state.cull"])
    if fields["state.cull"] is not None:
        cull_label += f"({fields['state.cull']})"

    alpha = fields["state.alpha_test"]
    alpha_label = "on" if alpha else ("off" if alpha == 0 else "?")
    alpha_label += "/f{}/r{}".format(
        named(CMP_NAMES, fields["state.alpha_func"]), fields["state.alpha_ref"]
    )
    return {"z": z_label, "blend": blend_label, "cull": cull_label, "alpha": alpha_label}


def read_capture(path: Path, root: Path) -> dict[str, Any]:
    raw = parse_kv(path.read_text(encoding="utf-8", errors="replace"))
    fields = {key: to_int(raw.get(key)) for key in INT_FIELDS}
    fmt = raw.get("format", "")
    kind = "present" if fmt == "d9_capture_present" else ("draw" if fmt else "unknown")
    labels = state_labels(fields) if kind == "draw" else None

    viewport = None
    if fields["viewport.width"] is not None:
        viewport = {
            "x": fields["viewport.x"], "y": fields["viewport.y"],
            "width": fields["viewport.width"], "height": fields["viewport.height"],
            "minz": fields["viewport.minz"], "maxz": fields["viewport.maxz"],
        }

    textures = []
    texture_count = fields["texture_count"] or 0
    for index in range(texture_count):
        prefix = f"texture[{index}]"
        texture_format = to_int(raw.get(prefix + ".format"))
        textures.append({
            "stage": to_int(raw.get(prefix + ".stage")),
            "width": to_int(raw.get(prefix + ".width")),
            "height": to_int(raw.get(prefix + ".height")),
            "format": texture_format,
            "format_name": format_name(texture_format),
            "row_pitch": to_int(raw.get(prefix + ".row_pitch")),
            "pixels_declared": to_int(raw.get(prefix + ".pixels.declared")),
            "pixels_stored": to_int(raw.get(prefix + ".pixels.stored")),
            "pixels_truncated": to_int(raw.get(prefix + ".pixels.truncated")),
        })

    return {
        "file": path.relative_to(root).as_posix() if root.is_dir() else path.name,
        "format": fmt,
        "kind": kind,
        "version": fields["version"],
        "capture_full": bool(to_int(raw.get("capture.full"))),
        "truncation": fields["truncation"],
        "truncation_names": raw.get("truncation.names", "none"),
        "frame_index": fields["frame.index"],
        "draw_index": fields["draw.index"],
        "draw": {
            "type": fields["draw.type"],
            "type_name": PRIMITIVE_NAMES.get(fields["draw.type"], "UNKNOWN"),
            "primitives": fields["draw.primitives"],
            "indexed": bool(fields["draw.indexed"]) if fields["draw.indexed"] is not None else None,
            "start": fields["draw.start"],
            "base_vertex": fields["draw.base_vertex"],
            "min_vertex": fields["draw.min_vertex"],
            "vertex_count": fields["draw.vertex_count"],
            "start_index": fields["draw.start_index"],
        },
        "vertices": {
            "stride": fields["vertices.stride"],
            "count": fields["vertices.count"],
            "captured": fields["vertices.captured"],
        },
        "indices": {
            "size": fields["indices.size"],
            "count": fields["indices.count"],
            "captured": fields["indices.captured"],
        },
        "textures": textures,
        "viewport": viewport,
        "state": {key: fields[key] for key in INT_FIELDS if key.startswith("state.")},
        "state_labels": labels,
        "target": {
            "handle": raw.get("target.handle"),
            "width": fields["target.width"],
            "height": fields["target.height"],
            "format": fields["target.format"],
            "format_name": format_name(fields["target.format"]),
        },
        "present": {
            "frame_index": fields["present.frame_index"],
            "present": fields["present.present"],
            "flags": raw.get("present.flags"),
            "swap_chain": fields["present.swap_chain"],
        },
        "bin_sibling": path.with_suffix(".bin").is_file(),
    }


def summarize(directory: Path, files: list[dict[str, Any]], errors: list[str]) -> dict[str, Any]:
    draws = [f for f in files if f["kind"] == "draw"]
    presents = [f for f in files if f["kind"] == "present"]

    def total(records: list[dict[str, Any]], section: str, key: str) -> int:
        return sum(f[section][key] or 0 for f in records)

    return {
        "directory": str(directory),
        "files": len(files),
        "draws": len(draws),
        "presents": len(presents),
        "unknown": len(files) - len(draws) - len(presents),
        "primitives": total(draws, "draw", "primitives"),
        "vertices_captured": total(draws, "vertices", "captured"),
        "vertices_count": total(draws, "vertices", "count"),
        "vertex_count_ranges": total(draws, "draw", "vertex_count"),
        "indices_captured": total(draws, "indices", "captured"),
        "indices_count": total(draws, "indices", "count"),
        "truncated_draws": sum(1 for f in draws if f["truncation"]),
        "indexed_draws": sum(1 for f in draws if f["draw"]["indexed"]),
        "with_bin": sum(1 for f in files if f["bin_sibling"]),
        "errors": errors,
    }


def fmt(value: Any) -> str:
    return "-" if value is None else str(value)


def render_table(summary: dict[str, Any], files: list[dict[str, Any]]) -> str:
    headers = ("FILE", "DRAW", "PRIMS", "IDX", "VCOUNT/CAPT", "VIEWPORT",
               "Z", "BLEND", "CULL", "ALPHA", "TEX FORMATS", "TRUNC", "BIN")
    rows: list[tuple[str, ...]] = []
    for f in files:
        if f["kind"] == "present":
            rows.append((f["file"], "PRESENT", "-", "-", "-", "-", "-", "-", "-", "-",
                         "-", "-", "yes" if f["bin_sibling"] else "no"))
            continue
        d = f["draw"]
        vp = f["viewport"]
        vp_text = "-"
        if vp:
            vp_text = "{}x{}@{},{}".format(vp["width"], vp["height"], vp["x"], vp["y"])
        counts = "{}/{}".format(fmt(d["vertex_count"]), fmt(f["vertices"]["count"]))
        labels = f["state_labels"]
        texture_formats = ",".join(
            f"{item['stage']}:{item['format_name']}" for item in f["textures"]
        ) or "-"
        rows.append((
            f["file"],
            "{}({})".format(d["type_name"], fmt(d["type"])),
            fmt(d["primitives"]),
            "yes" if d["indexed"] else "no",
            counts,
            vp_text,
            labels["z"], labels["blend"], labels["cull"], labels["alpha"],
            texture_formats, f["truncation_names"],
            "yes" if f["bin_sibling"] else "no",
        ))

    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))
    widths[0] = min(widths[0], 72)  # keep long relative paths from flooding the line

    def line(cells: tuple[str, ...]) -> str:
        return "  ".join(cell[:widths[i]].ljust(widths[i]) for i, cell in enumerate(cells)).rstrip()

    out = [
        "Directory: {} (recursive={})".format(summary["directory"], summary["recursive"]),
        line(headers),
        line(tuple("-" * w for w in widths)),
    ]
    out.extend(line(row) for row in rows)
    out.append(
        "Summary: files={files} draws={draws} presents={presents} primitives={primitives} "
        "vertices(captured)={vertices_captured} vertex_count(ranges)={vertex_count_ranges} "
        "indices(captured)={indices_captured} indexed_draws={indexed_draws} "
        "truncated_draws={truncated_draws} with_bin={with_bin}".format(**summary)
    )
    if summary["errors"]:
        out.append("Errors: {}".format("; ".join(summary["errors"])))
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-Dir", "--dir", dest="directory", type=Path,
                    help="directory to scan (also accepted as a positional argument)")
    ap.add_argument("directory_pos", nargs="?", type=Path, help=argparse.SUPPRESS)
    ap.add_argument("--json", action="store_true", help="emit a JSON document instead of a table")
    ap.add_argument("--no-recursive", action="store_true", help="scan only the top-level directory")
    args = ap.parse_args()

    directory = args.directory or args.directory_pos
    if directory is None:
        ap.error("provide a directory with -Dir or as a positional argument")
    directory = directory.expanduser()
    if not directory.exists():
        ap.error(f"directory does not exist: {directory}")

    if directory.is_file():
        paths = [directory]
        root = directory.parent
    else:
        root = directory
        if args.no_recursive:
            paths = sorted(p for p in directory.glob(PATTERN) if p.is_file())
        else:
            paths = sorted(p for p in directory.rglob(PATTERN) if p.is_file())

    files: list[dict[str, Any]] = []
    errors: list[str] = []
    for path in paths:
        try:
            files.append(read_capture(path, root))
        except (OSError, ValueError) as exc:
            errors.append(f"{path.name}: {exc}")
            print(f"warning: skipping {path}: {exc}", file=sys.stderr)

    summary = summarize(directory, files, errors)
    summary["recursive"] = not args.no_recursive
    summary["pattern"] = PATTERN
    if args.json:
        print(json.dumps({"summary": summary, "files": files}, indent=2))
    else:
        print(render_table(summary, files))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
