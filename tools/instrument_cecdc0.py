"""Instrument a disposable generated slice for the CECDC0 call-site probe.

Renames exactly one lifted call, f_CD4E40's `f_CECDC0(c)` at return address
0x00CD4E68, to tesv_cecdc0_probe_shadow(). The checked-in generated sources are
not modified; a `.cecdc0.bak` is created next to the edited file.

Usage:
    python tools/instrument_cecdc0.py GEN_DIR
    python tools/instrument_cecdc0.py GEN_DIR --restore
"""
import argparse
from pathlib import Path

CALL = ("esp -= 4; M32(esp) = 0xCD4E68u; SPILLN(0xFFu); f_CECDC0(c); "
        "RELOADN(0x51u);")
CALL_SHADOW = ("esp -= 4; M32(esp) = 0xCD4E68u; SPILLN(0xFFu); "
               "tesv_cecdc0_probe_shadow(c); RELOADN(0x51u);")
PROTO = "extern void tesv_cecdc0_probe_shadow(CPU *c);\n"
ANCHOR = '#include "lift_prelude.h"\n'


def restore(gen: Path) -> int:
    restored = 0
    suffix = ".cecdc0.bak"
    for bak in sorted(gen.glob("tesv_*.c" + suffix)):
        target = bak.with_name(bak.name[:-len(suffix)])
        target.write_bytes(bak.read_bytes())
        bak.unlink()
        print(f"restored {target}")
        restored += 1
    return restored


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("gen", type=Path)
    ap.add_argument("--restore", action="store_true")
    args = ap.parse_args()

    if args.restore:
        if not restore(args.gen):
            raise SystemExit("no .cecdc0.bak files found")
        return

    total = 0
    for path in sorted(args.gen.glob("tesv_*.c")):
        text = path.read_text(encoding="utf-8")
        if "tesv_cecdc0_probe_shadow" in text:
            raise SystemExit(f"{path} is already instrumented")
        count = text.count(CALL)
        if not count:
            continue
        if count != 1:
            raise SystemExit(f"{path}: expected one f_CD4E40 call, got {count}")
        if text.count(ANCHOR) != 1:
            raise SystemExit(f"{path}: lift_prelude anchor is not unique")
        bak = path.with_name(path.name + ".cecdc0.bak")
        if bak.exists():
            raise SystemExit(f"{bak} exists; run --restore first")
        bak.write_bytes(path.read_bytes())
        text = text.replace(CALL, CALL_SHADOW, 1)
        if PROTO not in text:
            text = text.replace(ANCHOR, ANCHOR + PROTO, 1)
        path.write_text(text, encoding="utf-8")
        print(f"{path}: f_CD4E40 -> tesv_cecdc0_probe_shadow")
        total += 1

    if total != 1:
        raise SystemExit(f"expected one instrumented file, got {total}")


if __name__ == "__main__":
    main()
