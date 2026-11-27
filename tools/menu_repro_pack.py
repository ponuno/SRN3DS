"""Build the tiny menu-crash repro IR container.

The crash is a guest fault inside ``0x60aa70``::

    60aa70  mov eax,[ecx+8]      <- fault here (ecx = bad "this")
    ...
    60aa7e  mov ecx,[0x1310460]
    60aa84  call 0x604750        <- registry lookup
    60aa8d  mov cl,[eax+0x12]    <- tag byte: '<' or 'G'

This tool extracts the *real* retail bytes of ``0x60aa70`` and ``0x604750``
from the IR container input image, adds two synthetic stubs (a deterministic
replacement for the hash-map lookup at ``0x451a30`` and a caller that mirrors
the real call site ``mov ecx,[ebp+0x28]; call 0x60aa70``), and packs them with
the existing C packer (``tests/ir_container_pack.c`` + the real
``source/ir_decode.c`` decoder) used by ``tools/ir_pack.py``.

Outputs:

* ``tests/menu_repro_irc.h``  - generated blob embedded by the host test
* ``build_menu_repro/menu_repro.irc`` - the same container as a file

Nothing is re-implemented: the IR words are produced by the real decoder, and
the container layout is the existing ``IRCNT001`` format.

Usage (needs an MSVC environment for stdio.h, i.e. run from menu_repro.ps1 or
after vcvars64.bat):

    python tools/menu_repro_pack.py
    python tools/menu_repro_pack.py --image E:/other/ir_container_input.bin
"""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IMAGE = Path(r"E:\census_ir2\ir_container\_work\ir_container_input.bin")
CLANG = Path(r"C:\Program Files\LLVM\bin\clang.exe")
IMAGE_MAGIC = 0x49524D31
CLIENT_BASE = 0x401000  # TESV preferred image base, used by tools/ir_pack.py

# Synthetic stubs.  These live at real (but unused by this repro) guest VAs so
# that the image stays contiguous between 0x451a30 and the real functions.
STUB_LOOKUP = 0x451A30       # replaces the heap-map lookup behind 0x604750
STUB_CALLER = 0x451A60       # mirrors the real caller block at 0x5fbb95
REAL_LOOKUP = 0x604750       # mov eax,[esp+4]; push; call 0x451a30; ret 4
REAL_CHECK = 0x60AA70        # the crashing function

# 0x451a30: deterministic replacement for 0x451a30 (hash map lookup keyed by
# the 4-byte guest string pointer).  A and B are two guest dwords in the
# "registry" page: A = key pointer, B = record pointer (0 = miss).
#
#   mov eax,[esp+4]        ; key
#   mov edx,[0x705000]     ; registered key
#   cmp eax,edx
#   jne miss
#   mov eax,[0x705004]     ; registered record
#   ret
# miss:
#   xor eax,eax
#   ret
STUB_LOOKUP_BYTES = bytes.fromhex(
    "8b442404"            # mov eax,[esp+4]
    "8b1500507000"        # mov edx,[0x705000]
    "3bc2"                # cmp eax,edx
    "7507"                # jne miss (0x451a45)
    "8b0504507000"        # mov eax,[0x705004]
    "c3"                  # ret
    "33c0"                # xor eax,eax
    "c3"                  # ret
)
STUB_LOOKUP_BLOCKS = [
    (0x451A30, 0x451A3E),  # mov/mov/cmp/jne
    (0x451A3E, 0x451A45),  # hit: mov eax,[0x705004]; ret
    (0x451A45, 0x451A48),  # miss: xor eax,eax; ret
]

