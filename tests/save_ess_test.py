#!/usr/bin/env python3
"""Tests for tools/save_ess.py.

Two fixtures:
  * a synthetic minimal LE save built byte-by-byte here (header, RGB
    screenshot, one plugin, global data, one zlib change form and one plain
    change form, formID array and empty unknown3 table);
  * optionally a real save from the local Saves directory (read only): only
    the header and the first change-form chunk are parsed.
"""
import glob
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import save_ess  # noqa: E402

FAILURES = 0


def check(cond, msg):
    global FAILURES
    if not cond:
        FAILURES += 1
        print("FAIL:", msg)


def wstr_bytes(text):
    b = text.encode("latin-1")
    return struct.pack("<H", len(b)) + b


def build_minimal_save():
    shot_w, shot_h, bpp = 3, 2, 3
    shot = bytes((i * 17) & 0xFF for i in range(shot_w * shot_h * bpp))
    plugins = ["Skyrim.esm"]
    plugin_block = bytes([len(plugins)]) + b"".join(wstr_bytes(p) for p in plugins)

    def header():
        out = bytearray()
        out += b"TESV_SAVEGAME"
        out += struct.pack("<III", 87, 9, 3)          # headerSize, version, save#
        out += wstr_bytes("Tester")
        out += struct.pack("<I", 1)
        out += wstr_bytes("Test Cell")
        out += wstr_bytes("000.01.01")
        out += wstr_bytes("NordRace")
        out += struct.pack("<H", 0)
        out += struct.pack("<ff", 10.0, 100.0)
        out += struct.pack("<Q", 0)
        out += struct.pack("<II", shot_w, shot_h)
        out += shot
        out += bytes([74])                            # formVersion
        out += struct.pack("<I", len(plugin_block))
        out += plugin_block
        return bytes(out)

    # change forms: one compressed (REFR) and one plain (QUST)
    raw = b"compressed-change-form-payload"
    comp = zlib.compress(raw, 9)
    cf1 = (b"\x40\xc0\xf2" + struct.pack("<I", 0x0) + bytes([0, 74,
           len(comp), len(raw)]) + comp)
    cf2 = (b"\x41\xc0\xf2" + struct.pack("<I", 0x0) + bytes([8, 74,
           3, 0]) + b"abc")
    changes = cf1 + cf2
    gdt1 = struct.pack("<II", 1, 4) + b"\x00\x00\x00\x00"

    table_off = len(header())
    gdt1_off = table_off + 100
    change_off = gdt1_off + len(gdt1)
    formid_off = change_off + len(changes)
    formid = struct.pack("<I", 2) + struct.pack("<II", 0x00000001, 0x0000000F)
    unknown3_off = formid_off + len(formid)
    unknown3 = struct.pack("<I", 0)

    table = struct.pack(
        "<25I",
        formid_off, unknown3_off, gdt1_off, gdt1_off,
        change_off, gdt1_off,
        1, 0, 1, 2,
        *([0] * 15),
    )
    assert len(table) == 100, len(table)
    return header() + table + gdt1 + changes + formid + unknown3


