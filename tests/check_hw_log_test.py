"""Tests for tools/check_hw_log.py hardware-log contract."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import check_hw_log  # noqa: E402


def main():
    good = """App memory region: 178 MiB
Guest fault at 00b2a970: unknown indirect function
IR cache: blocks=859 bytes=94584 hits=18468 misses=859 evicted=0
"""
    ok, lines = check_hw_log.validate(good, "b2a970", True)
    assert ok, lines
    print("  ok valid irprobe hardware log")

    cases = [
        (good.replace("178 MiB", "96 MiB"), "b2a970", True, "wrong region"),
        (good.replace("00b2a970", "00403780"), "b2a970", True, "wrong fault"),
        (good.replace("IR cache: blocks=859 bytes=94584 hits=18468 misses=859 evicted=0", ""),
         "b2a970", True, "missing IR evidence"),
        (good + "svcBreak\n", "b2a970", True, "pre-main crash"),
    ]
    for text, fault, require_ir, label in cases:
        ok, lines = check_hw_log.validate(text, fault, require_ir)
        assert not ok, (label, lines)
        print(f"  ok rejects {label}")

    aot = """app_region=178 MiB
Guest fault at 00403780: unknown indirect function
IR cache: blocks=0 bytes=0 hits=0 misses=0 evicted=0
"""
    ok, lines = check_hw_log.validate(aot, "0x403780", False)
    assert ok, lines
    print("  ok pure-AOT hwtest log without IR requirement")


if __name__ == "__main__":
    main()
