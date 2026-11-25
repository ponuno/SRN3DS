"""Regression check: the staged lifter must lift a real closure slice cleanly.

Runs staged/lift.py over one or more functions.json slices exactly the way the
growth loop does (MOD=tesv, local-decode budget 400000) into a scratch directory
and fails on a non-zero exit or an exception. Reports functions/instructions and
elapsed time so a silent slowdown or a new Unsupported path is visible.

Usage: python tools/lift_regression.py [SLICE ...] [--out DIR]
Defaults to the growth11 iter01 slice when it exists (24 772 functions).
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = ROOT / ".venv" / "Scripts" / "python.exe"
if not PY.exists():
    PY = Path(sys.executable)
EXE = os.environ.get("TESV_EXE", r"E:\skyr\TESV.exe")
DEFAULT = r"E:\Skyrim_N3DS_build\analysis\growth11\iter01\slice_functions.json"


def main():
    argv = sys.argv[1:]
    out = ROOT / "build_host" / "lift_regression"
    slices = []
    k = 0
    while k < len(argv):
        if argv[k] == "--out":
            out = Path(argv[k + 1])
            k += 2
            continue
        if not argv[k].startswith("-"):
            slices.append(Path(argv[k]))
        k += 1
    slices = slices or [Path(DEFAULT)]
    rc = 0
    for sl in slices:
        if not sl.exists():
            print(f"SKIP (missing): {sl}")
            continue
        try:
            info = json.loads(sl.read_text(encoding="utf-8"))
            n = len(info.get("functions", {}))
        except Exception:
            n = -1
        dest = out / (sl.parent.name or "slice")
        dest.mkdir(parents=True, exist_ok=True)
        t0 = time.time()
        p = subprocess.run([str(PY), str(ROOT / "staged" / "lift.py"), EXE, str(sl), str(dest),
                            "tesv", "400000"], capture_output=True, text=True)
        dt = time.time() - t0
        tail = " | ".join(p.stdout.strip().splitlines()[:2])
        if p.returncode != 0:
            print(f"FAIL: {sl}\n{p.stdout}\n{p.stderr}")
            rc = 1
        else:
            print(f"OK: {sl.name}: {n} functions in {dt:.1f}s ({tail})")
    return rc


if __name__ == "__main__":
    sys.exit(main())
