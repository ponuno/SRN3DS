"""Parse a Luma3DS exception dump (ARM11/ARM9) without external tools.

Format (Luma3DS arm9/source/types.h):
  ExceptionDumpHeader (0x28 bytes): magic[2], verMinor, verMajor, processor,
  core, type, totalSize, registerDumpSize, codeDumpSize, stackDumpSize,
  additionalDataSize.  Then regs (R0-R12, SP, LR, PC, CPSR, ...), code bytes,
  stack bytes, additional data (process name + title id for ARM11).

Usage: python tools/parse_luma_dump.py DUMP [--elf ELF] [--addr2line PATH]
"""
import argparse
import struct
import subprocess
from pathlib import Path

TYPE_NAMES = {0: "FIQ", 1: "undefined instruction", 2: "prefetch abort", 3: "data abort"}
REG_NAMES = ["R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10", "R11",
             "R12", "SP", "LR", "PC", "CPSR"]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--elf", default="")
    ap.add_argument("--addr2line", default=r"C:\devkitPro\devkitARM\bin\arm-none-eabi-addr2line.exe")
    args = ap.parse_args()

    b = args.dump.read_bytes()
    (magic0, magic1, ver_minor, ver_major, processor, core, etype,
     total, reg_size, code_size, stack_size, extra_size) = struct.unpack_from("<IIHHHHIIIIII", b, 0)
    print(f"magic: {magic0:08x} {magic1:08x}")
    print(f"version: {ver_major}.{ver_minor}  processor: ARM{processor} core {core}")
    print(f"type: {etype} ({TYPE_NAMES.get(etype, 'unknown')})")
    print(f"sizes: total={total} regs={reg_size} code={code_size} stack={stack_size} extra={extra_size}")

    regs_off = 0x28
    nregs = reg_size // 4
    regs = list(struct.unpack_from("<%dI" % nregs, b, regs_off))
    for i, val in enumerate(regs):
        name = REG_NAMES[i] if i < len(REG_NAMES) else f"REG{i}"
        print(f"  {name:5s} {val:08x}")
    if len(regs) > 20:
        print(f"  FPEXC {regs[20]:08x}")
        for i in range(17, len(regs)):
            if i != 20:
                print(f"  REG{i}  {regs[i]:08x}")

    code_off = regs_off + reg_size
    code = b[code_off:code_off + code_size]
    print(f"code bytes at file offset {code_off:#x}: {code.hex()}")
    stack_off = code_off + code_size
    stack = b[stack_off:stack_off + stack_size]
    print(f"stack bytes at file offset {stack_off:#x} (SP={regs[13]:08x}): {stack.hex()}")
    extra_off = stack_off + stack_size
    extra = b[extra_off:extra_off + extra_size]
    if extra:
        name = extra[:8].rstrip(b"\x00").decode("ascii", "replace")
        title = struct.unpack_from("<Q", extra, 8)[0] if len(extra) >= 16 else 0
        print(f"process: {name}  title id: {title:016x}")

    if args.elf and Path(args.elf).exists() and Path(args.addr2line).exists():
        addrs = [regs[15], regs[14]]
        out = subprocess.run([args.addr2line, "-f", "-C", "-e", args.elf] + [hex(a) for a in addrs],
                             capture_output=True, text=True)
        print("symbols (PC, LR):")
        print(out.stdout.strip() or out.stderr.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
