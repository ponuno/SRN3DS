#!/usr/bin/env python3
"""Tests for the generated runtime closure header.

These tests do not need the retail archives.  They prove that the checked-in
generated header is current and that a silently truncated or weakened manifest
cannot produce an integration header.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT if (ROOT / "source" / "d3d9_com.c").exists() else Path(
    r"C:\Users\vasya\Documents\Skyrim_N3DS")
PYTHON = str(REPO / ".venv" / "Scripts" / "python.exe")
if not Path(PYTHON).exists():
    PYTHON = sys.executable
GENERATOR = ROOT / "tools" / "gen_menu_closure_manifest.py"
_src_header = ROOT / "source" / "d3d9_menu_closure_manifest.h"
HEADER = _src_header if _src_header.exists() else (
    ROOT / "runtime" / "d3d9_menu_closure_manifest.h")
MANIFEST = ROOT / "tools" / "menu_closure_manifest.json"


def run_generator(manifest: Path, out: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [PYTHON, str(GENERATOR), "--manifest", str(manifest), "--out", str(out)],
        capture_output=True, text=True)


class GeneratedHeaderTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads(MANIFEST.read_text())

    def test_header_is_current(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "generated.h"
            proc = run_generator(MANIFEST, out)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(out.read_text(), HEADER.read_text(),
                             "runtime header is stale; rerun the generator")

    def test_manifest_digest_is_pinned(self):
        digest = hashlib.sha256(MANIFEST.read_bytes()).hexdigest()
        text = HEADER.read_text()
        self.assertIn('D9_MENU_CLOSURE_MANIFEST_SHA256 "%s"' % digest, text)

    def test_logo_entry_identity_is_exact(self):
        text = HEADER.read_text()
        self.assertIn('.key_known = 1u, .key = 0x01200005u', text)
        self.assertIn(".chunk_bytes = 852u", text)
        self.assertIn(".sha256 = d9_menu_sha_0", text)
        self.assertIn("0x87, 0x25, 0x39, 0x3b", text)

    def test_missing_required_entry_is_rejected(self):
        manifest = dict(self.manifest)
        manifest["entries"] = [e for e in self.manifest["entries"]
                               if e["id"] != "menu.composite.ps"]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text(json.dumps(manifest))
            proc = run_generator(path, Path(tmp) / "out.h")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("missing required closure entries", proc.stderr)

    def test_required_false_is_rejected(self):
        manifest = json.loads(MANIFEST.read_text())
        for entry in manifest["entries"]:
            if entry["id"] == "menu.logo.ps":
                entry["required"] = False
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text(json.dumps(manifest))
            proc = run_generator(path, Path(tmp) / "out.h")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("must stay required:true", proc.stderr)

    def test_missing_fixed_state_is_rejected(self):
        manifest = json.loads(MANIFEST.read_text())
        for entry in manifest["entries"]:
            if entry["id"] == "menu.fog.fixed":
                del entry["state"]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text(json.dumps(manifest))
            proc = run_generator(path, Path(tmp) / "out.h")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("state contract", proc.stderr)

    def test_bad_sha_length_is_rejected(self):
        manifest = json.loads(MANIFEST.read_text())
        for entry in manifest["entries"]:
            if entry["id"] == "menu.logo.ps":
                entry["sha256"] = "00" * 31
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.json"
            path.write_text(json.dumps(manifest))
            proc = run_generator(path, Path(tmp) / "out.h")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("SHA-256", proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
