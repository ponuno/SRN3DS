#!/usr/bin/env python3
"""Offline FXP -> native PICA200 pipeline compiler (variant B, stages B1/B2).

Commands::

    python tools\\fxp_compile.py compile  --data F:\\Skyrim\\Data ^
        --package shadersfx\\imagespace002.fxp --offset 4534
    python tools\\fxp_compile.py manifest --data F:\\Skyrim\\Data ^
        --out docs\\shader_plans.json
    python tools\\fxp_compile.py emit-c   --manifest docs\\shader_plans.json ^
        --out source\\d3d9_shader_plan_table.inc

The manifest maps a bytecode SHA-256 to a native descriptor.  No bytecode is
written.  ``emit-c`` is deterministic: the checked-in include is regenerated
by the host test and compared byte for byte.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Dict, List, Optional, Sequence

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import compile as C  # noqa: E402
from shaders import packages  # noqa: E402
from skyrim import bsa  # noqa: E402
from skyrim.shaders import parse_package  # noqa: E402

DEFAULT_DATA = [r"F:\Skyrim\Data",
                r"E:\SteamLibrary\steamapps\common\Skyrim\Data"]
ARCHIVE = "Skyrim - Shaders.bsa"
LOGO_HASH = "8725393bf3ed950789e50501893ab141efdfd0377309271f6ef7493239be5ff9"
COPY_HASH = "6022044ef19e4b1c241482207acca5a6be484081e629b6d72cdefdd9568a45de"


def find_data(explicit: str = "") -> str:
    if explicit:
        return explicit
    for candidate in DEFAULT_DATA:
        if os.path.exists(os.path.join(candidate, ARCHIVE)):
            return candidate
    raise SystemExit("Shaders.bsa not found; pass --data")


def _load_packages(data_dir: str, include_update: bool = False):
    archives = [ARCHIVE] + (["Update.bsa"] if include_update else [])
    out = []
    for archive_name in archives:
        path = os.path.join(data_dir, archive_name)
        if not os.path.exists(path):
            continue
        archive = bsa.open_archive(path)
        try:
            for entry in sorted(archive.list_files(), key=lambda e: e.path.lower()):
                if not entry.path.lower().endswith(".fxp"):
                    continue
                data = archive.read_file(entry.path)
                out.append(parse_package(entry.path, data))
        finally:
            archive.close()
    return packages.effective_packages(out)


def compile_chunk(package_path: str, offset: int, data_dir: str = "",
                  include_update: bool = False) -> dict:
    for package in _load_packages(find_data(data_dir), include_update):
        if package.path.lower() != package_path.lower():
            continue
        for chunk in package.chunks:
            if chunk.offset != offset:
                continue
            ir = C.build_ir(chunk.code)
            if chunk.kind == "ps":
                plan, cls, why = C.plan_ps(ir)
                if plan is None and cls == "tev_prebake":
                    plan = C.menu_light_plan(ir)
                    if plan is not None:
                        cls = "tev_prebake"
            else:
                plan, cls, why = C.plan_vs(ir)
            return {"package": package.path, "key": "0x%08x" % chunk.meta,
                    "offset": chunk.offset, "bytes": chunk.size,
                    "class": cls, "refusal": why,
                    "plan": plan, "ir": ir}
        raise SystemExit("offset %d not found in %s" % (offset, package_path))
    raise SystemExit("package not found: %s" % package_path)


def _entries(packages_list) -> List[dict]:
    out = []
    for package in packages_list:
        for chunk in package.chunks:
            out.append((package, chunk))
    return out


def _candidate_ps(packages_list) -> Optional[dict]:
    best = None
    for package, chunk in _entries(packages_list):
        if chunk.kind != "ps":
            continue
        ir = C.build_ir(chunk.code)
        plan, cls, why = C.plan_ps(ir)
        if plan is None or plan["stages"] < 2 or plan["family"] != "generic":
            continue
        key = (plan["stages"], plan["instructions"], ir.sha256)
        if best is None or key < best[0]:
            best = (key, package, chunk, plan)
    if best is None:
        return None
    _key, package, chunk, plan = best
    plan = dict(plan)
    plan["id"] = "game.ps.%s" % plan["sha256"][:8]
    plan["note"] = "offline-compiled generic TEV plan; not certified for draw"
    plan["draw_enabled"] = 0
    plan["source_package"] = package.path
    plan["source_key"] = "0x%08x" % chunk.meta
    return plan


def _candidate_vs(packages_list) -> Optional[dict]:
    best = None
    for package, chunk in _entries(packages_list):
        if chunk.kind != "vs":
            continue
        ir = C.build_ir(chunk.code)
        plan, cls, why = C.plan_vs(ir)
        if plan is None:
            continue
        inputs = {(i["usage"], i["usage_index"]) for i in plan["inputs"]}
        if (0, 0) not in inputs or not any(u == 5 for u, _ in inputs):
            continue
        key = (plan["instructions"], plan["sha256"])
        if best is None or key < best[0]:
            best = (key, package, chunk, plan)
    if best is None:
        return None
    _key, package, chunk, plan = best
    plan = dict(plan)
    plan["id"] = "game.vs.%s" % plan["sha256"][:8]
    plan["note"] = "PICA VS descriptor; native codegen pending, draw disabled"
    plan["draw_enabled"] = 0
    plan["source_package"] = package.path
    plan["source_key"] = "0x%08x" % chunk.meta
    return plan


def build_manifest(data_dir: str = "", include_update: bool = False) -> dict:
    pkgs = _load_packages(find_data(data_dir), include_update)
    entries: List[dict] = []

    logo = None
    copy = None
    for package, chunk in _entries(pkgs):
        ir = C.build_ir(chunk.code)
        if ir.sha256 == LOGO_HASH and chunk.kind == "ps":
            plan = C.menu_light_plan(ir)
            if plan is None:
                raise SystemExit("logo bytes do not reproduce the certified plan")
            plan = dict(plan)
            plan["id"] = "menu.logo.ps"
            plan["note"] = ("five-stage menu-light plan reproduced offline; "
                            "draw needs normal/tangent prebake and c8 ambient")
            plan["draw_enabled"] = 0
            plan["source_package"] = package.path
            plan["source_key"] = "0x%08x" % chunk.meta
            logo = plan
        elif ir.sha256 == COPY_HASH and chunk.kind == "ps":
            plan, cls, why = C.plan_ps(ir)
            if plan is None:
                raise SystemExit("copy bytes do not compile: %s" % why)
            plan = dict(plan)
            plan["id"] = "menu.composite.ps"
            plan["note"] = "certified one-stage copy; same plan as the runtime selector"
            plan["draw_enabled"] = 1
            plan["source_package"] = package.path
            plan["source_key"] = "0x%08x" % chunk.meta
            copy = plan
    if logo is None:
        raise SystemExit("certified logo hash not found in the archive")
    if copy is None:
        raise SystemExit("certified copy hash not found in the archive")
    entries.append(logo)
    entries.append(copy)

    game_ps = _candidate_ps(pkgs)
    if game_ps is not None:
        entries.append(game_ps)
    game_vs = _candidate_vs(pkgs)
    if game_vs is not None:
        entries.append(game_vs)

    return {
        "schema": "d9-shader-plan-v1",
        "generated_for": find_data(data_dir),
        "archive": ARCHIVE,
        "note": ("Native pipeline descriptors only. Hash -> plan; no shader "
                 "bytes are stored. draw_enabled is 1 only for a plan whose "
                 "runtime equivalence is separately certified."),
        "entries": entries,
    }


def _strip_ir(value):
    if isinstance(value, dict):
        return {k: _strip_ir(v) for k, v in value.items() if k != "ir"}
    if isinstance(value, list):
        return [_strip_ir(v) for v in value]
    return value


def write_manifest(path: str, manifest: dict) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(_strip_ir(manifest), handle, indent=2, sort_keys=True)
        handle.write("\n")


def read_manifest(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_compile = sub.add_parser("compile")
    p_compile.add_argument("--data", default="")
    p_compile.add_argument("--package", required=True)
    p_compile.add_argument("--offset", type=int, required=True)
    p_compile.add_argument("--update", action="store_true")

    p_manifest = sub.add_parser("manifest")
    p_manifest.add_argument("--data", default="")
    p_manifest.add_argument("--out", default=os.path.join(REPO, "docs", "shader_plans.json"))
    p_manifest.add_argument("--update", action="store_true")

    p_c = sub.add_parser("emit-c")
    p_c.add_argument("--manifest", default=os.path.join(REPO, "docs", "shader_plans.json"))
    p_c.add_argument("--out", default=os.path.join(REPO, "source",
                                                   "d3d9_shader_plan_table.inc"))

    args = parser.parse_args(argv)
    if args.cmd == "compile":
        result = compile_chunk(args.package, args.offset, args.data, args.update)
        result.pop("ir", None)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    if args.cmd == "manifest":
        manifest = build_manifest(args.data, args.update)
        write_manifest(args.out, manifest)
        print("wrote %s (%d entries)" % (args.out, len(manifest["entries"])))
        for entry in manifest["entries"]:
            print("  %-24s %s  %s  stages=%s draw=%d" % (
                entry["id"], entry["stage"], entry["sha256"][:16],
                entry.get("stages"), entry.get("draw_enabled", 0)))
        return 0
    manifest = read_manifest(args.manifest)
    text = C.emit_c_table(manifest["entries"])
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
