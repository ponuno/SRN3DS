import struct
from pathlib import Path

d = Path(r"E:\census_ir2\ir_container\_work\ir_container_input.bin").read_bytes()
magic, lo, size, entry = struct.unpack("<IIII", d[:16])
print("hdr", hex(magic), hex(lo), hex(size), hex(entry))
code = d[16:16 + size]
pats = [b"sStartingCell\0", b"sStartingWorld\0", b"sStartingCellX\0",
        b"sStartingCellY\0", b"-loadgame", b"-startgame", b"loadgame",
        b"bUseMyGamesDirectory", b"bRunVTuneTest", b"iFPSClamp",
        b"sStartingCellX", b"sStartingCellY"]
for pat in pats:
    i = 0
    hits = []
    while True:
        i = code.find(pat, i)
        if i < 0:
            break
        hits.append(lo + i)
        i += 1
    print(pat, [hex(h) for h in hits[:8]])
