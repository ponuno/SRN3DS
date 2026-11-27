#!/usr/bin/env python3
"""CI test for tools/menu_swf_assets.py.

The synthetic half builds small FWS movies plus a tiny BSA overlay
(``tools/skyrim/bsa_write.py``) so the tag walk, import/font resolution and the
named-missing paths are covered without any game bytes.  Set
``SKYRIM_N3DS_REAL_DATA=1`` (or to a Data directory) to also require the real
``Skyrim - Interface.bsa`` StartMenu closure: every bitmap/font asset resolves
through the shipped overlay and the missing list is empty.
"""

from __future__ import annotations

import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path

_HERE = Path(__file__).resolve().parent
ROOT = _HERE.parent if (_HERE.parent / "tools" / "skyrim").is_dir() else _HERE
sys.path.insert(0, str(ROOT / "tools"))

import menu_swf_assets as assets  # noqa: E402
from skyrim import bsa_write  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, what):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("FAIL: %s" % what)


# ------------------------------------------------------------- SWF builder ---
def tag(code: int, payload: bytes = b"") -> bytes:
    if len(payload) < 0x3F:
        return struct.pack("<H", (code << 6) | len(payload)) + payload
    return (struct.pack("<H", (code << 6) | 0x3F) +
            struct.pack("<I", len(payload)) + payload)


def _bit_writer(values, nbits):
    bits = 5
    for _ in values:
        bits += nbits
    byte_len = (bits + 7) // 8
    acc = 0
    acc = (acc << 5) | (nbits & 0x1F)
    for value in values:
        acc = (acc << nbits) | (value & ((1 << nbits) - 1))
    pad = byte_len * 8 - (5 + 4 * nbits)
    acc <<= pad
    return acc.to_bytes(byte_len, "big")


def build_swf(tags: list[bytes], rect=(0, 4000, 0, 2000)) -> bytes:
    body = _bit_writer(rect, 16)
    body += struct.pack("<HH", 0x1800, 1)
    body += b"".join(tags)
    body += tag(1)   # ShowFrame
    body += tag(0)   # End
    return b"FWS\x0a" + struct.pack("<I", len(body) + 8) + body


def font_tag(font_id: int, flags: int, name: str, glyphs: int) -> bytes:
    raw = name.encode("latin-1") + b"\x00"
    payload = (struct.pack("<HBB", font_id, flags, 1) + bytes([len(raw)]) +
               raw + struct.pack("<H", glyphs))
    return tag(75, payload)


def import_tag(url: str, symbols: list[tuple[int, str]]) -> bytes:
    payload = url.encode("latin-1") + b"\x00" + b"\x01\x00"
    payload += struct.pack("<H", len(symbols))
    for cid, name in symbols:
        payload += struct.pack("<H", cid) + name.encode("latin-1") + b"\x00"
    return tag(71, payload)


def jpeg_bytes(width: int, height: int) -> bytes:
    sof = (b"\xff\xc0" + struct.pack(">H", 17) + b"\x08" +
           struct.pack(">HH", height, width) + b"\x03" +
           b"\x01\x11\x00\x02\x11\x01\x03\x11\x01")
    return b"\xff\xd8" + sof + b"\xff\xd9"


def movie(good=True) -> bytes:
    tags = []
    tags.append(tag(20, struct.pack("<HBHH", 7, 5, 4, 2) + b"\x00" * 32))
    jpeg = jpeg_bytes(32, 16)
    tags.append(tag(35, struct.pack("<HI", 8, len(jpeg)) + jpeg))
    tags.append(font_tag(1, 0x00, "Futura Condensed", 243))
    tags.append(tag(14, struct.pack("<HB", 3, 0x2E) + struct.pack("<I", 44100) +
                    b"\x00" * 16))
    symbols = [(34, "$EverywhereMediumFont"), (41, "$EverywhereFont")]
    if not good:
        symbols += [(50, "$TwinFont"), (51, "$MissingFont")]
    tags.append(import_tag("gfxfontlib.swf", symbols))
    name = b"Interface\\art\\logo.dds\x00"
    tags.append(tag(1000, struct.pack("<H", 60) + name +
                    struct.pack("<II", 64, 64)))
    # A nested tag proves sprite-internal tags are not counted as assets.
    sprite = struct.pack("<HH", 100, 1) + tag(14, struct.pack(
        "<HB", 99, 0x2E) + struct.pack("<I", 1000))
    tags.append(tag(39, sprite))
    return build_swf(tags)


