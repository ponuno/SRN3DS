"""Verify D3DX9_42 import bindings in an N3DS_IMPORT_TRACE log.

The hotpatch layer has one correct answer per export; a resolver change that
silently binds a wrapper or a plain body must fail here before it reaches the
renderer. Plain exports are reported by class (host shim vs guest DLL body);
`--require-shims` makes a guest body a failure, which is the expected state
after the plain-shim modules are integrated.

Usage:
  python tools/check_import_bindings.py E:\\...\\trace.err
  python tools/check_import_bindings.py trace.err --require-shims

The trace records PE module exports (hotpatch bodies) and, since the loader
diagnostic change, bindings resolved by the host builtin before PE lookup
(`(host shim)`). Both are parsed; a plain export is a failure under
`--require-shims` only when it still points into the staged DLL.
"""
import argparse
import re
import sys

EXPECT = {
    "D3DXMatrixMultiply": 0x0802FA80,
    "D3DXMatrixTranspose": 0x0802FCE0,
    "D3DXMatrixMultiplyTranspose": 0x0802FD9D,
    "D3DXPlaneTransform": 0x08032303,
    "D3DXVec3TransformCoord": 0x08032EAD,
    "D3DXVec3TransformNormal": 0x0803306A,
    "D3DXVec3Normalize": 0x08032CBF,
    "D3DXPlaneNormalize": 0x08032000,
    "D3DXMatrixInverse": 0x0802FFFD,
}
PLAIN = [
    "D3DXGetImageInfoFromFileInMemory",
    "D3DXCreateTextureFromFileInMemory",
    "D3DXCreateCubeTextureFromFileInMemory",
    "D3DXCreateVolumeTextureFromFileInMemory",
    "D3DXCompileShader",
    "D3DXGetImageInfoFromFileA",
    "D3DXCreateTextureFromFileExA",
    "D3DXLoadSurfaceFromSurface",
    "D3DXLoadSurfaceFromMemory",
]
# Host thunks live below the Win32 window (0xff000000) and above the staged DLL.
HOST_SHIM_LO = 0xFC000000
HOST_SHIM_HI = 0xFF000000
LINE = re.compile(
    r"\[import\]\s+([^!\s]+)!([^\s]+)\s+->\s+([0-9a-fA-F]{8})"
    r"(?:\s+\(export\s+([0-9a-fA-F]{8})\s+hotpatch body\))?")


def parse(lines):
    seen = {}
    for line in lines:
        match = LINE.search(line)
        if not match:
            continue
        dll, name, addr, body = match.groups()
        if dll.lower() != "d3dx9_42.dll":
            continue
        seen[name] = (int(addr, 16), body is not None)
    return seen


def check(seen, require_shims):
    failed = False
    for name, want in EXPECT.items():
        got = seen.get(name)
        if not got:
            print(f"MISSING {name}")
            failed = True
            continue
        addr, hotpatch = got
        if not hotpatch or addr != want:
            print(f"MISMATCH {name}: {addr:08x} "
                  f"hotpatch={hotpatch} expected {want:08x} hotpatch=True")
            failed = True
        else:
            print(f"ok {name} -> {addr:08x} hotpatch body")
    for name in PLAIN:
        got = seen.get(name)
        if not got:
            print(f"MISSING {name}")
            failed = True
            continue
        addr, hotpatch = got
        if HOST_SHIM_LO <= addr < HOST_SHIM_HI:
            state, ok = "host shim", True
        elif hotpatch:
            state, ok = "hotpatch body", not require_shims
        else:
            state, ok = "guest DLL body", not require_shims
        print(f"{'ok' if ok else 'FAIL'} {name} -> {addr:08x} ({state})")
        failed = failed or not ok
    return 1 if failed else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace", help="stderr captured with N3DS_IMPORT_TRACE=1")
    parser.add_argument("--require-shims", action="store_true",
                        help="fail when a plain D3DX export still points at a guest body")
    args = parser.parse_args(argv)
    try:
        with open(args.trace, encoding="utf-8", errors="replace") as stream:
            return check(parse(stream), args.require_shims)
    except OSError as exc:
        print(f"cannot read trace: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
