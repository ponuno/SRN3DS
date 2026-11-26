"""Instrument a disposable generated slice for the CECDC0 source-fill probe.

Inserts n3ds_cecdc0_source_fill() into f_CECDC0's L_CED041 block, between the
index load and the uninitialised base-local load. A `.cecdc0src.bak` is
created next to the edited file; the checked-in generated sources are not
modified.

Usage:
    python tools/instrument_cecdc0_source.py GEN_DIR
    python tools/instrument_cecdc0_source.py GEN_DIR --restore
"""
import argparse
from pathlib import Path

FUNC = "void f_CECDC0(CPU *c) {"
SEQUENCE = (
    "    edx = (uint32_t)(M32((uint32_t)(esp + 0x54u)));\n"
    "    ecx = (uint32_t)(TL32((uint32_t)(edx + 0x624u)));\n"
    "    edx = (uint32_t)(M32((uint32_t)(esp + 0x18u)));\n"
    "    ecx = (uint32_t)(TL32((uint32_t)(edx + ecx*4)));\n"
    "    M32((uint32_t)(esp + 0x14u)) = (uint32_t)(ecx);\n"
)
SEQUENCE_HOOK = (
    "    edx = (uint32_t)(M32((uint32_t)(esp + 0x54u)));\n"
    "    ecx = (uint32_t)(TL32((uint32_t)(edx + 0x624u)));\n"
    "    if (!n3ds_cecdc0_source_fill((uint32_t)(esp), (uint32_t)(ecx), (uint32_t)(ebx))) {\n"
    "        edx = (uint32_t)(M32((uint32_t)(esp + 0x18u)));\n"
    "        ecx = (uint32_t)(TL32((uint32_t)(edx + ecx*4)));\n"
    "        M32((uint32_t)(esp + 0x14u)) = (uint32_t)(ecx);\n"
    "    }\n"
)
PROTO = "extern int n3ds_cecdc0_source_fill(uint32_t frame, uint32_t index, uint32_t ebx);\n"
ANCHOR = '#include "lift_prelude.h"\n'


def restore(gen: Path) -> int:
    restored = 0
    suffix = ".cecdc0src.bak"
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
            raise SystemExit("no .cecdc0src.bak files found")
        return

    total = 0
    for path in sorted(args.gen.glob("tesv_*.c")):
        text = path.read_text(encoding="utf-8")
        if "n3ds_cecdc0_source_fill" in text:
            raise SystemExit(f"{path} is already instrumented")
        start = text.find(FUNC)
        if start < 0:
            continue
        end = text.find("\nvoid f_", start + len(FUNC))
        if end < 0:
            raise SystemExit(f"{path}: no function boundary after f_CECDC0")
        block = text[start:end]
        count = block.count(SEQUENCE)
        if count != 1:
            raise SystemExit(
                f"{path}: expected one L_CED041 sequence in f_CECDC0, got {count}")
        if text.count(ANCHOR) != 1:
            raise SystemExit(f"{path}: lift_prelude anchor is not unique")
        bak = path.with_name(path.name + ".cecdc0src.bak")
        if bak.exists():
            raise SystemExit(f"{bak} exists; run --restore first")
        bak.write_bytes(path.read_bytes())
        block = block.replace(SEQUENCE, SEQUENCE_HOOK, 1)
        text = text[:start] + block + text[end:]
        if PROTO not in text:
            text = text.replace(ANCHOR, ANCHOR + PROTO, 1)
        path.write_text(text, encoding="utf-8")
        print(f"{path}: f_CECDC0 L_CED041 -> n3ds_cecdc0_source_fill")
        total += 1

    if total != 1:
        raise SystemExit(f"expected one instrumented file, got {total}")


if __name__ == "__main__":
    main()
