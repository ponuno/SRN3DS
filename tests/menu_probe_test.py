#!/usr/bin/env python3
"""Regression for tools/menu_probe.py on a synthetic slice log.

Run directly or through tools/run_host_tests.ps1.
"""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CANDIDATES = [
    os.path.join(HERE, "..", "tools"),
    os.path.join(os.getcwd(), "tools"),
    os.path.join(HERE, "..", "..", "menu-gfx", "tools"),
]
TOOL_PATH = next(
    (p for p in CANDIDATES if os.path.isfile(os.path.join(p, "menu_probe.py"))),
    CANDIDATES[0],
)
try:
    sys.path.insert(0, os.path.abspath(TOOL_PATH))
    import menu_probe  # noqa: E402
except ModuleNotFoundError:  # msys python may not honour the mapped path
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "menu_probe", os.path.join(TOOL_PATH, "menu_probe.py")
    )
    menu_probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(menu_probe)

LOG = """\
[filetrace] open 'C:\\Skyrim\\DATA\\Skyrim - Interface.bsa' host='F:/Skyrim/DATA/Skyrim - Interface.bsa' handle=103 size=34708888 caller=00a5851a
[filetrace] read h=103 off=0 n=65536 got=65536 caller=00a580e2 path=F:/Skyrim/DATA/Skyrim - Interface.bsa
[filetrace] read h=103 off=34668544 n=65536 got=40344 caller=00a580e2 path=F:/Skyrim/DATA/Skyrim - Interface.bsa
[filetrace] open FAIL 'C:\\Skyrim\\DATA\\Missing.swf' host='F:/Skyrim/DATA/Missing.swf' errno=2 caller=00a5ed20
[menutrace] BSScaleformFileOpener(0xa5ed20) url='Interface\\StartMenu.swf' caller=00a60c37
[drawprobe] SetIndices caller=00f874c0 wr=00f874e1 this=50007340 m28=0 arg0=00000000 gate=00 a1=00000002 a2=50007660 a3=00000000 a4=00000000 a5=00000000
[drawprobe] SetStreamSourceFreq caller=00f874c0 wr=00f874e1 this=50007340 m28=0 arg0=00000000 gate=00 a1=00000001 a2=00000001 a3=00000000 a4=00000000 a5=00000000
[d3d9-trace] IDirect3DDevice9::BeginScene slot=41 this=0x50007000 args=[0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000 0x00000000]
[d3d9-trace] IDirect3DDevice9::Present slot=17 this=0x50007000
[d3d9-trace] IDirect3DDevice9::DrawPrimitive slot=81 this=0x50007000
[sync] Sleep 25 ms from 006f40cf thread=11
first fault: Guest fault at 0802fa80: unknown indirect function
[guest] ExitProcess(0) from 0069107d
"""


class MenuProbeTest(unittest.TestCase):
    def test_parse_and_report(self):
        result = menu_probe.parse(LOG.splitlines())
        self.assertEqual(result["presents"], 1)
        self.assertEqual(result["draws"]["DrawPrimitive"], 1)
        self.assertEqual(result["wrapper"]["calls"], 2)
        self.assertEqual(result["wrapper"]["gates"], {"00": 2})
        self.assertEqual(result["opener_urls"], ["Interface\\StartMenu.swf"])
        self.assertEqual(len(result["opens"]), 1)
        self.assertEqual(result["opens"][0]["size"], 34708888)
        self.assertEqual(len(result["opens_failed"]), 1)
        self.assertIn("Interface.bsa", next(iter(result["bsa_ranges"])))
        lo, hi = next(iter(result["bsa_ranges"].values()))
        self.assertEqual((lo, hi), (0, 34668544 + 40344))
        self.assertEqual(result["first_fault"], "Guest fault at 0802fa80: unknown indirect function")
        text = menu_probe.report(result, "synthetic.log")
        self.assertIn("presents           : 1", text)
        self.assertIn("StartMenu.swf", text)

    def test_json_roundtrip(self):
        with tempfile.TemporaryDirectory() as td:
            log = os.path.join(td, "run.log")
            out = os.path.join(td, "run.json")
            with open(log, "w", encoding="utf-8") as f:
                f.write(LOG)
            menu_probe.main([log, "--json", out])
            with open(out, encoding="utf-8") as f:
                data = json.load(f)
            self.assertEqual(data["presents"], 1)
            self.assertEqual(data["wrapper"]["calls"], 2)


if __name__ == "__main__":
    unittest.main()
