#!/usr/bin/env python3
"""Dependency-free tests for tools/pex_dump.py (synthetic Skyrim PEX 3.2).

Run from the repository root:
    python tests/pex_dump_test.py
    python tests/pex_dump_test.py --real /path/to/Script.pex [--real ...]
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "pex_dump.py"
SPEC = importlib.util.spec_from_file_location("pex_dump", TOOL)
pex_dump = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = pex_dump
SPEC.loader.exec_module(pex_dump)


def u16(value: int) -> bytes:
    return struct.pack(">H", value)


def u32(value: int) -> bytes:
    return struct.pack(">I", value)


def i32(value: int) -> bytes:
    return struct.pack(">i", value)


def u64(value: int) -> bytes:
    return struct.pack(">Q", value)


def string(value: str) -> bytes:
    raw = value.encode("utf-8")
    return u16(len(raw)) + raw


def make_pex(code: bytes | None = None) -> bytes:
    default_code = code is None
    strings = ["", "DemoScript", "ActiveMagicEffect", "Main", "value", "Int", "OnInit", "None"]
    if code is None:
        # nop; assign value = 42; return None
        code = b"\x00" + b"\x0d\x01" + u16(4) + b"\x03" + i32(42) + b"\x1a\x00"
    out = bytearray(pex_dump.SKYRIM_MAGIC)
    out += bytes((3, 2)) + u16(1) + u64(1_700_000_000)
    out += string("DemoScript.psc") + string("tester") + string("synthetic")
    out += u16(len(strings)) + b"".join(string(s) for s in strings)
    # One debug function, one source line per instruction in the default body.
    line_count = 3 if default_code else 1
    out += b"\x01" + u64(1_700_000_100) + u16(1)
    out += u16(1) + u16(3) + u16(6) + b"\x00" + u16(line_count)
    out += b"".join(u16(10 + i) for i in range(line_count))
    out += u16(0)  # user flag definitions
    out += u16(1)  # objects
    out += u16(1) + u32(0) + u16(2) + u16(0) + u32(0) + u16(3)
    out += u16(1) + u16(4) + u16(5) + u32(0) + b"\x00"  # variable value:Int = None
    out += u16(1) + u16(4) + u16(5) + u16(0) + u32(0) + b"\x04" + u16(4)  # auto property
    out += u16(1) + u16(3) + u16(1) + u16(6)  # state Main, function OnInit
    out += u16(7) + u16(0) + u32(0) + b"\x00"  # return None, doc, user flags, flags
    out += u16(0) + u16(0)  # params, locals
    out += u16(3 if default_code else 1) + code
    return bytes(out)


class PexDumpTests(unittest.TestCase):
    def test_header_strings_debug_objects_functions_and_opcodes(self):
        pex = pex_dump.parse_pex(make_pex(), "synthetic.pex")
        self.assertEqual((pex.major_version, pex.minor_version, pex.game_id), (3, 2, 1))
        self.assertEqual(pex.source_name, "DemoScript.psc")
        self.assertEqual(pex.strings[1], "DemoScript")
        self.assertEqual(len(pex.debug_functions), 1)
        self.assertEqual(pex.debug_functions[0].line_numbers, [10, 11, 12])
        self.assertEqual(len(pex.objects), 1)
        obj = pex.objects[0]
        self.assertEqual((obj.name, obj.parent_class, obj.auto_state_name),
                         ("DemoScript", "ActiveMagicEffect", "Main"))
        self.assertEqual(obj.variables[0].default_value.type, "none")
        self.assertEqual(obj.properties[0].auto_var_name, "value")
        self.assertEqual(len(obj.functions), 1)
        fn = obj.functions[0]
        self.assertEqual(fn.qualified_name, "DemoScript.Main.OnInit")
        self.assertEqual(fn.debug_lines, [10, 11, 12])
        self.assertEqual([i.name for i in fn.instructions], ["nop", "assign", "return"])
        self.assertEqual([i.offset for i in fn.instructions], [0, 1, 10])
        self.assertEqual(fn.instructions[1].args[0].display(pex.strings), "value[s4]")
        self.assertEqual(fn.instructions[1].args[1].value, 42)
        self.assertEqual(pex.opcode_histogram, {0: 1, 0x0D: 1, 0x1A: 1})
        self.assertEqual(pex.trailing_bytes, 0)
        self.assertEqual(pex.warnings, [])

    def test_cli_json_includes_requested_sections(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "demo.pex"
            path.write_bytes(make_pex())
            proc = subprocess.run(
                [sys.executable, str(TOOL), str(path), "--json", "--strings",
                 "--functions", "--instructions"],
                capture_output=True, text=True, check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            payload = json.loads(proc.stdout)
            self.assertEqual(len(payload), 1)
            row = payload[0]
            self.assertEqual(row["version"], "3.2")
            self.assertEqual(row["string_count"], 8)
            self.assertEqual(row["object_count"], 1)
            self.assertEqual(row["function_count"], 1)
            self.assertEqual(row["instruction_count"], 3)
            self.assertEqual(row["functions"][0]["instructions"][1]["name"], "assign")

    def test_bad_magic_truncation_unknown_opcode_and_bad_varargs_fail(self):
        with self.assertRaises(pex_dump.PexError):
            pex_dump.parse_pex(b"NOPE" + make_pex()[4:])
        with self.assertRaises(pex_dump.PexError):
            pex_dump.parse_pex(make_pex()[:-1])
        with self.assertRaisesRegex(pex_dump.PexError, "unknown opcode 0xfe"):
            pex_dump.parse_pex(make_pex(b"\xfe"))
        # callmethod with three None fixed args and a String vararg count.
        with self.assertRaisesRegex(pex_dump.PexError, "vararg count is not an integer"):
            pex_dump.parse_pex(make_pex(b"\x17\x00\x00\x00\x02" + u16(0)))

    def test_trailing_bytes_are_reported_not_hidden(self):
        pex = pex_dump.parse_pex(make_pex() + b"tail")
        self.assertEqual(pex.trailing_bytes, 4)
        self.assertTrue(any("trailing" in warning for warning in pex.warnings))

    def test_string_index_is_bounds_checked(self):
        data = bytearray(make_pex())
        # Object name index is the first uint16 after string/debug/user-flag data.
        # Locate it through the known synthetic layout instead of a fragile literal.
        marker = u16(0) + u16(1)  # user-flag count, object count
        pos = data.index(marker, data.index(u16(len(["", "DemoScript"])))) + len(marker)
        data[pos:pos + 2] = u16(0xFFFF)
        with self.assertRaisesRegex(pex_dump.PexError, "outside string table"):
            pex_dump.parse_pex(bytes(data))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--real", action="append", default=[], type=Path,
                    help="also parse a real read-only .pex file (repeatable)")
    args = ap.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PexDumpTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        return 1
    for path in args.real:
        pex = pex_dump.load_pex(path)
        print(f"REAL PASS {path}: {pex.major_version}.{pex.minor_version} "
              f"objects={len(pex.objects)} functions={len(pex.functions)} "
              f"instructions={sum(pex.opcode_histogram.values())} warnings={len(pex.warnings)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