def font_library() -> bytes:
    return build_swf([
        font_tag(10, 0x00, "Widget", 5),
        font_tag(11, 0x01, "Widget", 5),
        font_tag(12, 0x00, "Twin", 5),
        font_tag(13, 0x00, "Twin", 5),
    ])


FONTCONFIG = (
    'fontlib "Interface\\Fonts_Test.swf"\n'
    'map "$EverywhereMediumFont" = "Widget" Normal\n'
    'map "$EverywhereFont" = "Widget" Normal\n'
    'map "$TwinFont" = "Twin" Normal\n'
)


def write_overlay(tmp: Path, good: bool) -> Path:
    data = tmp / "Data"
    data.mkdir(parents=True)
    bsa_write.write_bsa(str(data / "Skyrim - Interface.bsa"), {
        "interface\\fontconfig.txt": FONTCONFIG.encode("ascii"),
        "interface\\fonts_test.swf": font_library(),
        "interface\\art\\logo.dds": b"DDS " + b"\x00" * 124,
        "interface\\startmenu.swf": movie(good),
    })
    return data


# ---------------------------------------------------------------- synthetic ---
def test_parsers(tmp: Path):
    data = write_overlay(tmp, good=True)
    with assets.DataOverlay(data) as overlay:
        found = overlay.resolve("Interface\\Fonts_Test.swf")
        check(found is not None and "Skyrim - Interface.bsa" in found.source,
              "overlay resolves a case-mismatched archive path")
        check(overlay.resolve("interface/nope.swf") is None,
              "overlay reports an absent path as None")

    report = assets.collect_assets(movie(True), "interface/startmenu.swf")
    check(report.tags > 0 and report.nested_tags == 1,
          "tag walk counts the nested sprite tag separately")
    check(report.counts["bitmaps"] == 2, "two bitmap tags")
    check(report.counts["fonts"] == 1, "one local font tag")
    check(report.counts["sounds"] == 1, "sprite-internal sound is not counted")
    check(report.counts["imports"] == 1, "one ImportAssets2 tag")
    check(report.counts["external_images"] == 1, "one DefineExternalImage")
    lossless, jpeg = report.bitmaps
    check(lossless["id"] == 7 and lossless["format"] == 5 and
          (lossless["width"], lossless["height"]) == (4, 2),
          "DefineBitsLossless2 header")
    check(jpeg["kind"] == "DefineBitsJPEG3" and
          (jpeg["width"], jpeg["height"]) == (32, 16) and
          jpeg["alpha_bytes"] == len(jpeg_bytes(32, 16)),
          "DefineBitsJPEG3 SOF dimensions and alpha split")
    sound = report.sounds[0]
    check(sound["id"] == 3 and sound["rate"] == 44100 and
          abs(sound["seconds"] - 1.0) < 0.01, "DefineSound rate/duration")
    font = report.fonts[0]
    check(font["id"] == 1 and font["name"] == "Futura Condensed" and
          font["glyphs"] == 243, "DefineFont3 name/glyph count")
    symbols = report.imports[0]["symbols"]
    check([s["id"] for s in symbols] == [34, 41] and
          symbols[0]["name"] == "$EverywhereMediumFont",
          "ImportAssets2 symbol order")
    check(report.external_images[0]["file"] == "Interface\\art\\logo.dds",
          "DefineExternalImage file name")

    header = assets.frame_header(assets.swf_body(movie(True)))
    check(header["width_twips"] == 4000 and header["height_twips"] == 2000,
          "frame header rect")
    check(header["frame_rate"] == 24.0 and header["frame_count"] == 1,
          "frame header rate/count")


