#!/usr/bin/env python3
"""Diagnostic single-frame render of Interface/StartMenu.swf without TESV.

The tool extracts the movie (and, when present, ``interface/fonts_en.swf`` and
``interface/fontconfig.txt``) from a retail BSA into a temporary directory,
runs the host probe ``menu_swf_frame_probe`` over the existing bounded SWF
pipeline (summary -> display list -> tessellation -> batch -> A8 raster/atlas)
and writes one RGBA8 PNG.  This is a diagnostic render, **not** an engine
frame, and it makes no parity claim.

Usage:
    python tools/menu_swf_probe.py --out out/startmenu.png
    python tools/menu_swf_probe.py --swf Interface/StartMenu.swf --probe PATH
    python tools/menu_swf_probe.py --bsa E:/.../Skyrim - Interface.bsa --json
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from skyrim import bsa  # noqa: E402

DEFAULT_BSA = Path(
    r"E:\SteamLibrary\steamapps\common\Skyrim\Data\Skyrim - Interface.bsa")
MOVIE = "interface/startmenu.swf"
FONT_SWF = "interface/fonts_en.swf"
FONTCONFIG = "interface/fontconfig.txt"


# --------------------------------------------------------------------- PNG ---
def _chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def rgba_to_png(rgba: bytes, width: int, height: int) -> bytes:
    stride = width * 4
    if len(rgba) != stride * height:
        raise ValueError("RGBA buffer is %d bytes, expected %d" %
                         (len(rgba), stride * height))
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter type None
        rows += rgba[y * stride:(y + 1) * stride]
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) +
            _chunk(b"IDAT", zlib.compress(bytes(rows), 9)) +
            _chunk(b"IEND", b""))


# --------------------------------------------------------------- extraction ---
def extract_movie(archive_path: Path, workdir: Path) -> dict:
    archive = bsa.open_archive(str(archive_path))
    found = {}
    for key, target in ((MOVIE, "StartMenu.swf"),
                        (FONT_SWF, "fonts_en.swf"),
                        (FONTCONFIG, "fontconfig.txt")):
        try:
            payload = archive.read_file(key)
        except Exception:
            continue
        path = workdir / target
        path.write_bytes(payload)
        entry = archive.find(key)
        found[key] = {"path": str(path), "bytes": len(payload),
                      "archive_path": getattr(entry, "path", key)}
    if MOVIE not in found:
        raise SystemExit("%s does not contain %s" % (archive_path, MOVIE))
    return found


# ------------------------------------------------------------------- probe ---
def find_probe(explicit: str | None) -> Path | None:
    if explicit:
        path = Path(explicit)
        return path if path.exists() else None
    env = os.environ.get("MENU_SWF_PROBE")
    if env and Path(env).exists():
        return Path(env)
    for base in (ROOT / "build_host", ROOT / ".venv"):
        if not base.exists():
            continue
        hits = sorted(base.glob("run_menu_swf_frame_probe_*/menu_swf_frame_probe.exe"),
                      key=lambda p: p.stat().st_mtime, reverse=True)
        if hits:
            return hits[0]
    for name in ("menu_swf_frame_probe.exe", "menu_swf_frame_probe"):
        cand = ROOT / "build_host" / name
        if cand.exists():
            return cand
    return None


def _exec_path(path: Path) -> str:
    # This repository is often driven by MSYS2 Python on Windows; subprocess
    # needs forward slashes for a drive-letter path there.
    return str(path).replace("\\", "/")


def run_probe(probe: Path, swf: Path, raw: Path, report: Path,
              fonts: Path | None, fontconfig: Path | None,
              width: int = 0, height: int = 0) -> str:
    cmd = [_exec_path(probe), "--swf", _exec_path(swf),
           "--out-raw", _exec_path(raw), "--out-json", _exec_path(report)]
    if fonts:
        cmd += ["--font-source", _exec_path(fonts)]
    if fontconfig:
        cmd += ["--fontconfig", _exec_path(fontconfig)]
    if width:
        cmd += ["--width", str(width)]
    if height:
        cmd += ["--height", str(height)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit("probe failed (%d): %s%s" %
                         (done.returncode, done.stdout, done.stderr))
    return (done.stdout + done.stderr).strip()


# ----------------------------------------------------------------- summary ---
def format_summary(stats: dict, extraction: dict | None) -> str:
    lines = ["StartMenu.swf diagnostic frame (NOT an engine frame)"]
    if extraction:
        for key, info in extraction.items():
            lines.append("  source: %s -> %s (%d bytes)" %
                         (info["archive_path"], Path(info["path"]).name,
                          info["bytes"]))
    lines.append("  swf: %d bytes, %d tags (%d top-level), ShowFrame=%d" %
                 (stats["swf_bytes"], stats["tags"], stats["top_level_tags"],
                  stats["show_frame"]))
    tw, th = stats["scene_twips"]
    lines.append("  scene: %dx%d twips = %dx%d px; rendered %dx%d" %
                 (tw, th, tw // 20, th // 20, stats["rgba"]["width"],
                  stats["rgba"]["height"]))
    lines.append("  definitions: shape=%d font2=%d font3=%d sprite=%d "
                 "lossless=%d jpeg=%d" %
                 (stats["define_shape"], stats["define_font2"],
                  stats["define_font3"], stats["define_sprite"],
                  stats["define_bits_lossless"], stats["define_bits_jpeg"]))
    lines.append("  display list: records=%d sprites=%d masks=%d shapes=%d "
                 "texts=%d unsupported=%d" %
                 (stats["records"], stats["sprite_records"],
                  stats["mask_records"], stats["shape_records"],
                  stats["text_records"], stats["unsupported_records"]))
    fills = stats["fill_styles"]
    lines.append("  fills: solid=%d gradient=%d focal=%d bitmap=%d; "
                 "triangle records=%d, triangles=%d, strokes skipped=%d" %
                 (fills["solid"], fills["gradient"], fills["focal"],
                  fills["bitmap"], stats["shape_records"],
                  stats["shape_triangles"], stats["stroke_segments"]))
    lines.append("  text: definitions=%d nonempty=%d resolved=%d refused=%d "
                 "glyphs=%d missing_glyphs=%d quads=%d" %
                 (stats["text_definitions"], stats["text_nonempty"],
                  stats["text_resolved"], stats["text_refused"],
                  stats["text_glyphs"], stats["text_missing_glyphs"],
                  stats["text_quads"]))
    atlas = stats["atlas"]
    lines.append("  atlas: %dx%d, placed=%d, used_height=%d, occupied=%d px" %
                 (atlas["size"], atlas["size"], atlas["placed_glyphs"],
                  atlas["used_height"], atlas["occupied_pixels"]))
    rgba = stats["rgba"]
    lines.append("  frame: opaque_pixels=%d (%.2f%%), bbox=%s, triangles=%d" %
                 (rgba["opaque_pixels"],
                  100.0 * rgba["opaque_pixels"] /
                  max(1, rgba["width"] * rgba["height"]),
                  rgba["bbox"], stats["render_triangles"]))
    if stats["missing"]:
        lines.append("  gaps (named, not substituted): %s" % stats["missing"])
    return "\n".join(lines)


# -------------------------------------------------------------------- main ---
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bsa", default=str(DEFAULT_BSA),
                    help="Skyrim - Interface.bsa (default: %(default)s)")
    ap.add_argument("--swf", help="render this SWF directly (skips BSA)")
    ap.add_argument("--font-source", help="fonts_en.swf for imported fonts")
    ap.add_argument("--fontconfig", help="fontconfig.txt alias map")
    ap.add_argument("--probe", help="menu_swf_frame_probe binary")
    ap.add_argument("--out", default="startmenu_diagnostic.png",
                    help="PNG output path")
    ap.add_argument("--json", action="store_true", help="print the raw summary")
    ap.add_argument("--width", type=int, default=0)
    ap.add_argument("--height", type=int, default=0)
    ap.add_argument("--keep-raw", action="store_true")
    args = ap.parse_args(argv)

    probe = find_probe(args.probe)
    if not probe:
        raise SystemExit("menu_swf_frame_probe binary not found; build it with "
                         "tools/run_host_tests.ps1 -Only menu_swf_frame_probe")
    out_png = Path(args.out)
    out_png.parent.mkdir(parents=True, exist_ok=True)

    workdir = Path(tempfile.mkdtemp(prefix="menu_swf_probe_"))
    extraction = None
    try:
        if args.swf:
            swf = Path(args.swf)
            fonts = Path(args.font_source) if args.font_source else None
            fontconfig = Path(args.fontconfig) if args.fontconfig else None
        else:
            extraction = extract_movie(Path(args.bsa), workdir)
            swf = Path(extraction[MOVIE]["path"])
            fonts = (Path(extraction[FONT_SWF]["path"])
                     if FONT_SWF in extraction else None)
            fontconfig = (Path(extraction[FONTCONFIG]["path"])
                          if FONTCONFIG in extraction else None)
        raw = workdir / "frame.rgba"
        report = workdir / "frame.json"
        log = run_probe(probe, swf, raw, report, fonts, fontconfig,
                        args.width, args.height)
        stats = json.loads(report.read_text(encoding="utf-8"))
        if not stats.get("ok"):
            raise SystemExit("probe reported: %s" % stats.get("reason"))
        rgba = raw.read_bytes()
        png = rgba_to_png(rgba, stats["rgba"]["width"],
                          stats["rgba"]["height"])
        out_png.write_bytes(png)
        if args.json:
            print(json.dumps(stats, indent=2))
        print(format_summary(stats, extraction))
        print("  png: %s (%d bytes) sha256=%s" %
              (out_png, len(png), __import__("hashlib").sha256(png).hexdigest()))
        if log:
            print("  probe: %s" % log.replace("\n", " | "))
        if not args.keep_raw:
            raw.unlink(missing_ok=True)
        return 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