# 0x451a60: caller stub.  The real sites are
#   0x5fbb95: mov ecx,[ebp+0x28] / 0x5fc860: mov ecx,[eax+0x28]
#   then call 0x60aa70.
# Here the record pointer is argument 0:
#   mov eax,[esp+4]        ; record
#   mov ecx,[eax+0x28]     ; this = record->field_0x28
#   call 0x60aa70
#   ret
def _caller_bytes():
    call_at = STUB_CALLER + 7          # after mov eax,[esp+4]; mov ecx,[eax+0x28]
    rel = REAL_CHECK - (call_at + 5)
    return bytes.fromhex("8b442404" "8b4828") + b"\xe8" + struct.pack("<i", rel) + b"\xc3"


STUB_CALLER_BYTES = _caller_bytes()
STUB_CALLER_BLOCKS = [
    (0x451A60, 0x451A6C),  # ends at the call (interpreter resumes at +0xc)
    (0x451A6C, 0x451A6D),  # ret
]

# Real function block boundaries (hand-checked against the capstone listing;
# kept literal so this tool has no capstone dependency).
REAL_CHECK_BLOCKS = [
    (0x60AA70, 0x60AA77),  # mov/test/je
    (0x60AA77, 0x60AA7A),  # push eax; jmp
    (0x60AA7A, 0x60AA7E),  # mov ecx,[ecx+0xc]; push ecx
    (0x60AA7E, 0x60AA89),  # mov ecx,[0x1310460]; call 0x604750
    (0x60AA89, 0x60AA8D),  # test/je
    (0x60AA8D, 0x60AA95),  # mov cl,[eax+0x12]; cmp '<'; je
    (0x60AA95, 0x60AA9A),  # cmp 'G'; je
    (0x60AA9A, 0x60AA9C),  # xor eax,eax
    (0x60AA9C, 0x60AA9D),  # ret
]
REAL_LOOKUP_BLOCKS = [(0x604750, 0x604760)]


def read_image(path):
    data = Path(path).read_bytes()
    if len(data) < 16:
        raise SystemExit("image too short")
    magic, lo, size, _entry = struct.unpack("<IIII", data[:16])
    if magic != IMAGE_MAGIC:
        raise SystemExit("not an IRM1 container input (magic=%08x)" % magic)
    code = data[16:16 + size]
    if len(code) != size:
        raise SystemExit("truncated image")
    return code, lo, size


def extract(code, lo, size, start, end):
    if start < lo or end > lo + size:
        raise SystemExit("range %08x..%08x outside image %08x..%08x" % (start, end, lo, lo + size))
    return code[start - lo:end - lo]


def build_input(image_bytes, lo, funcs, out_path):
    payload = struct.pack("<IIII", IMAGE_MAGIC, lo, len(image_bytes), len(funcs))
    payload += image_bytes
    for addr, end, blocks in funcs:
        payload += struct.pack("<III", addr, end, len(blocks))
        payload += b"".join(struct.pack("<II", a, b) for a, b in blocks)
    Path(out_path).write_bytes(payload)


def find_clang(explicit):
    if explicit:
        p = Path(explicit)
        if p.exists():
            return p
        raise SystemExit("clang not found: %s" % explicit)
    if CLANG.exists():
        return CLANG
    found = shutil.which("clang")
    if not found:
        raise SystemExit("clang not found")
    return Path(found)


def find_packer(explicit, clang, work):
    """Prefer an already built packer (the main tree has one); else compile."""
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    candidates.append(ROOT / "build_ir_container" / "ir_container_pack.exe")
    candidates.append(Path(r"C:\Users\vasya\Documents\Skyrim_N3DS\build_ir_container"
                           r"\ir_container_pack.exe"))
    for c in candidates:
        if c.exists():
            return c
    return compile_packer(clang, work / "ir_container_pack.exe")


def compile_packer(clang, out):
    cmd = [str(clang), "-std=gnu11", "-O1", "-g", "-Isource", "-Istaged",
           str(ROOT / "tests" / "ir_container_pack.c"),
           str(ROOT / "source" / "ir_decode.c"),
           "-o", str(out)]
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("packer build failed (MSVC INCLUDE missing?)")
    return out