def test_container_refusals():
    for bad, why in (
            (b"FWS", "short header"),
            (b"ZWS\x0a" + struct.pack("<I", 8), "ZWS is named unsupported"),
            (b"FWS\x0a" + struct.pack("<I", 999) + b"\x00" * 4,
             "no frame header"),
    ):
        try:
            assets.collect_assets(bad)
        except assets.SwfError:
            check(True, why)
        else:
            check(False, why)
    # CWS declared length mismatch must be a named refusal.
    body = assets.swf_body(movie(True))
    cws = b"CWS\x0a" + struct.pack("<I", len(body)) + zlib.compress(body)
    try:
        assets.swf_body(cws)
    except assets.SwfError:
        check(True, "CWS declared mismatch")
    else:
        check(False, "CWS declared mismatch")


def test_fontconfig():
    config = assets.parse_fontconfig(
        '# comment\n'
        'fontlib "Interface\\fonts_en.swf"\n'
        'map "$EverywhereMediumFont" = "Futura Condensed" Normal\n'
        'map "$EverywhereBoldFont" = "Futura Condensed" Bold\n')
    check(config["fontlibs"] == ["Interface/fonts_en.swf"],
          "fontlib path normalised to forward slashes")
    check(len(config["maps"]) == 2 and
          config["maps"][1]["bold"] and not config["maps"][0]["bold"],
          "map style flags")


def test_closure(tmp: Path):
    good = write_overlay(tmp / "good", good=True)
    with assets.DataOverlay(good) as overlay:
        report = assets.close_assets(
            assets.collect_assets(movie(True), "interface/startmenu.swf"),
            overlay)
    check(report.ok and not report.missing, "good movie closes with no missing")
    check(report.resolved == 7,
          "2 bitmaps + 1 font + 1 sound + 2 imports + 1 image resolved")
    check(len(report.deferred) == 1 and report.deferred[0]["id"] == 3,
          "embedded sound is deferred, not missing")
    record = report.imports[0]
    resolved = record["symbols_resolved"]
    check(all(s["how"] == "fontconfig" for s in resolved),
          "virtual gfxfontlib resolves through fontconfig")
    check([s["font_id"] for s in resolved] == [10, 10],
          "symbols resolve to the plain Widget face")
    check(record["symbols_resolved"][0]["source"] == "Skyrim - Interface.bsa"
          and record["symbols_resolved"][0]["fontlib_bytes"] > 0,
          "fontlib size reported")
    check(report.external_images[0]["resolution"]["size"] == 128,
          "DefineExternalImage resolves through the overlay")

    bad = write_overlay(tmp / "bad", good=False)
    with assets.DataOverlay(bad) as overlay:
        broken = assets.close_assets(
            assets.collect_assets(movie(False), "interface/startmenu.swf"),
            overlay)
    check(not broken.ok, "ambiguous/missing symbols keep the closure open")
    reasons = [m["symbol"] for m in broken.missing if m["kind"] == "import"]
    check(reasons == ["$TwinFont", "$MissingFont"],
          "ambiguous face and absent alias are named per symbol")
    twin = next(m for m in broken.missing if m.get("symbol") == "$TwinFont")
    check("ambiguous" in twin["reason"], "ambiguity is named, not guessed")
    missing = next(m for m in broken.missing if m.get("symbol") == "$MissingFont")
    check("no fontconfig map" in missing["reason"],
          "unmapped alias is a named refusal")


