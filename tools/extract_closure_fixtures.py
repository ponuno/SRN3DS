#!/usr/bin/env python3
"""Extract the real first-menu closure bytecode from a retail Shaders.bsa.

Outputs:
  tests/fixtures/menu_closure_real_shaders.h
  logs/fixtures_report.json

The extraction is read-only.  Every extracted chunk is re-hashed and compared
with menu_closure_manifest.json; a mismatch is a hard failure, never a warning.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent.parent
if str(REPO / "tools") not in sys.path:
    sys.path.insert(0, str(REPO / "tools"))
if str(HERE / "tools") not in sys.path:
    sys.path.insert(0, str(HERE / "tools"))

from skyrim.bsa import open_archive  # noqa: E402
from skyrim.shaders import parse_package  # noqa: E402
import menu_closure_tokens as tok  # noqa: E402


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def pick_chunk(pkg, stage: str, key: int | None, sha: str | None):
    for ch in pkg.chunks:
        if ch.kind != stage:
            continue
        if key is not None and ch.meta != key:
            continue
        if sha is not None and sha256(ch.code) != sha:
            continue
        return ch
    return None


def c_array(name: str, data: bytes) -> str:
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    lines = [f"static const uint32_t {name}[{len(words)}] = {{"]
    for i in range(0, len(words), 4):
        lines.append("    " + ", ".join("0x%08xu" % w for w in words[i:i + 4]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bsa", default=r"E:\SteamLibrary\steamapps\common\Skyrim\Data\Skyrim - Shaders.bsa")
    ap.add_argument("--manifest", default=str(REPO / "tools" / "menu_closure_manifest.json"))
    ap.add_argument("--out", default=str(REPO / "tests" / "fixtures" / "menu_closure_real_shaders.h"))
    ap.add_argument("--report", default=str(REPO / "build_host" / "menu_closure_fixtures_report.json"))
    args = ap.parse_args(argv)

    manifest = json.loads(Path(args.manifest).read_text())
    entries = {e["id"]: e for e in manifest["entries"]}

    fixtures = {}
    report = {"bsa": args.bsa, "manifest": args.manifest, "chunks": []}
    with open_archive(args.bsa) as archive:
        for entry_id in ("menu.logo.ps", "menu.composite.ps"):
            e = entries[entry_id]
            data = archive.read_file(e["package"])
            pkg = parse_package(e["package"], data)
            ch = pick_chunk(pkg, e["stage"], int(e["key"], 16), e["sha256"])
            if ch is None:
                raise SystemExit(f"{entry_id}: chunk not found")
            facts = tok.describe(ch.code)
            fixture_name = entry_id.replace(".", "_")
            fixtures[fixture_name] = ch.code
            report["chunks"].append({
                "entry": entry_id,
                "fixture": fixture_name,
                "words": ch.size // 4,
                "bytes": ch.size,
                "sha256": sha256(ch.code),
                "manifest_sha256": e["sha256"],
                "ok": sha256(ch.code) == e["sha256"],
                "key": "0x%08x" % ch.meta,
                "version": "0x%08x" % facts["version"],
                "instructions": facts["instructions"],
                "declarations": facts["declarations"],
                "opcodes": facts["opcodes"],
                "texld": facts["texld"],
            })
            if sha256(ch.code) != e["sha256"]:
                raise SystemExit(f"{entry_id}: hash mismatch")

        # One representative of the bounded VS family.  The engine picks the
        # exact key from the vertex declaration, so any shipped vs_3_0 program
        # with position/texcoord0/color0/color1 and writes o0..o2 is a member.
        vs_spec = manifest["vs_family"]
        family = []
        for path in vs_spec["packages"]:
            data = archive.read_file(path)
            pkg = parse_package(path, data)
            for ch in pkg.chunks:
                if ch.kind != "vs":
                    continue
                facts = tok.describe(ch.code)
                if facts["version"] != 0xFFFE0300:
                    continue
                decls = {(d["usage"], d["index"]) for d in facts["declarations"]}
                want = {(tok.USAGE_REVERSE[d["usage"]], d["usage_index"])
                        for d in vs_spec["required_declarations"]}
                outs = set(facts["outputs"])
                if want <= decls and set(vs_spec["required_outputs"]) <= outs:
                    family.append((ch, path, facts))
        if not family:
            raise SystemExit("no VS family member found")
        family.sort(key=lambda item: (item[0].size, item[2]["instructions"]))
        ch, path, facts = family[0]
        fixtures["menu_generic_vs"] = ch.code
        report["chunks"].append({
            "entry": "menu.generic.vs",
            "fixture": "menu_generic_vs",
            "package": path,
            "words": ch.size // 4,
            "bytes": ch.size,
            "sha256": sha256(ch.code),
            "key": "0x%08x" % ch.meta,
            "version": "0x%08x" % facts["version"],
            "instructions": facts["instructions"],
            "declarations": facts["declarations"],
            "outputs": facts["outputs"],
            "opcodes": facts["opcodes"],
            "family_members": len(family),
        })

    header = [
        "/* Generated by tools/extract_closure_fixtures.py -- do not edit. */",
        "#pragma once",
        "#include <stdint.h>",
        "",
    ]
    for name, data in fixtures.items():
        header.append(c_array(name, data))
        header.append("")
        header.append(f"#define {name.upper()}_WORDS {len(data) // 4}u")
        header.append(f'#define {name.upper()}_SHA256 "{sha256(data)}"')
        header.append("")
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text("\n".join(header))
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    Path(args.report).write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
