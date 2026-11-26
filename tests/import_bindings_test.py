"""Unit tests for tools/check_import_bindings.py.

Run with the project venv:  .venv/Scripts/python.exe tests/import_bindings_test.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_import_bindings as cb  # noqa: E402

HOTPATCH = {
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


def trace(plain_addr=None, hot_override=None):
    lines = []
    for i, (name, addr) in enumerate(HOTPATCH.items()):
        if hot_override and name == hot_override[0]:
            addr = hot_override[1]
        lines.append(f"[import] d3dx9_42.dll!{name} -> {addr:08x} "
                     f"(export {addr - 11:08x} hotpatch body)")
    for i, name in enumerate(PLAIN):
        addr = cb.PLAIN_EXPECT[name] if plain_addr is None else plain_addr + 4 * i
        lines.append(f"[import] d3dx9_42.dll!{name} -> {addr:08x} (host shim)")
    return lines


def main():
    print("import-binding trace checks")
    assert cb.check(cb.parse(trace()), require_shims=True) == 0
    print("  ok: nine hotpatch bodies plus nine host shims")

    assert cb.check(cb.parse(trace()), require_shims=False) == 0
    print("  ok: guest plain bodies are informational without --require-shims")

    assert cb.check(cb.parse(trace(plain_addr=0x080BA5D3)), require_shims=True) == 1
    print("  ok: a guest plain body fails --require-shims")

    # A host thunk at the wrong slot (texture/surface swap) must also fail.
    swapped = cb.parse(trace())
    swapped["D3DXCompileShader"] = (0xFD000000, False)
    assert cb.check(swapped, require_shims=True) == 1
    print("  ok: a swapped host thunk fails")

    assert cb.check(cb.parse(trace(hot_override=("D3DXMatrixMultiply", 0x0802FA5D))),
                    require_shims=False) == 1
    print("  ok: a wrapper/raw address fails the hotpatch expectation")

    missing = [line for line in trace() if "D3DXMatrixInverse" not in line]
    assert cb.check(cb.parse(missing), require_shims=False) == 1
    print("  ok: a missing export fails")

    # A non-D3DX import is ignored, not misread as a D3DX binding.
    assert cb.check(cb.parse(["[import] binkw32.dll!_BinkOpen@8 -> 09010690"]),
                    require_shims=False) == 1
    print("  ok: unrelated imports do not satisfy the table")

    print("import-binding trace: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
