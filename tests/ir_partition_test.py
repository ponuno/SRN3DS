"""Partition validation test for tools/ir_select.py output.

Runs a synthetic valid partition and a set of deliberately broken ones, then
validates the real selection under E:\\Skyrim_N3DS_build\\hybrid_select_v2 when
that directory exists (this is the real profile run from docs/IR.md).
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import ir_validate

FUNCS = {
    "0x1000": [[0x1000, 0x1010], [0x1010, 0x1020]],
    "0x1100": [[0x1100, 0x1120]],
    "0x1200": [[0x1200, 0x1210], [0x1300, 0x1310]],  # shared cold block
}


def manifest_for(aot, ir):
    ir_ranges = [r for a, _ in ir if f"0x{a:x}" in FUNCS for r in FUNCS[f"0x{a:x}"]]
    return {"aot_functions": len(aot), "ir_functions": len(ir),
            "ir_x86_bytes": ir_validate.union_bytes(ir_ranges)}


def case(name, aot, ir, want_error):
    errs = ir_validate.validate(aot, ir, manifest_for(aot, ir), FUNCS)
    if bool(errs) != want_error:
        print(f"  FAIL {name}: errors={errs}")
        return 1
    print(f"  ok {name}" + (f": {errs[0]}" if want_error else ""))
    return 0


def main():
    fail = 0
    aot = [0x1000, 0x1200]
    ir = [(0x1100, 0x1120)]
    fail |= case("valid disjoint partition", aot, ir, False)
    fail |= case("duplicate AOT", [0x1000, 0x1000], ir, True)
    fail |= case("AOT/IR overlap", [0x1000, 0x1100], ir, True)
    fail |= case("duplicate IR entry", aot, [(0x1100, 0x1120), (0x1100, 0x1120)], True)
    fail |= case("bad end", aot, [(0x1100, 0x1100)], True)
    fail |= case("unknown function", aot, [(0x9999, 0x99a0)], True)

    real = Path(r"E:\Skyrim_N3DS_build\hybrid_select_v2")
    realf = Path(r"E:\Skyrim_N3DS_build\analysis\growth12\iter01\slice_functions.json")
    if (real / "aot.txt").exists() and (real / "ir_table.c").exists() and realf.exists():
        functions = json.loads(realf.read_text(encoding="utf-8"))["functions"]
        a = ir_validate.read_aot(real / "aot.txt")
        t = ir_validate.read_ir_table(real / "ir_table.c")
        m = json.loads((real / "manifest.json").read_text(encoding="utf-8"))
        errs = ir_validate.validate(a, t, m, functions)
        if errs:
            print("  FAIL real hybrid_select_v2:", errs[:3])
            fail = 1
        else:
            print(f"  ok real hybrid_select_v2 ({len(a)} AOT / {len(t)} IR, "
                  f"{m['ir_x86_bytes']} union bytes)")
    else:
        print("  skip real hybrid_select_v2 (not present)")

    if fail:
        print("partition tests FAILED")
        return 1
    print("partition tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
