"""Read-only PE inspection: sections, entropy, entry-point bytes.

Flags executable sections with high entropy as likely packed/encrypted. This is
a diagnostic for deciding whether static recompilation is possible from a given
copy; it does not decrypt or modify anything.

Usage: python tools/inspect_pe.py <file.exe|dll> [--entry-insns N]
"""
import argparse
import math
from collections import Counter

import capstone
import pefile


def entropy(data):
    if not data:
        return 0.0
    counts = Counter(data)
    n = len(data)
    return -sum((c / n) * math.log2(c / n) for c in counts.values())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("file")
    ap.add_argument("--entry-insns", type=int, default=12)
    args = ap.parse_args()

    pe = pefile.PE(args.file, fast_load=True)
    pe.parse_data_directories()
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()
    entry = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    print(f"file: {args.file}")
    print(f"machine: {pe.FILE_HEADER.Machine:#x}  image base: {base:#x}  entry: {entry:#x}")
    print(f"image bytes: {pe.OPTIONAL_HEADER.SizeOfImage}  entry section:", end=" ")
    entry_sec = next((s.Name.decode(errors="replace").strip("\x00")
                      for s in pe.sections
                      if s.VirtualAddress <= (entry - base) < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)), "?")
    print(entry_sec)

    suspicious = 0
    for s in pe.sections:
        data = s.get_data()
        e = entropy(data[:0x10000])
        executable = bool(s.Characteristics & 0x20000000)
        marker = ""
        if executable and e > 7.5:
            marker = "  <-- high entropy: likely packed/encrypted"
            suspicious += 1
        print(f"  {s.Name.decode(errors='replace').strip(chr(0)):8s} rva {s.VirtualAddress:#08x} "
              f"raw {len(data):8d} entropy {e:4.2f}{marker}")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    off = entry - base
    print("entry instructions:")
    n = 0
    for i in md.disasm(image[off:off + 64], entry):
        print(f"  {i.address:#010x} {i.mnemonic:8s} {i.op_str}")
        n += 1
        if n >= args.entry_insns:
            break

    if suspicious:
        print("WARNING: executable section entropy indicates protection/packing.")
        print("Static recompilation requires the original unprotected executable;")
        print("do not attempt to bypass DRM. Use a lawfully obtained retail/unpacked copy.")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
