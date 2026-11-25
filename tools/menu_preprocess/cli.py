"""Command-line entry point for offline menu shader preprocessing."""

from __future__ import annotations

import argparse
import sys
from typing import Optional, Sequence

from .common import Limits, MenuPreprocessError
from .glyph import validate_glyph
from .material import validate_material
from .normal import NormalPolicy, preprocess_normal
from .sample import DEFAULT_DATA, DEFAULT_OUT, run_sample


def _limits(args: argparse.Namespace) -> Limits:
    return Limits(
        max_input_bytes=args.max_input_bytes,
        max_memory_bytes=args.max_memory_bytes,
        max_json_bytes=args.max_json_bytes,
        max_output_bytes=args.max_output_bytes,
        max_pixels=args.max_pixels,
        max_vertex_records=args.max_vertex_records,
    )


def _add_limits(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--max-input-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--max-memory-bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--max-json-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--max-output-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--max-pixels", type=int, default=4 * 1024 * 1024)
    parser.add_argument("--max-vertex-records", type=int, default=1_000_000)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.menu_preprocess",
        description=__doc__,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    normal = sub.add_parser("normal", help="validate/decode a menu-light normal DDS")
    normal.add_argument("input")
    normal.add_argument("--signature", required=True,
                        choices=("p5_MAD_NORMAL", "p1200005_ADD_NORMAL"))
    normal.add_argument("--output", help="normalized RGBA8 XYZ DDS (optional)")
    normal.add_argument("--report", help="JSON report/manifest (recommended)")
    normal.add_argument("--source-layout", default="auto",
                        choices=("auto", "rgba_xzyw", "rgba_xyzw"))
    normal.add_argument("--allow-layout-conversion", action="store_true",
                        help="explicitly permit a conventional xyzw source")
    normal.add_argument("--strict-range", action="store_true",
                        help="fail when decoded mean normal length exceeds 0.5")
    _add_limits(normal)

    material = sub.add_parser("material", help="validate tangent basis and c0..c7")
    material.add_argument("descriptor")
    material.add_argument("--output", help="write the successful loader manifest")
    _add_limits(material)

    glyph = sub.add_parser("glyph", help="validate fixed Scaleform glyph data")
    glyph.add_argument("descriptor")
    glyph.add_argument("--output", help="write the successful loader manifest")
    _add_limits(glyph)

    sample = sub.add_parser("sample", help="optional read-only retail sample")
    sample.add_argument("--data", default=DEFAULT_DATA)
    sample.add_argument("--out", default=DEFAULT_OUT)
    _add_limits(sample)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        limits = _limits(args)
        if args.command == "normal":
            report = preprocess_normal(
                args.input,
                signature=args.signature,
                output_path=args.output,
                report_path=args.report,
                policy=NormalPolicy(
                    source_layout=args.source_layout,
                    allow_layout_conversion=args.allow_layout_conversion,
                    max_mean_normal_length=0.50 if args.strict_range else None,
                ),
                limits=limits,
            )
            print("normal: %s %s -> %s" % (
                report["status"], args.signature,
                report["output"]["file"] if report["output"] else "validation only",
            ))
        elif args.command == "material":
            report = validate_material(args.descriptor, output_path=args.output,
                                       limits=limits)
            print("material: %s %s" % (report["status"], report["pipeline"]))
        elif args.command == "glyph":
            report = validate_glyph(args.descriptor, output_path=args.output,
                                    limits=limits)
            print("glyph: %s %s (%d quads)" % (
                report["status"], report["pipeline"], report["vertices"]["quads"]
            ))
        else:
            report = run_sample(args.data, args.out, limits=limits)
            print("sample: %s -> %s" % (report["status"], args.out))
        return 0
    except (MenuPreprocessError, OSError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
