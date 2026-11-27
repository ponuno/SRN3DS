"""Build the host-only SM3 corpus that ``tools/sm3_dump`` consumes (P1).

Reads the retail ``Skyrim - Shaders.bsa`` read-only, reuses the existing
package/family loader (``tools/shaders/packages.py``) so the family assignment
is byte-identical to ``docs/SHADER_CENSUS.json``, and writes:

* ``<out>/ps_chunks.bin`` / ``<out>/vs_chunks.bin`` -- the project chunk-dump
  format ``u32 count; count * {u32 size; bytes}``;
* ``<out>/corpus.json`` -- one row per chunk in dump order:
  ``{kind, index, sha256, sha1, program_sha1, family, package, version, bytes}``.

Everything is a local build artefact: retail bytecode is never copied into the
repository and the JSON keeps hashes/metadata only.

Usage::

    .venv\\Scripts\\python.exe tools\\sm3_corpus.py --out E:\\host_sm3
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if REPO not in sys.path:
    sys.path.insert(0, REPO)
sys.path.insert(0, os.path.join(REPO, "tools"))

from shaders import packages  # noqa: E402

DEFAULT_DATA = [
    r"E:\SteamLibrary\steamapps\common\Skyrim\Data",
]
DEFAULT_OUT = r"E:\host_sm3"


def find_data(explicit: str = "") -> str:
    if explicit:
        return explicit
    for candidate in DEFAULT_DATA:
        if os.path.exists(os.path.join(candidate, "Skyrim - Shaders.bsa")):
            return candidate
    raise SystemExit("Shaders.bsa not found; pass --data")


def collect(data_dir: str) -> list:
    all_packages = packages.load_packages(data_dir, include_update=False)
    effective = packages.effective_packages(all_packages)
    return packages.build_records(effective)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", default="")
    parser.add_argument("--out", default=DEFAULT_OUT)
    args = parser.parse_args(argv)

    data_dir = find_data(args.data)
    records = collect(data_dir)

    os.makedirs(args.out, exist_ok=True)
    buckets = {"ps": [], "vs": []}
    rows = []
    for record in records:
        chunk = record.chunk
        kind = chunk.kind
        import hashlib

        sha256 = hashlib.sha256(chunk.code).hexdigest()
        bucket = buckets.setdefault(kind, [])
        rows.append({
            "kind": kind,
            "index": len(bucket),
            "sha256": sha256,
            "sha1": record.bytecode_sha1,
            "program_sha1": record.program_sha1,
            "family": record.family,
            "package": record.package.path,
            "version": chunk.version_name,
            "bytes": len(chunk.code),
        })
        bucket.append(chunk.code)

    for kind, blobs in buckets.items():
        path = os.path.join(args.out, "%s_chunks.bin" % kind)
        with open(path, "wb") as handle:
            handle.write(struct.pack("<I", len(blobs)))
            for blob in blobs:
                handle.write(struct.pack("<I", len(blob)))
                handle.write(blob)

    with open(os.path.join(args.out, "corpus.json"), "w", encoding="utf-8") as handle:
        json.dump({
            "data_dir": data_dir,
            "packages_effective": len(packages.effective_packages(
                packages.load_packages(data_dir, include_update=False))),
            "chunks": len(rows),
            "kinds": {kind: len(blobs) for kind, blobs in buckets.items()},
            "rows": rows,
        }, handle, indent=1)
        handle.write("\n")

    print("corpus: %d chunks (%s) -> %s" % (
        len(rows),
        ", ".join("%s=%d" % (k, len(v)) for k, v in sorted(buckets.items())),
        args.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
