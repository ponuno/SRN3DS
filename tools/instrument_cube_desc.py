"""Instrument a generated slice copy with the cube descriptor-slot probe.

Inserts n3ds_cube_desc_probe() after the two f_CECDC0 slot computations
(L_CED041 and L_CED160). Run only on a disposable copy of the generated slice;
the checked-in generated sources are not modified.

Usage: python tools/instrument_cube_desc.py GEN_DIR
"""
import argparse
from pathlib import Path

SLOT_041 = """    edx = (uint32_t)(M32((uint32_t)(esp + 0x54u)));
    ecx = (uint32_t)(TL32((uint32_t)(edx + 0x624u)));
    edx = (uint32_t)(M32((uint32_t)(esp + 0x18u)));
    ecx = (uint32_t)(TL32((uint32_t)(edx + ecx*4)));
    M32((uint32_t)(esp + 0x14u)) = (uint32_t)(ecx);
"""
SLOT_041_HOOK = SLOT_041 + """    n3ds_cube_desc_probe((uint32_t)M32((uint32_t)(esp + 0x54u)),
                        (uint32_t)M32((uint32_t)(M32((uint32_t)(esp + 0x54u)) + 0x624u)),
                        (uint32_t)M32((uint32_t)(esp + 0x18u)),
                        (uint32_t)M32((uint32_t)(esp + 0x14u)));
"""

SLOT_160 = """    edx = (uint32_t)(M32((uint32_t)(esp + 0x60u)));
    ecx = (uint32_t)(M32((uint32_t)(esp + 0x24u)));
    ebx = (uint32_t)(eax);
    eax = (uint32_t)(TL32((uint32_t)(edx + 0x624u)));
    eax = (uint32_t)(TL32((uint32_t)(ecx + eax*4)));
"""
SLOT_160_HOOK = SLOT_160 + """    n3ds_cube_desc_probe((uint32_t)M32((uint32_t)(esp + 0x60u)),
                        (uint32_t)M32((uint32_t)(M32((uint32_t)(esp + 0x60u)) + 0x624u)),
                        (uint32_t)M32((uint32_t)(esp + 0x24u)),
                        (uint32_t)eax);
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("gen", type=Path)
    args = ap.parse_args()
    total = 0
    for path in sorted(args.gen.glob("tesv_*.c")):
        text = path.read_text(encoding="utf-8")
        if "n3ds_cube_desc_probe" in text:
            raise SystemExit(f"{path} is already instrumented")
        n041 = text.count(SLOT_041)
        n160 = text.count(SLOT_160)
        if not n041 and not n160:
            continue
        text = text.replace(SLOT_041, SLOT_041_HOOK)
        text = text.replace(SLOT_160, SLOT_160_HOOK)
        path.write_text('#include "n3ds_lift_runtime.h"\n' + text, encoding="utf-8")
        total += n041 + n160
        print(f"{path}: L_CED041={n041} L_CED160={n160}")
    if not total:
        raise SystemExit("no cube descriptor slot computations found")
    print(f"instrumented {total} cube descriptor slot computations")


if __name__ == "__main__":
    main()
