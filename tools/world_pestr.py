"""Find engine INI keys / argv switches in the retail TESV.exe (raw file +
PE section map) and the dword xrefs to their C strings.

Usage:
  python tools/world_pestr.py sStartingCell:General -loadgame
"""
import re
import sys
from pathlib import Path

import pefile

PE = Path(r"E:\SteamLibrary\steamapps\common\Skyrim\TESV.exe")


def main(argv):
    pe = pefile.PE(str(PE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    secs = [(s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData,
             s.SizeOfRawData, s.Name.rstrip(b"\0").decode()) for s in pe.sections]
    print("base %08x image %x" % (base, len(img)))
    pats = [a.encode() for a in argv[1:]] or [b"sStartingCell"]
    for pat in pats:
        hits = [m.start() for m in re.finditer(re.escape(pat), img)]
        print("\n== %s : %d hits ==" % (pat.decode(), len(hits)))
        for h in hits[:4]:
            va = base + h
            refs = []
            i = img.find(h.to_bytes(4, "little"))
            while i >= 0 and len(refs) < 12:
                refs.append(base + i)
                i = img.find(h.to_bytes(4, "little"), i + 1)
            ctx = img[max(0, h - 40):h + 70]
            print("  @%08x refs=%s" % (va, [hex(r) for r in refs]))
            print("    ctx: %r" % ctx)


if __name__ == "__main__":
    main(sys.argv)
