"""Checks the read-only TESV StartMenu harness map without game bytes.

The real game executable is exercised only when present, and that check is
skipped otherwise.  The signature table itself is always validated, including a
one-byte corruption that must be reported as a failure.
"""

import os
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import menu_harness  # noqa: E402


def build_tiny_pe():
    image = bytearray(0x400)
    e = 0x80
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, e)
    image[e:e + 4] = b"PE\0\0"
    struct.pack_into("<H", image, e + 6, 1)
    struct.pack_into("<H", image, e + 20, 0xE0)
    struct.pack_into("<I", image, e + 0x28, 0x1000)
    struct.pack_into("<I", image, e + 0x34, 0x400000)
    sec = e + 24 + 0xE0
    image[sec:sec + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", image, sec + 8, 0x100, 0x1000, 0x200, 0x200)
    image[0x200:0x208] = b"ABCDEFGH"
    return bytes(image)


def main():
    checks = 0
    assert menu_harness.self_test() == len(menu_harness.ANCHORS) + \
        len(menu_harness.STRINGS)
    checks += 1

    with tempfile.NamedTemporaryFile(suffix=".exe", delete=False) as tmp:
        tmp.write(build_tiny_pe())
        path = tmp.name
    try:
        pe = menu_harness.Pe(path)
        assert pe.base == 0x400000
        assert pe.entry == 0x401000
        assert pe.read_va(0x401000, 8) == b"ABCDEFGH"
        assert pe.sections[0]["name"] == ".text"
        pe.close()
        checks += 1
        # Every recorded anchor must be unique, because a shifted/duplicated VA
        # would silently describe the wrong instruction or string.
        vas = [a[1] for a in menu_harness.ANCHORS] + \
              [s[1] for s in menu_harness.STRINGS]
        assert len(vas) == len(set(vas))
        checks += 1
    finally:
        os.unlink(path)

    real = os.environ.get("TESV_EXE", r"F:\Skyrim\TESV.exe")
    if os.path.exists(real):
        pe = menu_harness.Pe(real)
        try:
            ok, rows = menu_harness.verify_pe(pe)
        finally:
            pe.close()
        assert ok, [r for r in rows if not r[1]]
        checks += 1
    print("menu_harness_test: %d checks OK" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
