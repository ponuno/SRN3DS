"""Static check: Win32 and D3DX host-shim thunk windows must be disjoint.

A real bug this catches: win32_shell and win32_locale both used 0xff030000, so
every shell32 call was dispatched to a locale handler (win32_lookup checks
locale first). The collision was invisible in tests that only exercised one
module. The same class of bug appeared when the D3DX texture and surface shim
modules both used 0xfd000000: individually green, but in one TESV slice the
later module's entries replaced the earlier module's.

The check parses the `#define WIN32_*_BASE ...` constants (the simple forms the
project uses) and `#define D3DX_*_BASE ...` host windows, then fails on a
duplicate, a window that overlaps another 64 KiB slot, or a base outside its
window.
"""
import re
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "source"
WINDOW_LO = 0xFF000000
WINDOW_HI = 0xFF100000
THUNK_BASE = 0xFF000000
HOST_LO = 0xFC000000
HOST_HI = 0xFF000000
SLOT = 0x10000

DEF = re.compile(r"^#define\s+(WIN32_[A-Z0-9_]+_BASE)\s+(.+?)\s*$", re.MULTILINE)
HOST_DEF = re.compile(r"^#define\s+(D3DX_[A-Z0-9_]+_BASE)\s+(?:UINT32_C\()?(0x[0-9a-fA-F]+)", re.MULTILINE)
VALUE = re.compile(r"^(?:UINT32_C\(\s*(0x[0-9a-fA-F]+)\s*\)|\(\s*WIN32_THUNK_BASE\s*\+\s*(0x[0-9a-fA-F]+)u?\s*\)|(0x[0-9a-fA-F]+))$")


def parse(text):
    out = {}
    for name, expr in DEF.findall(text):
        if not name.endswith("_BASE") or name.endswith("HANDLE_BASE"):
            continue
        expr = re.sub(r"/\*.*?\*/", "", expr).strip()
        m = VALUE.match(expr)
        if not m:
            raise ValueError(f"cannot parse {name} = {expr!r}")
        if m.group(1):
            out[name] = int(m.group(1), 16)
        elif m.group(2):
            out[name] = THUNK_BASE + int(m.group(2), 16)
        else:
            out[name] = int(m.group(3), 16)
    return out


def parse_host(text):
    out = {}
    for name, value in HOST_DEF.findall(text):
        out[name] = int(value, 16)
    return out


def main():
    bases = {}
    files = sorted(SOURCE.glob("win32_*.h"))
    if not files:
        print(f"no win32 headers under {SOURCE}")
        return 1
    for path in files:
        for name, value in parse(path.read_text(encoding="utf-8")).items():
            if name in bases and bases[name] != value:
                print(f"FAIL: {name} defined as {value:#010x} and {bases[name][0]:#010x}")
                return 1
            bases[name] = (value, path.name)
    # Host shim windows (D3DX plain exports) live below the Win32 window.
    for path in sorted(list(SOURCE.glob("*.c")) + list(SOURCE.glob("*.h"))):
        for name, value in parse_host(path.read_text(encoding="utf-8", errors="replace")).items():
            if name in bases and bases[name][0] != value:
                print(f"FAIL: {name} defined as {value:#010x} and {bases[name][0]:#010x}")
                return 1
            bases[name] = (value, path.name)
    ok = True
    seen = {}
    for name, (value, where) in sorted(bases.items(), key=lambda kv: kv[1][0]):
        if name.startswith("D3DX_"):
            if not (HOST_LO <= value < HOST_HI):
                print(f"FAIL: {name} = {value:#010x} is outside {HOST_LO:#010x}..{HOST_HI:#010x}")
                ok = False
        elif not (WINDOW_LO <= value < WINDOW_HI):
            print(f"FAIL: {name} = {value:#010x} is outside {WINDOW_LO:#010x}..{WINDOW_HI:#010x}")
            ok = False
        if value in seen:
            print(f"FAIL: {name} ({where}) collides with {seen[value]} at {value:#010x}")
            ok = False
        seen[value] = name
    ordered = sorted(bases.items(), key=lambda kv: kv[1][0])
    for (prev_name, (prev_value, _)), (name, (value, where)) in zip(ordered, ordered[1:]):
        if value - prev_value < SLOT:
            print(f"FAIL: {name} ({where}, {value:#010x}) is inside "
                  f"{prev_name}'s 64 KiB window starting at {prev_value:#010x}")
            ok = False
    for name, (value, where) in ordered:
        print(f"  {name:28s} {value:#010x}  {where}")
    print("shim bases: " + ("all distinct" if ok else "COLLISION"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