def emit_header(container_bytes, path):
    sha = hashlib.sha256(container_bytes).hexdigest()
    lines = [
        "/* Generated by tools/menu_repro_pack.py. Do not edit.",
        " *",
        " * Mini IRCNT001 container for the menu crash at 0x60aa70.",
        " * Real retail bytes: 0x60aa70 (check) and 0x604750 (registry call).",
        " * Synthetic stubs:   0x451a30 (lookup) and 0x451a60 (caller).",
        " */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "#define MENU_REPRO_ENTRY        0x%08xu" % STUB_CALLER,
        "#define MENU_REPRO_CHECK        0x%08xu" % REAL_CHECK,
        "#define MENU_REPRO_LOOKUP       0x%08xu" % REAL_LOOKUP,
        "#define MENU_REPRO_LOOKUP_STUB  0x%08xu" % STUB_LOOKUP,
        "#define MENU_REPRO_REGISTRY_KEY 0x00705000u",
        "#define MENU_REPRO_REGISTRY_REC 0x00705004u",
        "#define MENU_REPRO_GLOBAL       0x01310460u",
        '#define MENU_REPRO_IRC_SHA256   "%s"' % sha,
        "#define MENU_REPRO_IRC_BYTES    %du" % len(container_bytes),
        "",
        "static const uint8_t menu_repro_irc[MENU_REPRO_IRC_BYTES] = {",
    ]
    for i in range(0, len(container_bytes), 16):
        lines.append("    " + ",".join("0x%02x" % b for b in container_bytes[i:i + 16]) + ",")
    lines += ["};", ""]
    Path(path).write_text("\n".join(lines), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    ap.add_argument("--clang", default="")
    ap.add_argument("--packer", default="")
    ap.add_argument("--out-dir", type=Path, default=ROOT / "build_menu_repro")
    ap.add_argument("--header", type=Path, default=ROOT / "tests" / "menu_repro_irc.h")
    args = ap.parse_args()

    code, lo, size = read_image(args.image)
    real_check = extract(code, lo, size, REAL_CHECK, 0x60AA9D)
    real_lookup = extract(code, lo, size, REAL_LOOKUP, 0x604760)

    lo_va = STUB_LOOKUP
    hi_va = 0x60AA9D
    image = bytearray(b"\xcc" * (hi_va - lo_va))

    def put(va, blob):
        image[va - lo_va:va - lo_va + len(blob)] = blob

    put(STUB_LOOKUP, STUB_LOOKUP_BYTES)
    put(STUB_CALLER, STUB_CALLER_BYTES)
    put(REAL_LOOKUP, real_lookup)
    put(REAL_CHECK, real_check)

    funcs = [
        (STUB_LOOKUP, 0x451A48, STUB_LOOKUP_BLOCKS),
        (STUB_CALLER, 0x451A6D, STUB_CALLER_BLOCKS),
        (REAL_LOOKUP, 0x604760, REAL_LOOKUP_BLOCKS),
        (REAL_CHECK, 0x60AA9D, REAL_CHECK_BLOCKS),
    ]

    args.out_dir.mkdir(parents=True, exist_ok=True)
    work = args.out_dir / "_work"
    work.mkdir(parents=True, exist_ok=True)
    inp = work / "menu_repro_input.bin"
    build_input(bytes(image), lo_va, funcs, inp)

    clang = find_clang(args.clang)
    packer = find_packer(args.packer, clang, work)
    container = args.out_dir / "menu_repro.irc"
    p = subprocess.run([str(packer), str(inp), str(container),
                        "--report", str(work / "menu_repro.json")],
                       capture_output=True, text=True)
    print(p.stdout.strip())
    if p.returncode != 0:
        print(p.stderr)
        raise SystemExit("packer failed")

    emit_header(container.read_bytes(), args.header)
    print("wrote %s and %s" % (container, args.header))
    return 0


if __name__ == "__main__":
    sys.exit(main())
