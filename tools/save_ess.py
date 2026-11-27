#!/usr/bin/env python3
"""Read-only validator for Skyrim LE ``.ess`` save files.

The engine's save loader (menu "Continue" -> ``0x6a0ae0`` -> ``0x6a0870``)
parses the whole file in guest code.  This tool only answers host-layer
questions: is the container well formed, are the offsets in range, do the
plugin names parse, and do the zlib change-form chunks inflate to exactly the
length declared in the record header?

Format reference: UESP "Skyrim Mod:Save File Format".  LE saves below are
version 9, ``formVersion`` 74, RGB (3 bytes/pixel) screenshot, and zlib
compressed *change forms*.

Usage:
  python tools/save_ess.py <file.ess> [...]          # one summary per file
  python tools/save_ess.py --dir <saves-dir>        # every *.ess in a dir
  python tools/save_ess.py --json <file.ess>        # machine-readable
  python tools/save_ess.py --limit-changes 8 <f>    # header + first chunks

Exit code is 1 if any file fails structural validation.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import zlib

MAGIC = b"TESV_SAVEGAME"

# Change-form type (low 6 bits of the type byte) -> record signature.
CHANGE_FORM_TYPES = {
    0: "REFR", 1: "ACHR", 2: "PMIS", 3: "PGRE", 4: "PBEA", 5: "PFLA",
    6: "CELL", 7: "INFO", 8: "QUST", 9: "NPC_", 10: "ACTI", 11: "TACT",
    12: "ARMO", 13: "BOOK", 14: "CONT", 15: "DOOR", 16: "INGR", 17: "LIGH",
    18: "MISC", 19: "APPA", 20: "STAT", 21: "MSTT", 22: "FURN", 23: "WEAP",
    24: "AMMO", 25: "KEYM", 26: "ALCH", 27: "IDLM", 28: "NOTE", 29: "ECZN",
    30: "CLAS", 31: "FACT", 32: "PACK", 33: "NAVM", 34: "WOOP", 35: "MGEF",
    36: "SMQN", 37: "SCEN", 38: "LCTN", 39: "RELA", 40: "PHZD", 41: "PBAR",
    42: "PCON", 43: "FLST", 44: "LVLN", 45: "LVLI", 46: "LVSP", 47: "PARW",
    48: "ENCH",
}

# Global data table 1/2/3 type ids -> human name (UESP).
GLOBAL_TYPES = {
    0: "MiscStats", 1: "PlayerLocation", 2: "TES", 3: "GlobalVariables",
    4: "CreatedObjects", 5: "Effects", 6: "Weather", 7: "Audio", 8: "SkyCells",
    100: "ProcessLists", 101: "Combat", 102: "Interface", 103: "ActorCauses",
    104: "Unknown104", 105: "DetectionManager", 106: "LocationMetaData",
    107: "QuestStaticData", 108: "StoryTeller", 109: "MagicFavorites",
    110: "PlayerControls", 111: "StoryEventManager", 112: "IngredientShared",
    113: "MenuControls", 114: "MenuTopicManager", 1000: "TempEffects",
    1001: "Papyrus", 1002: "AnimObjects", 1003: "Timer",
    1004: "SynchronizedAnimations", 1005: "Main",
}


class SaveError(Exception):
    pass


class Reader:
    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size
        self.pos = 0

    def close(self):
        self.f.close()

    def at(self, off: int):
        if off < 0 or off > self.size:
            raise SaveError(f"offset {off:#x} outside file ({self.size:#x})")
        self.f.seek(off)
        self.pos = off

    def take(self, n: int) -> bytes:
        if n < 0 or self.pos + n > self.size:
            raise SaveError(f"truncated read {n} bytes at {self.pos:#x}")
        data = self.f.read(n)
        if len(data) != n:
            raise SaveError(f"short read {len(data)}/{n} at {self.pos:#x}")
        self.pos += n
        return data

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def f32(self) -> float:
        return struct.unpack("<f", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def skip(self, n: int):
        if n < 0 or self.pos + n > self.size:
            raise SaveError(f"truncated skip {n} at {self.pos:#x}")
        self.pos += n
        self.f.seek(self.pos)

    def string(self) -> str:
        """Length-prefixed byte string (UESP calls it wstring; TESV LE stores
        one byte per character, length is the byte count)."""
        n = self.u16()
        if n > 4096:
            raise SaveError(f"implausible string length {n} at {self.pos - 2:#x}")
        return self.take(n).decode("latin-1")


def _parse_plugin_block(block: bytes) -> tuple[int, list[str], int]:
    """pluginInfo: uint8 count, then count * (uint16 len + bytes).  The outer
    pluginInfoSize includes the count byte."""
    if not block:
        raise SaveError("empty plugin info block")
    count = block[0]
    p = 1
    names = []
    for _ in range(count):
        if p + 2 > len(block):
            raise SaveError("plugin info truncated at name length")
        n = struct.unpack_from("<H", block, p)[0]
        p += 2
        if p + n > len(block):
            raise SaveError("plugin info truncated at name")
        names.append(block[p:p + n].decode("latin-1"))
        p += n
    return count, names, len(block) - p


def _parse_global_table(r: Reader, off: int, count: int, limit: int | None):
    r.at(off)
    records = []
    for i in range(count):
        if limit is not None and i >= limit:
            break
        rtype = r.u32()
        length = r.u32()
        if length > r.size - r.pos:
            raise SaveError(
                f"global data type {rtype} length {length} at {r.pos - 4:#x} "
                f"exceeds file")
        # Global data payloads are not compressed in LE; they are zlib only
        # inside individual structured payloads, which the guest walks.
        r.skip(length)
        records.append({"type": rtype,
                        "name": GLOBAL_TYPES.get(rtype, f"type{rtype}"),
                        "length": length})
    return records


def _read_len(r: Reader, width: int) -> int:
    if width == 1:
        return r.u8()
    if width == 2:
        return r.u16()
    if width == 4:
        return r.u32()
    raise SaveError(f"unsupported change-form length width {width}")


def parse_save(path: str, limit_changes: int | None = None,
               inflate: bool = True) -> dict:
    r = Reader(path)
    try:
        if r.take(13) != MAGIC:
            raise SaveError("bad magic (expected TESV_SAVEGAME)")
        header_size = r.u32()
        version = r.u32()
        save_number = r.u32()
        if version == 12:
            raise SaveError(
                "Skyrim SE save (version 12) is not supported; this tool "
                "validates LE saves (version 7-9)")
        if version not in (7, 8, 9):
            raise SaveError(f"unsupported save version {version}")

        player = {
            "name": r.string(),
            "level": r.u32(),
            "location": r.string(),
            "gameDate": r.string(),
            "race": r.string(),
            "sex": r.u16(),
            "curExp": r.f32(),
            "lvlUpExp": r.f32(),
            "filetime": r.u64(),
        }
        shot_w = r.u32()
        shot_h = r.u32()
        if version >= 12:
            shot_bpp = 4  # SE: BGRA
            compression_type = r.u16()
        else:
            shot_bpp = 3  # LE: RGB
            compression_type = 0
        if shot_w > 4096 or shot_h > 4096:
            raise SaveError(f"implausible screenshot {shot_w}x{shot_h}")
        shot_bytes = shot_w * shot_h * shot_bpp
        if shot_bytes > r.size - r.pos:
            raise SaveError("screenshot exceeds file")
        r.skip(shot_bytes)

        form_version = r.u8()
        plugin_info_size = r.u32()
        if plugin_info_size > r.size - r.pos:
            raise SaveError("plugin info exceeds file")
        plugin_block = r.take(plugin_info_size)
        plugin_count, plugins, plugin_trailing = _parse_plugin_block(plugin_block)

        light_plugins: list[str] = []
        if version >= 12 and form_version >= 78:
            light_count = r.u16()
            for _ in range(light_count):
                light_plugins.append(r.string())

        table_off = r.pos
        if table_off + 100 > r.size:
            raise SaveError("file location table truncated")
        fields = [r.u32() for _ in range(10)]
        r.skip(15 * 4)
        (formid_count_off, unknown3_off, gdt1_off, gdt2_off, change_off,
         gdt3_off, gdt1_count, gdt2_count, gdt3_count,
         change_count) = fields

        # Offsets are absolute.  A valid save has data chunks in this order,
        # but we only require that each lies inside the file and after the
        # table; the engine itself trusts them.
        region_offsets = {
            "globalDataTable1": (gdt1_off, gdt1_count),
            "globalDataTable2": (gdt2_off, gdt2_count),
            "changeForms": (change_off, change_count),
            "globalDataTable3": (gdt3_off, gdt3_count),
            "formIDArray": (formid_count_off, None),
            "unknown3Table": (unknown3_off, None),
        }
        for name, (off, _) in region_offsets.items():
            if off < table_off + 100 or off > r.size:
                raise SaveError(f"{name} offset {off:#x} outside data area")

        gdt1 = _parse_global_table(r, gdt1_off, gdt1_count, None)
        gdt2 = _parse_global_table(r, gdt2_off, gdt2_count, None)
        gdt3 = _parse_global_table(r, gdt3_off, gdt3_count, None)

        # Change forms.  Layout: RefID(3) + changeFlags(4) + type(1) +
        # version(1) + length1[width] + length2[width] + data[length1].
        # length2 != 0 means zlib; the stream must inflate to length2 bytes.
        r.at(change_off)
        change_types: dict[str, int] = {}
        compressed = 0
        compressed_in = 0
        compressed_out = 0
        inflated_ok = 0
        change_errors: list[str] = []
        parsed_changes = 0
        for i in range(change_count):
            if limit_changes is not None and i >= limit_changes:
                break
            if r.pos + 11 > r.size:
                raise SaveError(f"change form {i} header truncated")
            r.skip(3)                    # RefID
            r.u32()                      # changeFlags
            type_byte = r.u8()
            r.u8()                       # version
            width = 1 << (type_byte >> 6)
            if width == 8:
                raise SaveError(f"change form {i}: 8-byte lengths unsupported")
            length1 = _read_len(r, width)
            length2 = _read_len(r, width)
            if length1 > r.size - r.pos:
                raise SaveError(f"change form {i}: data exceeds file")
            payload = r.take(length1)
            parsed_changes += 1
            sig = CHANGE_FORM_TYPES.get(type_byte & 0x3F,
                                         f"type{type_byte & 0x3F}")
            change_types[sig] = change_types.get(sig, 0) + 1
            if length2:
                compressed += 1
                compressed_in += length1
                compressed_out += length2
                if inflate:
                    try:
                        raw = zlib.decompress(payload)
                    except zlib.error as exc:
                        change_errors.append(
                            f"change form {i} zlib: {exc}")
                        continue
                    if len(raw) != length2:
                        change_errors.append(
                            f"change form {i} inflate {len(raw)} != {length2}")
                        continue
                inflated_ok += 1
            elif length1 != 0 and inflate:
                # Not compressed; nothing to prove beyond the length.
                pass

        # formID array and tail tables.
        formid_count = 0
        visited_worldspace_count = 0
        unknown3_size = 0
        if formid_count_off + 4 <= r.size:
            r.at(formid_count_off)
            formid_count = r.u32()
            if formid_count > (r.size - r.pos) // 4:
                raise SaveError(f"formIDArray count {formid_count} exceeds file")
            after_formids = r.pos + 4 * formid_count
            if after_formids + 4 <= r.size:
                r.at(after_formids)
                visited_worldspace_count = r.u32()
                if visited_worldspace_count > (r.size - r.pos) // 4:
                    raise SaveError("visitedWorldspaceArray count exceeds file")
        if unknown3_off + 4 <= r.size:
            r.at(unknown3_off)
            unknown3_size = r.u32()
            if unknown3_size > r.size - r.pos:
                raise SaveError("unknown3Table exceeds file")

        result = {
            "path": path,
            "size": r.size,
            "headerSize": header_size,
            "version": version,
            "saveNumber": save_number,
            "player": player,
            "screenshot": {"width": shot_w, "height": shot_h,
                           "bpp": shot_bpp, "bytes": shot_bytes},
            "compressionType": compression_type,
            "formVersion": form_version,
            "pluginInfoSize": plugin_info_size,
            "pluginCount": plugin_count,
            "plugins": plugins,
            "pluginTrailingBytes": plugin_trailing,
            "lightPlugins": light_plugins,
            "fileLocationTable": {
                "offset": table_off,
                "formIDArrayCountOffset": formid_count_off,
                "unknownTable3Offset": unknown3_off,
                "globalDataTable1Offset": gdt1_off,
                "globalDataTable2Offset": gdt2_off,
                "changeFormsOffset": change_off,
                "globalDataTable3Offset": gdt3_off,
                "globalDataTable1Count": gdt1_count,
                "globalDataTable2Count": gdt2_count,
                "globalDataTable3Count": gdt3_count,
                "changeFormCount": change_count,
            },
            "globalData": {
                "table1": gdt1,
                "table2": gdt2,
                "table3": gdt3,
            },
            "changes": {
                "declared": change_count,
                "parsed": parsed_changes,
                "compressed": compressed,
                "compressedBytes": compressed_in,
                "uncompressedBytes": compressed_out,
                "inflatedOk": inflated_ok,
                "types": dict(sorted(change_types.items(),
                                     key=lambda kv: (-kv[1], kv[0]))),
                "errors": change_errors,
            },
            "formIDArrayCount": formid_count,
            "visitedWorldspaceCount": visited_worldspace_count,
            "unknown3TableSize": unknown3_size,
        }
        if limit_changes is not None or not inflate:
            result["partial"] = True
        return result
    finally:
        r.close()


def validate(result: dict, check_inflate: bool = True) -> list[str]:
    """Structural checks that do not depend on the guest implementation."""
    problems: list[str] = []
    if not result["plugins"]:
        problems.append("no plugins")
    if result["plugins"] and result["plugins"][0].lower() != "skyrim.esm":
        problems.append("first plugin is not Skyrim.esm")
    if result["pluginTrailingBytes"]:
        problems.append(
            f"plugin info has {result['pluginTrailingBytes']} trailing bytes")
    if result["screenshot"]["bytes"] <= 0:
        problems.append("empty screenshot")
    if result["fileLocationTable"]["changeFormsOffset"] <= \
            result["fileLocationTable"]["offset"]:
        problems.append("change forms before file location table")
    if check_inflate:
        problems.extend(result["changes"]["errors"][:10])
        if result["changes"]["errors"]:
            problems.append(
                f"{len(result['changes']['errors'])} change-form inflate errors")
    return problems


def summary_line(result: dict) -> str:
    shot = result["screenshot"]
    ch = result["changes"]
    top = ", ".join(f"{k}:{v}" for k, v in list(ch["types"].items())[:6])
    return (
        f"{os.path.basename(result['path'])}\n"
        f"  version={result['version']} headerSize={result['headerSize']} "
        f"formVersion={result['formVersion']} size={result['size']} B\n"
        f"  player={result['player']['name']!r} level={result['player']['level']} "
        f"race={result['player']['race']!r} location={result['player']['location']!r}\n"
        f"  screenshot={shot['width']}x{shot['height']} bpp={shot['bpp']} "
        f"({shot['bytes']} B)\n"
        f"  plugins={result['pluginCount']} first={result['plugins'][:3]}\n"
        f"  globals: t1={len(result['globalData']['table1'])} "
        f"t2={len(result['globalData']['table2'])} "
        f"t3={len(result['globalData']['table3'])}\n"
        f"  changeForms declared={ch['declared']} parsed={ch['parsed']} "
        f"compressed={ch['compressed']} "
        f"inflated={ch['inflatedOk']} "
        f"in/out={ch['compressedBytes']}/{ch['uncompressedBytes']} "
        f"types=[{top}]\n"
        f"  formIDs={result['formIDArrayCount']} "
        f"visitedWorldspaces={result['visitedWorldspaceCount']} "
        f"unknown3={result['unknown3TableSize']} B"
    )


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Validate Skyrim .ess saves")
    ap.add_argument("files", nargs="*")
    ap.add_argument("--dir", help="directory to scan for *.ess")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--limit-changes", type=int, default=None,
                    help="parse only the first N change forms")
    ap.add_argument("--no-inflate", action="store_true",
                    help="record compressed sizes but do not zlib-inflate")
    args = ap.parse_args(argv)

    paths = list(args.files)
    if args.dir:
        for name in sorted(os.listdir(args.dir)):
            if name.lower().endswith(".ess"):
                paths.append(os.path.join(args.dir, name))
    if not paths:
        ap.error("no .ess files (pass paths or --dir)")

    failures = 0
    results = []
    for path in paths:
        try:
            result = parse_save(path, limit_changes=args.limit_changes,
                                inflate=not args.no_inflate)
        except (SaveError, OSError) as exc:
            print(f"FAIL {path}: {exc}", file=sys.stderr)
            failures += 1
            continue
        problems = validate(result)
        result["problems"] = problems
        results.append(result)
        if not args.json:
            print(summary_line(result))
            if problems:
                for p in problems:
                    print(f"  PROBLEM: {p}")
            print()
        if problems:
            failures += 1
    if args.json:
        print(json.dumps(results if len(results) != 1 else results[0],
                         indent=2, default=str))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
