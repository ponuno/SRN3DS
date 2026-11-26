#!/usr/bin/env python3
"""Read-only map of TESV's real StartMenu path (not the SWF reimplementation).

The tool decodes only PE headers plus a handful of byte ranges.  It never
writes to the executable, never runs the guest and makes no render claim.

Usage:
    python tools/menu_harness.py --exe F:\\Skyrim\\TESV.exe
    python tools/menu_harness.py --exe F:\\Skyrim\\TESV.exe --json
    python tools/menu_harness.py --self-test

Exit status is 0 when every checked signature matches and non-zero on the first
mismatch.  The self-test uses a synthetic PE so it runs without game bytes.
"""

import argparse
import json
import struct
import sys
import tempfile


# --------------------------------------------------------------------------
# Minimal PE reader (stdlib only, bounded reads)
# --------------------------------------------------------------------------
class Pe:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        head = self.f.read(0x400)
        if head[:2] != b"MZ":
            raise ValueError("not an MZ file")
        e = struct.unpack_from("<I", head, 0x3C)[0]
        if head[e:e + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsec = struct.unpack_from("<H", head, e + 6)[0]
        opt_size = struct.unpack_from("<H", head, e + 20)[0]
        self.base = struct.unpack_from("<I", head, e + 0x34)[0]
        self.entry = self.base + struct.unpack_from("<I", head, e + 0x28)[0]
        off = e + 24 + opt_size
        self.f.seek(off)
        sec_table = self.f.read(40 * nsec)
        self.sections = []
        for i in range(nsec):
            name = sec_table[i * 40:i * 40 + 8].rstrip(b"\0").decode("latin-1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from(
                "<IIII", sec_table, i * 40 + 8)
            self.sections.append({
                "name": name,
                "va": self.base + vaddr,
                "vsize": vsize,
                "rawptr": rawptr,
                "rawsize": rawsize,
            })

    def read_va(self, va, size):
        for s in self.sections:
            span = max(s["vsize"], s["rawsize"])
            if s["va"] <= va < s["va"] + span:
                self.f.seek(s["rawptr"] + (va - s["va"]))
                return self.f.read(size)
        raise ValueError("VA 0x%08x is outside every section" % va)

    def read_cstr(self, va, limit=160):
        raw = self.read_va(va, limit)
        return raw.split(b"\0", 1)[0].decode("latin-1")

    def close(self):
        self.f.close()


# --------------------------------------------------------------------------
# Measured StartMenu anchors (TESV 1.9.32.0.8, SHA256 d97eb0cc...f5e605)
# --------------------------------------------------------------------------
ANCHORS = [
    # name, VA, expected bytes, what it proves
    ("winmain.intro_setting", 0x69DADF,
     "a1382b27013bc374083818740433c0eb05b801000000",
     "reads [0x1272b38] = sIntroSequence:General; empty means skip"),
    ("winmain.open_main_menu", 0x69DE08,
     "6a01e851911d00",
     "push 1; call 0x876f60 -> open the Main Menu"),
    ("mainmenu.show", 0x876F60,
     "833d98fcb30100754f",
     "cmp [0x1b3fc98],0 -> Main Menu singleton"),
    ("mainmenu.ctor", 0x876B90,
     "53568bf1e8d7df1e00",
     "Main Menu object constructor (name at +8 = \"StartMenu\")"),
    ("mainmenu.ctor_createmovie", 0x876BC0,
     "8b0db0e9b20156e8b4a01e00",
     "call 0xa60c80 CreateMovie with loader 0x1b2e9b0"),
    ("menu.movie_path", 0xA60C10,
     "568b74240c578b7c240c5768 4ce21001".replace(" ", ""),
     "builds \"Interface/%s.swf\" (0x110e24c)"),
    ("gfx.createmovie", 0xA60C80,
     "81ec5c010000a154ebbb01",
     "Menu::CreateMovie body"),
    ("menu_type.register", 0x100F0E0,
     "a148352e0185c0750fb960e3b301e83deda4ffa348352e01",
     "static init registers \"Main Menu\" (0x12b3a94) with factory 0x876fc0"),
]

STRINGS = [
    ("menu.movie_name", 0x10E67AC, "StartMenu"),
    ("menu.registered_name", 0x12B3A94, "Main Menu"),
    ("movie.path_format", 0x110E24C, "Interface/%s.swf"),
    ("ini.intro_sequence", 0x10CD570, "sIntroSequence:General"),
    ("ini.intro_file", 0x10CD560, "BGS_LOGO.BIK"),
]

# Objects whose constructor/factory calls are the next observable milestones.
MILESTONES = [
    ("main_menu_factory", 0x876FC0, "allocates 0x24 and calls the ctor"),
    ("main_menu_singleton", 0x1B3FC98, "non-NULL after 0x876f60(1)"),
    ("startmenu_movie", 0x1B2E9B0, "GFx loader used by CreateMovie"),
    ("menu_manager", 0x1B10DCC, "MenuManager singleton"),
    ("main_menu_type", 0x12E3548, "registered menu-type table"),
    ("gfx_registry", 0x12B3AD0, "GFxValue/global used by the menu scripts"),
]


def verify_pe(pe, anchors=ANCHORS, strings=STRINGS):
    """Return (ok, rows).  Rows are (name, ok, detail)."""
    rows = []
    ok_all = True
    for name, va, want, note in anchors:
        try:
            got = pe.read_va(va, len(want) // 2).hex()
        except ValueError as exc:
            rows.append((name, False, str(exc)))
            ok_all = False
            continue
        ok = got == want
        ok_all = ok_all and ok
        rows.append((name, ok, "0x%08x %s %s" % (va, got, note)))
    for name, va, want in strings:
        try:
            got = pe.read_cstr(va)
        except ValueError as exc:
            rows.append((name, False, str(exc)))
            ok_all = False
            continue
        ok = got == want
        ok_all = ok_all and ok
        rows.append((name, ok, "0x%08x %r" % (va, got)))
    return ok_all, rows


class SparsePe:
    """In-memory stand-in with the same read interface as Pe."""

    def __init__(self, blocks):
        self.blocks = blocks
        self.base = 0x400000
        self.entry = 0x401000

    def read_va(self, va, size):
        for (start, blob) in self.blocks:
            if start <= va and va + size <= start + len(blob):
                return blob[va - start:va - start + size]
        raise ValueError("VA 0x%08x is outside every section" % va)

    def read_cstr(self, va, limit=160):
        raw = self.read_va(va, limit)
        return raw.split(b"\0", 1)[0].decode("latin-1")


def self_test():
    """Feed every anchor through the verifier and require one corruption hit."""
    blocks = []
    for _name, va, want, _note in ANCHORS:
        blocks.append((va, bytes.fromhex(want)))
    for _name, va, want in STRINGS:
        raw = want.encode("latin-1") + b"\0"
        blocks.append((va, raw + b"\0" * (160 - len(raw))))
    pe = SparsePe(blocks)
    ok, rows = verify_pe(pe)
    if not ok:
        for name, good, detail in rows:
            if not good:
                print("self-test failed:", name, detail)
        raise SystemExit(1)
    bad = list(blocks)
    name, va, want, _note = ANCHORS[0]
    blob = bytearray(bytes.fromhex(want))
    blob[0] ^= 0xFF
    bad[0] = (va, bytes(blob))
    ok2, _rows = verify_pe(SparsePe(bad))
    if ok2:
        print("self-test failed: corrupted signature was accepted")
        raise SystemExit(1)
    return len(ANCHORS) + len(STRINGS)


HARNESS = {
    "ini": {
        "path": "<game root>/Skyrim.ini",
        "section": "General",
        "values": {
            "sIntroSequence": "",
            "bPreloadIntroSequence": "0",
        },
        "effect": "0x69dadf sees an empty sIntroSequence, sets byte 0x1b3fced "
                  "and skips the 0xf500f0 Bink playback at 0x69db7c",
    },
    "measurement_env": [
        "TESV_D3D9_TRACE=1",
        "TESV_D3D9_CAPTURE_DRAWS=8",
        "TESV_D3D9_CAPS_PROBE=1",
        "TESV_D3D9_SM3_GATE_PROBE=1",
        "TESV_D3D9_DEPTH_PAIR_PROBE=1",
        "TESV_D3D9_HDR_LDR_FALLBACK=1",
        "TESV_D3D9_SHADER_SKIP_PROBE=1",
        "TESV_D3D9_BUDGET_MB=92",
        "N3DS_IRC_PATH=F:\\Skyrim\\ir\\tesv.irc",
    ],
    "not_needed": (
        "no world/load-game skip: WinMain calls 0x876f60(1) automatically when "
        "0x6982c0 returns \"no startup save\" (normal launch); no input is "
        "required to open the menu or to produce its first GFx frame"
    ),
}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", default=r"F:\Skyrim\TESV.exe")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        n = self_test()
        print("menu_harness self-test: %d checks PASS" % n)
        return 0
    pe = Pe(args.exe)
    try:
        ok, rows = verify_pe(pe)
        result = {
            "exe": args.exe,
            "base": pe.base,
            "entry": pe.entry,
            "ok": ok,
            "checks": [{"name": n, "ok": o, "detail": d} for n, o, d in rows],
            "milestones": [{"name": n, "va": v, "role": r}
                           for n, v, r in MILESTONES],
            "harness": HARNESS,
        }
    finally:
        pe.close()
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("exe: %s  base 0x%08x  entry 0x%08x" % (args.exe, pe.base, pe.entry))
        for name, good, detail in rows:
            print("  %-26s %s  %s" % (name, "PASS" if good else "FAIL", detail))
        print("\nopen path:")
        for name, va, role in MILESTONES:
            print("  0x%08x  %-18s %s" % (va, name, role))
        print("\nharness:")
        for k, v in HARNESS["ini"]["values"].items():
            print("  [General] %s=%s" % (k, v))
        for e in HARNESS["measurement_env"]:
            print("  " + e)
        print("  " + HARNESS["not_needed"])
        print("\nresult: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
