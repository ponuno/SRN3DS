"""Compare a protected PE with its unprotected/official counterpart.

Prints per-section SHA256 and the differing sections. Used to prove that a
provided executable is the same build as the installed game file (all data and
resource sections identical, only the encrypted .text and the protection
section differ). Read-only.

Usage: python tools/compare_pe.py <reference.exe> <candidate.exe>
"""
import argparse
import hashlib
from pathlib import Path

import pefile


def sections(path):
    pe = pefile.PE(str(path), fast_load=True)
    info = {}
    for s in pe.sections:
        name = s.Name.decode(errors="replace").strip("\x00")
        data = s.get_data()
        info[name] = {
            "rva": s.VirtualAddress,
            "vsize": s.Misc_VirtualSize,
            "raw": s.SizeOfRawData,
            "sha256": hashlib.sha256(data).hexdigest(),
        }
    entry = pe.OPTIONAL_HEADER.ImageBase + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    return info, entry


def digest(path):
    data = Path(path).read_bytes()
    return len(data), hashlib.sha1(data).hexdigest(), hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    args = ap.parse_args()

    a, ea = sections(args.reference)
    b, eb = sections(args.candidate)
    for label, path, entry, secs in (("reference", args.reference, ea, a), ("candidate", args.candidate, eb, b)):
        size, sha1, sha256 = digest(path)
        print(f"{label}: {path}")
        print(f"  bytes {size}  entry {entry:#x}")
        print(f"  sha1 {sha1}")
        print(f"  sha256 {sha256}")

    differing = []
    print("sections:")
    for name in sorted(set(a) | set(b)):
        x, y = a.get(name), b.get(name)
        same = bool(x and y and x["sha256"] == y["sha256"])
        print(f"  {name:8s} reference={bool(x)} candidate={bool(y)} identical={same}")
        if not same:
            differing.append(name)
    print("differing sections:", ", ".join(differing) if differing else "none")
    if differing == ["text"] or differing == [".text"] or differing == [".bind", ".text"]:
        print("verdict: same PE build; only code section/protection wrapper differs")
        return 0
    print("verdict: inspect differences manually")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
