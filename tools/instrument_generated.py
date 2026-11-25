"""Instrument generated lift output with a per-basic-block budget guard.

Inserts n3ds_pilot_tick_pc(<block address>) after every block label and includes
pilot_guard.h. The PC-aware tick also drives the optional guest watchpoint.
With --profile it also inserts RT_PROF(0xADDR) at every function entry, so a run
records which lifted functions executed and how often. Fails if labels are found
but not all of them were instrumented, so a silent miss cannot turn into an
unbounded run.

Usage: python tools/instrument_generated.py GEN_DIR [--pattern GLOB] [--profile]
"""
import argparse
import re
from pathlib import Path

LABEL = re.compile(r"^(L_[A-Za-z0-9]+:)[ \t]*$", re.MULTILINE)
FUNC_HEAD = re.compile(
    r"^void (f_)([0-9A-Fa-f]+)(\(CPU \*c\) \{.*?^    \(void\)f; \(void\)jt_;)$",
    re.MULTILINE | re.DOTALL)


def add_profile_hooks(text):
    """Insert RT_PROF(<address>) after the declarations of every function."""
    count = 0

    def repl(m):
        nonlocal count
        count += 1
        return f"void {m.group(1)}{m.group(2)}{m.group(3)}\n    RT_PROF(0x{int(m.group(2), 16):08x}u);"

    return FUNC_HEAD.sub(repl, text), count


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("gen", type=Path)
    ap.add_argument("--pattern", default="*_[0-9][0-9].c")
    ap.add_argument("--profile", action="store_true",
                    help="insert RT_PROF(<address>) at every function entry")
    args = ap.parse_args()

    files = sorted(args.gen.glob(args.pattern))
    if not files:
        raise SystemExit(f"no generated files matching {args.pattern} in {args.gen}")
    total_labels = total_calls = 0
    used_files = 0
    calls = 0
    profiled = 0
    for path in files:
        text = path.read_text(encoding="utf-8")
        if "n3ds_pilot_tick" in text:
            raise SystemExit(f"{path} is already instrumented")
        labels = len(LABEL.findall(text))
        if not labels:
            continue
        if args.profile:
            # The counter sits after the local declarations and before the
            # entry label, so a jump back to L_ENTRY is not double counted.
            text, hooks = add_profile_hooks(text)
            if hooks != len(re.findall(r"^void f_[0-9A-Fa-f]+\(CPU \*c\) \{", text, re.MULTILINE)):
                raise SystemExit(f"{path}: profiled {hooks} functions, expected "
                                 f"{len(re.findall(r'^void f_[0-9A-Fa-f]+\\(CPU \\*c\\) \\{', text, re.MULTILINE))}")
            profiled += hooks
        def add_tick(m):
            pc = m.group(1)[2:-1]
            return f"{m.group(1)}\n    n3ds_pilot_tick_pc(0x{pc}u);"

        text, calls = LABEL.subn(add_tick, text)
        if calls != labels:
            raise SystemExit(f"{path}: instrumented {calls} of {labels} labels")
        path.write_text('#include "pilot_guard.h"\n#include "rt_prof.h"\n' + text, encoding="utf-8")
        total_labels += labels
        total_calls += calls
        used_files += 1
    if not calls:
        raise SystemExit("no block labels found in any generated file")
    print(f"instrumented {total_calls} labels in {used_files} files"
          + (f", profiled {profiled} functions" if args.profile else ""))


if __name__ == "__main__":
    main()