def test_synthetic(tmp):
    path = os.path.join(tmp, "synthetic.ess")
    with open(path, "wb") as f:
        f.write(build_minimal_save())

    r = save_ess.parse_save(path)
    check(r["version"] == 9, "synthetic version")
    check(r["formVersion"] == 74, "synthetic formVersion")
    check(r["screenshot"] == {"width": 3, "height": 2, "bpp": 3, "bytes": 18},
          "synthetic screenshot")
    check(r["pluginCount"] == 1 and r["plugins"] == ["Skyrim.esm"],
          "synthetic plugin list")
    check(r["pluginTrailingBytes"] == 0, "synthetic plugin size")
    check(r["changes"]["declared"] == 2 and r["changes"]["parsed"] == 2,
          "synthetic change count")
    check(r["changes"]["compressed"] == 1, "synthetic compressed count")
    check(r["changes"]["inflatedOk"] == 1, "synthetic inflate")
    check(r["changes"]["uncompressedBytes"] == len(
        b"compressed-change-form-payload"), "synthetic inflated length")
    check(r["changes"]["types"] == {"REFR": 1, "QUST": 1},
          "synthetic change types")
    check(r["formIDArrayCount"] == 2, "synthetic formID count")
    check(r["unknown3TableSize"] == 0, "synthetic unknown3")
    check(save_ess.validate(r) == [], "synthetic validates clean")

    # Header-only path: stops after the first change form.
    p = save_ess.parse_save(path, limit_changes=1)
    check(p.get("partial") is True, "limit marks partial")
    check(p["changes"]["parsed"] == 1, "limit parses one change form")

    # Corrupt zlib must be reported, not crash.
    bad = bytearray(build_minimal_save())
    bad[change_off_of(bad) + 11] = 0x00  # clobber 0x78 in the zlib header
    bad_path = os.path.join(tmp, "bad_zlib.ess")
    with open(bad_path, "wb") as f:
        f.write(bad)
    b = save_ess.parse_save(bad_path)
    check(len(b["changes"]["errors"]) == 1, "corrupt zlib reported")
    check(any("zlib" in p for p in save_ess.validate(b)),
          "validate flags corrupt zlib")

    # Truncated header must raise SaveError, not IndexError.
    short = os.path.join(tmp, "short.ess")
    with open(short, "wb") as f:
        f.write(build_minimal_save()[:20])
    try:
        save_ess.parse_save(short)
        check(False, "truncated header should raise")
    except save_ess.SaveError:
        pass


def change_off_of(buf):
    """Offset of the first change form in the synthetic layout."""
    # header is variable; the table starts right after plugin info and the
    # first change form follows gdt1 (8 + 4 bytes data = 12) after table+100.
    # Recompute using the same fields the builder wrote.
    o = 13 + 12
    n = struct.unpack_from("<H", buf, o)[0]      # player name
    o += 2 + n
    o += 4                                       # player level
    for _ in range(3):                           # location, date, race
        n = struct.unpack_from("<H", buf, o)[0]
        o += 2 + n
    o += 2 + 4 + 4 + 8                           # sex, exp, filetime
    w, h = struct.unpack_from("<II", buf, o)
    o += 8
    o += w * h * 3                               # LE RGB screenshot
    o += 1
    pis = struct.unpack_from("<I", buf, o)[0]
    o += 4 + pis
    table_off = o
    gdt1_off = struct.unpack_from("<I", buf, table_off + 8)[0]
    return gdt1_off + 12


def find_real_save():
    candidates = []
    env = os.environ.get("SKYRIM_SAVE")
    if env:
        candidates.append(env)
    candidates += glob.glob(os.path.expanduser(
        r"~/Documents/My Games/Skyrim/Saves/*.ess"))
    candidates += glob.glob(r"C:/Users/*/Documents/My Games/Skyrim/Saves/*.ess")
    for p in candidates:
        if os.path.isfile(p) and os.path.getsize(p) > 1024:
            return p
    return None


def test_real():
    path = find_real_save()
    if not path:
        print("SKIP: no real .ess available")
        return
    full = os.path.getsize(path)
    r = save_ess.parse_save(path, limit_changes=1)
    check(r["size"] == full, "real size")
    check(r["version"] in (7, 8, 9, 12), "real version")
    check(r["pluginCount"] >= 1 and r["plugins"][0].lower() == "skyrim.esm",
          "real plugin list starts with Skyrim.esm")
    check(r["screenshot"]["bytes"] == r["screenshot"]["width"] *
          r["screenshot"]["height"] * r["screenshot"]["bpp"], "real screenshot")
    if r["changes"]["declared"]:
        check(r["changes"]["parsed"] == 1, "real first change parsed")
        if r["changes"]["compressed"]:
            check(r["changes"]["inflatedOk"] == 1,
                  "real first change form inflates")
            check(not r["changes"]["errors"], "real first chunk has no zlib error")
    else:
        print("NOTE: real save has no change forms")
    print(f"real: {os.path.basename(path)} size={full} "
          f"plugins={r['pluginCount']} changes={r['changes']['declared']} "
          f"first_compressed={bool(r['changes']['compressed'])}")


def main():
    import tempfile
    with tempfile.TemporaryDirectory(prefix="save_ess_") as tmp:
        test_synthetic(tmp)
        test_real()
    if FAILURES:
        print(f"FAIL: {FAILURES} checks")
        return 1
    print("PASS: save_ess parser")
    return 0


if __name__ == "__main__":
    sys.exit(main())
