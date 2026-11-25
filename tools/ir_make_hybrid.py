"""Package a validated hybrid selection into a TESV generated directory.

Inputs are the stable slice functions.json, a profile-selected manifest (from
tools/ir_select.py), the real TESV.exe, and an output directory. The AOT subset
is lifted exactly (no closure growth); all other discovered functions remain
exact entries in the emitted IR table.

Usage:
  python tools/ir_make_hybrid.py \
      --functions E:/Skyrim_N3DS_build/analysis/growth12/iter01/slice_functions.json \
      --selection E:/Skyrim_N3DS_build/irfull/select \
      --exe E:/skyr/TESV.exe --out E:/Skyrim_N3DS_build/irfull/gen
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = ROOT / ".venv" / "Scripts" / "python.exe"
if not PY.exists():
    PY = Path(sys.executable)
sys.path.insert(0, str(ROOT / "tools"))
import ir_validate


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--functions", type=Path, required=True)
    ap.add_argument("--selection", type=Path, required=True)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    full = json.loads(args.functions.read_text(encoding="utf-8"))
    functions = full["functions"]
    manifest = json.loads((args.selection / "manifest.json").read_text(encoding="utf-8"))
    aot = ir_validate.read_aot(args.selection / "aot.txt")
    ir = ir_validate.read_ir_table(args.selection / "ir_table.c")
    errs = ir_validate.validate(aot, ir, manifest, functions)
    if errs:
        raise SystemExit("invalid selection:\n  " + "\n  ".join(errs))

    selected = set(aot)
    filtered = {
        "image_base": full["image_base"],
        "entry": full["entry"],
        "iat": full.get("iat", {}),
        "jump_tables": full.get("jump_tables", {}),
        "indirect_jumps": full.get("indirect_jumps", []),
        "functions": {f"0x{a:x}": functions[f"0x{a:x}"] for a in aot},
    }
    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    fj = args.out / "aot_functions.json"
    fj.write_text(json.dumps(filtered, indent=0), encoding="utf-8")

    env = dict(os.environ)
    env.update({"LIFT_LAZY_FLAGS": "1", "LIFT_SPILL": "1", "LIFT_GLOBALREGS": "1"})
    subprocess.run([str(PY), str(ROOT / "staged" / "lift.py"), str(args.exe),
                    str(fj), str(args.out), "tesv"], env=env, check=True)
    subprocess.run([str(PY), str(ROOT / "tools" / "instrument_generated.py"),
                    str(args.out), "--profile"], check=True)
    shutil.copyfile(args.selection / "ir_table.c", args.out / "tesv_ir_table.c")

    generated = None
    import re
    table = (args.out / "tesv_table.c").read_text(encoding="utf-8")
    generated = len(re.findall(r"\{\s*0x([0-9A-Fa-f]+)u\s*,\s*f_[0-9A-Fa-f]+\s*\}", table))
    rejected = json.loads((args.out / "tesv_rejected.json").read_text(encoding="utf-8"))
    if generated != len(aot) or rejected:
        raise SystemExit(f"lift mismatch: selected {len(aot)}, generated {generated}, "
                         f"rejected {len(rejected)}: {list(rejected.items())[:8]}")
    print(f"hybrid package: AOT {generated}, IR {len(ir)}, out {args.out}")
    print(f"entry {int(full['entry']):08x}: "
          f"{'AOT' if int(full['entry']) in selected else 'IR'} (exact table membership checked)")


if __name__ == "__main__":
    main()
