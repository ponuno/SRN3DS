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
import hashlib
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
        self.import_rva = struct.unpack_from("<I", head, e + 24 + 104)[0]
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
# GFx's own D3D9 renderer creates its built-in shaders from three pointer
# tables in .data (caller 0xf8838c VS / 0xf883df PS).  The first table is
# always used; the third only when the device shader level is >= 3.
GFX_SHADER_TABLES = [
    ("gfx.vs", 0x12DFCA8, 6),
    ("gfx.ps.level2", 0x12DFCD8, 45),
    ("gfx.ps.level3", 0x12DFA80, 25),
]

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


# --------------------------------------------------------------------------
# Input path: the two device APIs the engine really imports.  The menu reads
# keyboard/mouse state through DirectInput8 (fed by win32_input.c from 3DS HID)
# and queries GetAsyncKeyState/GetKeyState/GetCursorPos on the same state.
# --------------------------------------------------------------------------
INPUT_IMPORTS = [
    ("DINPUT8.dll", "DirectInput8Create"),
    ("USER32.dll", "GetAsyncKeyState"),
    ("USER32.dll", "GetKeyState"),
    ("USER32.dll", "GetCursorPos"),
]


def read_imports(pe):
    """Map DLL name (upper case) -> set of imported function names."""
    out = {}
    if not getattr(pe, "import_rva", 0):
        return out
    va = pe.base + pe.import_rva
    for i in range(256):
        try:
            desc = pe.read_va(va + 20 * i, 20)
        except ValueError:
            break
        if not any(desc):
            break
        name_va = struct.unpack_from("<I", desc, 12)[0]
        if not name_va:
            break
        dll = pe.read_cstr(pe.base + name_va).upper()
        names = out.setdefault(dll, set())
        thunk = struct.unpack_from("<I", desc, 0)[0]
        if not thunk:
            thunk = struct.unpack_from("<I", desc, 16)[0]
        for k in range(4096):
            try:
                ent = struct.unpack_from("<I", pe.read_va(pe.base + thunk + 4 * k, 4))[0]
            except ValueError:
                break
            if ent == 0:
                break
            if ent & 0x80000000:
                names.add("#%d" % (ent & 0xFFFF))
                continue
            try:
                names.add(pe.read_cstr(pe.base + ent + 2))
            except ValueError:
                break
    return out


def verify_input_imports(pe):
    rows = []
    ok_all = True
    try:
        imports = read_imports(pe)
    except (ValueError, struct.error) as exc:
        return False, [("imports", False, str(exc))]
    for dll, name in INPUT_IMPORTS:
        ok = name in imports.get(dll.upper(), set())
        ok_all = ok_all and ok
        rows.append((("input.%s!%s" % (dll, name)), ok,
                     "import present" if ok else "import missing"))
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


def gfx_shaders(pe):
    """Return the GFx built-in shader tables with real sizes and SHA-256.

    The D3D9 blobs in this build carry the DWORD length in the high half of
    their second token, not the first (first = version/type).
    """
    rows = []
    for name, va, count in GFX_SHADER_TABLES:
        for i in range(count):
            ptr = struct.unpack("<I", pe.read_va(va + i * 4, 4))[0]
            if ptr < 0x1000:
                rows.append((name, i, ptr, 0, None))
                continue
            hdr = struct.unpack("<II", pe.read_va(ptr, 8))
            words = (hdr[1] >> 16) & 0xFFFF
            if 0 < words < 4096:
                digest = hashlib.sha256(pe.read_va(ptr, words * 4)).hexdigest()
            else:
                words, digest = 0, None
            rows.append((name, i, ptr, words * 4, digest))
    return rows


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
        "TESV_D3D9_SHADER_USAGE=1",
        "TESV_D3D9_BUDGET_MB=92",
        "TESV_CECDC0_SOURCE_FILL=1",
        "N3DS_IRC_PATH=F:\\Skyrim\\ir\\tesv.irc",
        "TESV_INPUT_PROFILE=menu",
    ],
    "input": {
        "profile_env": "TESV_INPUT_PROFILE=menu|legacy (default menu)",
        "runtime_api": "win32_input_set_profile() / win32_input_apply_pad()",
        "mapping": {
            "D-pad": "VK_UP/DOWN/LEFT/RIGHT plus W/S/A/D",
            "A": "VK_RETURN (0x0D); not a mouse click",
            "B": "VK_ESCAPE (0x1B)",
            "Start": "VK_ESCAPE (0x1B)",
            "Select": "VK_TAB (0x09)",
            "X": "VK_SPACE (0x20)",
            "Y": "VK_E (0x45)",
            "touch": "absolute cursor + left mouse button",
        },
        "guest_paths": [
            "dinput8.dll!DirectInput8Create -> IDirectInputDevice8::GetDeviceState "
            "(DIK_RETURN 0x1C, DIK_ESCAPE 0x01, DIK_TAB 0x0F, DIK_UP 0x48)",
            "user32.dll!GetAsyncKeyState / GetKeyState (IAT 0x0106b2bc / 0x0106b324)",
            "user32.dll!GetCursorPos (IAT 0x0106b328)",
        ],
        "not_claimed": "no menu frame was executed; mapping is host-tested only",
    },
    "slice_args": [
        "F:\\Skyrim\\TESV.exe", "<root>", "6000000000", "167772160",
    ],
    "note": "160 MiB guest budget is required for renderer init; 64 MiB OOMs "
            "at 549a9038. 2e9 blocks stop before the menu draw; 6e9 is the "
            "current run.",
    "not_needed": (
        "no world/load-game skip: WinMain calls 0x876f60(1) automatically when "
        "0x6982c0 returns \"no startup save\" (normal launch); no input is "
        "required to open the menu or to produce its first GFx frame"
    ),
}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", default=r"E:\skyr\TESV.exe")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--gfx-shaders", action="store_true",
                    help="print GFx's built-in VS/PS tables with hashes")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        n = self_test()
        print("menu_harness self-test: %d checks PASS" % n)
        return 0
    pe = Pe(args.exe)
    if args.gfx_shaders:
        try:
            for name, i, ptr, size, digest in gfx_shaders(pe):
                if not size:
                    print("%-14s %2d null/0x%08x" % (name, i, ptr))
                else:
                    print("%-14s %2d 0x%08x %4d B %s" %
                          (name, i, ptr, size, digest))
        finally:
            pe.close()
        return 0
    try:
        ok, rows = verify_pe(pe)
        iok, irows = verify_input_imports(pe)
        ok = ok and iok
        rows += irows
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
        print("  slice: " + " ".join(HARNESS["slice_args"]))
        print("  " + HARNESS["note"])
        print("  " + HARNESS["not_needed"])
        print("\ninput profile (%s):" % HARNESS["input"]["profile_env"])
        for k, v in HARNESS["input"]["mapping"].items():
            print("  %-8s -> %s" % (k, v))
        for e in HARNESS["input"]["guest_paths"]:
            print("  " + e)
        print("  " + HARNESS["input"]["not_claimed"])
        print("\nresult: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
