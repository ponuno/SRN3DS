"""Dump GFx9 shader/declaration tables from the on-disk TESV.exe."""
import struct
import sys

import pefile

EXE = r"F:\Skyrim\TESV.exe"
TABLES = {
    "decl_a": (0x12DFC8C, 4),
    "decl_b": (0x12DFC9C, 4),
    "vs_a": (0x12DFCA8, 5),
    "vs_b": (0x12DFCB8, 5),
    "ps_l2": (0x12DFCD8, 45),
    "ps_l3": (0x12DFA80, 25),
}


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = open(path, "rb").read()
        self.secs = []
        for s in self.pe.sections:
            self.secs.append((self.base + s.VirtualAddress,
                              max(s.Misc_VirtualSize, s.SizeOfRawData),
                              s.PointerToRawData))
        self.exec = [r for r in self.secs
                     if r[1] > 0]

    def read(self, va, n):
        for lo, size, raw in self.secs:
            if lo <= va < lo + size:
                off = raw + (va - lo)
                return self.data[off:off + n]
        return None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else None


def classify(t0):
    t = (t0 >> 16) & 0xFFFF
    major = (t0 >> 8) & 0xFF
    return ("vs" if t == 0xFFFE else "ps" if t == 0xFFFF else "??"), major


def blob(img, va):
    """Return (len_to_END, ctab_bytes, version, token_count)."""
    p = va
    t0 = img.u32(p)
    t1 = img.u32(p + 4)
    if t0 is None or t1 is None:
        return None
    ctab = t1 >> 16
    # A comment token is 4 bytes of header followed by `ctab` bytes of data;
    # the next instruction starts after both.
    q = p + 4 + ctab
    n = 0
    while n < 8192:
        tok = img.u32(q)
        if tok is None:
            return None
        q += 4
        n += 1
        if (tok & 0xFFFF) == 0xFFFF:
            break
    return q - va, ctab, t0, n


def main():
    img = Image(EXE)
    print("base=%08x" % img.base)
    for name, (va, count) in TABLES.items():
        print("\n== %s @ %08x (%d slots)" % (name, va, count))
        for i in range(count):
            ptr = img.u32(va + i * 4)
            if not ptr:
                print("  [%2d] NULL" % i)
                continue
            r = blob(img, ptr)
            if not r:
                print("  [%2d] %08x unreadable" % (i, ptr))
                continue
            ln, ctab, t0, ntok = r
            kind, major = classify(t0)
            h = img.read(ptr, min(ln, 8)).hex()
            print("  [%2d] %08x len=%4d tok=%3d ctab=%4d %s_%d   %s" %
                  (i, ptr, ln, ntok, ctab, kind, major, h))


if __name__ == "__main__":
    main()
