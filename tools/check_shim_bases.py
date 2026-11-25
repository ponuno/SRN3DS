"""Static check: Win32 shim thunk windows must be disjoint.

A real bug this catches: win32_shell and win32_locale both used 0xff030000, so
every shell32 call was dispatched to a locale handler (win32_lookup checks
locale first). The collision was invisible in tests that only exercised one
module.

The check parses the `#define WIN32_*_BASE ...` constants (the simple forms the
project uses) and fails on duplicates or a base outside the shim window.
"""
import re
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "source"
WINDOW_LO = 0xFF000000
WINDOW_HI = 0xFF100000
THUNK_BASE = 0xFF000000

DEF = re.compile(r"^#define\s+(WIN32_[A-Z0-9_]+_BASE)\s+(.+?)\s*$", re.MULTILINE)
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
    ok = True
    seen = {}
    for name, (value, where) in sorted(bases.items(), key=lambda kv: kv[1][0]):
        if not (WINDOW_LO <= value < WINDOW_HI):
            print(f"FAIL: {name} = {value:#010x} is outside {WINDOW_LO:#010x}..{WINDOW_HI:#010x}")
            ok = False
        if value in seen:
            print(f"FAIL: {name} ({where}) collides with {seen[value]} at {value:#010x}")
            ok = False
        seen[value] = name
    for name, (value, where) in sorted(bases.items(), key=lambda kv: kv[1][0]):
        print(f"  {name:28s} {value:#010x}  {where}")
    print("shim bases: " + ("all distinct" if ok else "COLLISION"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