def test_missing_external_image(tmp: Path):
    data = write_overlay(tmp, good=True)
    # Drop the image from a private copy of the overlay directory.
    subset = tmp / "subset"
    subset.mkdir()
    bsa_write.write_bsa(str(subset / "Skyrim - Interface.bsa"), {
        "interface\\fontconfig.txt": FONTCONFIG.encode("ascii"),
        "interface\\fonts_test.swf": font_library(),
        "interface\\startmenu.swf": movie(True),
    })
    with assets.DataOverlay(subset) as overlay:
        report = assets.close_assets(
            assets.collect_assets(movie(True), "interface/startmenu.swf"),
            overlay)
    check(any(m["kind"] == "external-image" for m in report.missing),
          "absent external image is a named missing entry")
    check(not report.ok, "missing external image keeps the closure open")


# --------------------------------------------------------------- real data ---
REAL_STARTMENU_SHA = ("26a46704c034536fb64ce9064cd70df74fe656bda1ea1a5241f20dc"
                      "4e6d38922")
REAL_FONTS_SIZE = 417809


def real_data_dir() -> Path | None:
    value = os.environ.get("SKYRIM_N3DS_REAL_DATA")
    if not value:
        print("SKIP: set SKYRIM_N3DS_REAL_DATA=1 (or a Data dir) for the real "
              "StartMenu closure")
        return None
    path = Path(r"F:\Skyrim\Data") if value == "1" else Path(value)
    if not (path / "Skyrim - Interface.bsa").is_file():
        print("SKIP: %s has no Skyrim - Interface.bsa" % path)
        return None
    return path


def test_real(tmp: Path):
    data = real_data_dir()
    if data is None:
        return
    movie_bytes, path, info = assets.load_movie(
        data, "Skyrim - Interface.bsa", "interface/startmenu.swf")
    report = assets.collect_assets(movie_bytes, path)
    report.movie["archive"] = info["archive"]
    report.movie["stored_bytes"] = info["stored"]
    with assets.DataOverlay(data) as overlay:
        assets.close_assets(report, overlay)

    check(report.movie["sha256"] == REAL_STARTMENU_SHA,
          "real Interface.bsa StartMenu sha256")
    check(report.movie["stored_bytes"] == 60157, "real StartMenu stored size")
    check(report.counts == {"bitmaps": 0, "fonts": 1, "sounds": 0,
                            "imports": 2, "external_images": 0},
          "real StartMenu asset census (%s)" % report.counts)
    check(report.fonts[0]["id"] == 1 and report.fonts[0]["glyphs"] == 243,
          "real local DefineFont3 id 1 with 243 glyphs")
    check(report.ok and not report.missing,
          "real closure has no missing asset (%s)" % report.missing)
    resolved = {s["id"]: s for r in report.imports
                for s in r["symbols_resolved"]}
    check(set(resolved) == {34, 41}, "real import ids 34/41")
    check(resolved[34]["font_id"] == 3 and resolved[34]["face"] ==
          "Futura Condensed", "$EverywhereMediumFont -> DefineFont3 3")
    check(resolved[41]["font_id"] == 1 and resolved[41]["face"] ==
          "Futura CondensedLight", "$EverywhereFont -> DefineFont3 1")
    check(all(s["fontlib"].lower() == "interface/fonts_en.swf" and
              s["fontlib_bytes"] == REAL_FONTS_SIZE for s in resolved.values()),
          "real fontlib is fonts_en.swf, %d B" % REAL_FONTS_SIZE)
    rc = assets.main(["--data", str(data), "--bsa", "Skyrim - Interface.bsa"])
    check(rc == 0, "tool exit status on real data")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="menu_swf_assets_test_") as d:
        tmp = Path(d)
        (tmp / "parsers").mkdir()
        test_parsers(tmp / "parsers")
        test_container_refusals()
        test_fontconfig()
        test_closure(tmp)
        test_missing_external_image(tmp / "no_image")
        test_real(tmp)
    print("menu swf assets test: %d checks, %d failures" % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
