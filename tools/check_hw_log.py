#!/usr/bin/env python3
"""Validate a physical New 3DS TESV diagnostic log.

This does not prove rendering. It checks the documented hardware-test contract:
178 MiB application region, no pre-main svcBreak, expected guest frontier, and
(for irprobe) evidence that the IR interpreter actually executed.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def validate(text: str, expected_fault: str, require_ir: bool) -> tuple[bool, list[str]]:
    checks: list[tuple[bool, str]] = []

    region = re.search(r"(?:App memory region|app_region)\s*[:=]\s*(\d+)\s*MiB", text, re.I)
    checks.append((bool(region and int(region.group(1)) == 178),
                   "application region is 178 MiB"))
    checks.append(("svcBreak" not in text, "no svcBreak/pre-main libctru crash"))

    fault = re.search(r"Guest fault at\s+([0-9a-fA-F]{8}):\s*(.+)", text)
    expected = expected_fault.lower().removeprefix("0x").zfill(8)
    checks.append((bool(fault and fault.group(1).lower() == expected),
                   f"guest fault is 0x{expected}"))
    if fault:
        checks.append((bool(fault.group(2).strip()), "guest fault has a named reason"))

    if require_ir:
        cache = re.search(
            r"IR cache:\s*blocks=(\d+)\s+bytes=(\d+)\s+hits=(\d+)\s+misses=(\d+)",
            text,
            re.I,
        )
        checks.append((bool(cache), "IR cache statistics are present"))
        if cache:
            blocks, nbytes, hits, misses = map(int, cache.groups())
            checks.append((blocks > 0 and nbytes > 0,
                           f"IR cache is nonempty (blocks={blocks}, bytes={nbytes})"))
            checks.append((hits > 0 and misses > 0,
                           f"IR executed and decoded blocks (hits={hits}, misses={misses})"))

    return all(ok for ok, _ in checks), [
        ("PASS: " if ok else "FAIL: ") + label for ok, label in checks
    ]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    ap.add_argument("--expected-fault", default="00b2a970",
                    help="expected guest fault address (default: irprobe 00b2a970)")
    ap.add_argument("--require-ir", action="store_true",
                    help="require nonzero IR cache/decode evidence (irprobe)")
    args = ap.parse_args()
    try:
        text = args.log.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"FAIL: cannot read {args.log}: {exc}")
        return 2
    ok, lines = validate(text, args.expected_fault, args.require_ir)
    for line in lines:
        print(line)
    print("HARDWARE LOG: PASS" if ok else "HARDWARE LOG: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
