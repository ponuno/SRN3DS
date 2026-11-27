"""List CELL editor IDs from a TES5 (Skyrim) ESM without loading it all.

TES5 layout used here: record header 24 bytes (sig,size,flags,formid,stamp,
version,unknown), GRUP header 24 bytes and GRUP size includes the header,
compressed records (flag 0x40000) carry a u32 uncompressed size then zlib.

Usage:
  python tools/world_esm_cells.py Skyrim.esm Riverwood Whiterun
"""
import mmap
import struct
import sys
import zlib
from pathlib import Path


def main(argv):
    path = Path(argv[1])
    want = [w.lower() for w in argv[2:]]
    with open(path, "rb") as fh:
        d = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        if d[:4] != b"TES4":
            sys.exit("not a TES4/TES5 plugin")
        size = struct.unpack_from("<I", d, 4)[0]
        pos = 24 + size
        ends = []
        n = 0
        hits = []
        while pos + 24 <= len(d):
            while ends and pos >= ends[-1]:
                ends.pop()
            if not ends and pos + 24 > len(d):
                break
            sig = d[pos:pos + 4]
            rsize, flags, fid = struct.unpack_from("<III", d, pos + 4)
            if sig == b"GRUP":
                ends.append(pos + rsize)
                pos += 24
                continue
            body = pos + 24
            if sig == b"CELL":
                buf = bytes(d[body:body + rsize])
                if flags & 0x40000:
                    try:
                        buf = zlib.decompress(buf[4:])
                    except zlib.error:
                        buf = b""
                edid = None
                interior = 0
                p, end = 0, len(buf)
                while p + 6 <= end:
                    ssig = buf[p:p + 4]
                    ssize = struct.unpack_from("<H", buf, p + 4)[0]
                    sp = p + 6
                    if ssig == b"EDID":
                        edid = buf[sp:sp + ssize].split(b"\0")[0].decode("latin-1")
                    elif ssig == b"DATA" and ssize >= 2:
                        interior = struct.unpack_from("<H", buf, sp)[0] & 1
                    p = sp + ssize
                if edid:
                    n += 1
                    if not want or any(w in edid.lower() for w in want):
                        hits.append("%-52s form=%08x interior=%d" % (edid, fid, interior))
            pos = body + rsize
        for h in hits:
            print(h)
        print("# cells with EDID: %d, matches: %d" % (n, len(hits)), file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv)
