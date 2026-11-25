#!/usr/bin/env python3
"""CLI for the bounded-memory DDS/BC -> ETC1/ETC1A4 pipeline.

Examples (PowerShell, from the repository root):

    $env:PYTHONPATH='tools'
    .venv\\Scripts\\python.exe tools\\texture_pipeline.py project `
        --inventory E:\\Skyrim_N3DS_build\\assets\\textures\\textures.json `
        --out E:\\Skyrim_N3DS_build\\assets\\textures\\etc1_pipeline.json

    .venv\\Scripts\\python.exe tools\\texture_pipeline.py sample `
        --out E:\\Skyrim_N3DS_build\\assets\\textures\\etc1_sample

Nothing is written into the repository; derived output belongs under
``E:\\Skyrim_N3DS_build``.  The sample command reads the four menu textures
named in docs/TEXTURES.md from the read-only retail archive.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from skyrim import bsa, etc1  # noqa: E402

DEFAULT_DATA = r"E:\SteamLibrary\steamapps\common\Skyrim\Data"
DEFAULT_INVENTORY = r"E:\Skyrim_N3DS_build\assets\textures\textures.json"
DEFAULT_OUT = r"E:\Skyrim_N3DS_build\assets\textures\etc1_sample"


def _policy(args) -> etc1.TexturePolicy:
    return etc1.TexturePolicy(
        max_dim=args.max_dim,
        min_mip_dim=args.min_mip_dim,
        alpha_mode=args.alpha,
        mip_mode=args.mip,
        max_memory_bytes=args.max_memory,
        max_output_bytes=args.max_output,
        quality=args.quality,
    )


def _write_json(path: str, value) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2)
        handle.write("\n")


def cmd_project(args) -> int:
    rows = json.load(open(args.inventory, encoding="utf-8"))
    report = etc1.project_inventory(rows, max_dim=args.max_dim)
    report["inventory"] = args.inventory
    _write_json(args.out, report)
    print("textures: %d" % report["textures"])
    print("downscaled to <= %d: %d" % (report["max_dim"],
                                       report["downscaled_to_max_dim"]))
    print("ETC1 (alpha dropped): %.2f MiB" %
          (report["etc1_alpha_dropped_bytes"] / 1048576.0))
    print("mixed auto (ETC1/ETC1A4): %.2f MiB" %
          (report["mixed_auto_bytes"] / 1048576.0))
    print("ETC1A4 all: %.2f MiB" % (report["etc1a4_all_bytes"] / 1048576.0))
    print("wrote %s" % args.out)
    return 0


def _convert_one(archive, name: str, out_dir: str, policy, tex3ds=None) -> dict:
    data = archive.read_file(name)
    result = etc1.convert_texture(data, name, out_dir, policy=policy,
                                  tex3ds=tex3ds)
    out = result.as_dict()
    out["source_bytes_read"] = len(data)
    return out


def cmd_convert(args) -> int:
    archive = bsa.BsaArchive(os.path.join(args.data,
                                          "Skyrim - Textures.bsa"))
    result = _convert_one(archive, args.path, args.out, _policy(args),
                          args.tex3ds)
    _write_json(args.report or os.path.join(args.out, "conversion.json"),
                result)
    print("%s: %s %dx%d -> %s, %d B, visible PSNR %s, peak %d B" %
          (args.path, result["source_format"], result["width"],
           result["height"], result["output_format"], result["chosen_bytes"],
           result["quality"].get("visible", {}).get("psnr_db"),
           result["memory"]["peak_bytes"]))
    return 0


def cmd_sample(args) -> int:
    archive = bsa.BsaArchive(os.path.join(args.data,
                                          "Skyrim - Textures.bsa"))
    policy = _policy(args)
    os.makedirs(args.out, exist_ok=True)
    results = []
    for name in etc1.MENU_TEXTURES:
        result = _convert_one(archive, name, args.out, policy, args.tex3ds)
        results.append(result)
        print("%-58s %-5s %4dx%-4d -> %-6s %8d B  visible PSNR %-7s peak %8d" %
              (name.split("\\")[-1], result["source_format"],
               result["width"], result["height"], result["output_format"],
               result["chosen_bytes"],
               str(result["quality"].get("visible", {}).get("psnr_db")),
               result["memory"]["peak_bytes"]))
    report = {
        "policy": policy.__dict__,
        "textures": results,
        "peak_simultaneous_bytes": max(
            (r["memory"]["peak_bytes"] for r in results), default=0),
        "note": ("Peak is the host pipeline's live source+RGBA+PNG+raw bytes; "
                 "the tex3ds process heap is not included.  Quality is RGB "
                 "PSNR over pixels whose source alpha is >= 8; alpha is 4-bit "
                 "in ETC1A4 and is reported analytically, not as BC3-equivalent."),
    }
    out = os.path.join(args.out, "conversion_sample.json")
    _write_json(out, report)
    print("peak simultaneous host bytes: %d" %
          report["peak_simultaneous_bytes"])
    print("wrote %s" % out)
    return 0


def cmd_plan(args) -> int:
    data = open(args.dds, "rb").read()
    plan = etc1.plan_texture(data, args.dds, _policy(args))
    print(json.dumps(plan.as_dict(), indent=2))
    if args.out:
        _write_json(args.out, plan.as_dict())
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--max-dim", type=int, default=1024)
    common.add_argument("--min-mip-dim", type=int, default=8)
    common.add_argument("--alpha", choices=("auto", "force", "drop"),
                        default="auto")
    common.add_argument("--mip", choices=("source", "generate"),
                        default="source")
    common.add_argument("--max-memory", type=int, default=16 * 1024 * 1024)
    common.add_argument("--max-output", type=int, default=0)
    common.add_argument("--quality", choices=("low", "medium", "high"),
                        default="high")
    common.add_argument("--tex3ds", default=None)

    p = sub.add_parser("project", parents=[common],
                       help="project the header-derived inventory")
    p.add_argument("--inventory", default=DEFAULT_INVENTORY)
    p.add_argument("--out", default=r"E:\Skyrim_N3DS_build\assets\textures\etc1_pipeline.json")
    p.set_defaults(func=cmd_project)

    p = sub.add_parser("convert", parents=[common],
                       help="convert one DDS path from Textures.bsa")
    p.add_argument("--data", default=DEFAULT_DATA)
    p.add_argument("--path", required=True)
    p.add_argument("--out", default=DEFAULT_OUT)
    p.add_argument("--report", default="")
    p.set_defaults(func=cmd_convert)

    p = sub.add_parser("sample", parents=[common],
                       help="convert the four menu textures")
    p.add_argument("--data", default=DEFAULT_DATA)
    p.add_argument("--out", default=DEFAULT_OUT)
    p.set_defaults(func=cmd_sample)

    p = sub.add_parser("plan", parents=[common],
                       help="print the plan for a local .dds file")
    p.add_argument("dds")
    p.add_argument("--out", default="")
    p.set_defaults(func=cmd_plan)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (etc1.TextureError, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
