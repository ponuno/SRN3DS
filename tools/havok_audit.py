#!/usr/bin/env python3
"""Static Havok coverage audit for the TESV guest image.

The IR container stores only .text (code) plus its function table, so there are
no Havok RTTI strings inside it.  The code bytes are byte-identical to
E:\\skyr\\TESV.exe, whose .rdata still has the type descriptors and vtable
names.  This tool therefore:

  * finds hkp*/hknp*/hkVector*/Physics strings and RTTI vtables in TESV.exe;
  * turns string/vtable references in .text into guest function addresses;
  * clusters those functions into address ranges;
  * decodes every Havok function with capstone and checks each SSE/x87 form
    against the exact predicates used by source/ir_decode.c;
  * cross-checks source/ir_decode.c's invalid.csv verdict for the same
    function addresses;
  * writes a functions.json subset (same schema as the lifter input) so
    staged/lift.py can be run over exactly the Havok closure.

Usage:
  python tools/havok_audit.py [--json out.json] [--subset out.json]
"""
import argparse
import bisect
import collections
import csv
import json
import re
import struct
import sys
from pathlib import Path

IMAGE = Path(r"E:\census_ir2\ir_container\_work\ir_container_input.bin")
EXE = Path(r"E:\skyr\TESV.exe")
FUNCS = Path(r"E:\Skyrim_N3DS_build\analysis\tesv\functions.json")
IDX = Path(r"E:\census_ir2\ir_container\idx.csv")
INVALID = Path(r"E:\census_ir2\ir_container\invalid.csv")
ROOT = Path(__file__).resolve().parent.parent

HAVOK_RE = re.compile(r"(?:^|[^A-Za-z0-9])(?:hkp|hknp|hkVector|hkBool|hkReal|hkInt|hkString|Physics|RigidBody|Collision|Constraint)")
RTTI_RE = re.compile(r"^\.\?AV(?:hknp|hkp|hkVector|hkBase|hkReferenced|hkWorld|bhk|Physics)")

# ---------------------------------------------------------------- image


def load_image():
    b = IMAGE.read_bytes()
    magic, lo, text_size, nfunc = struct.unpack_from("<IIII", b, 0)
    if magic != 0x49524D31:
        raise SystemExit("unexpected IR image magic %08x" % magic)
    return b, lo, text_size


def load_pe_sections(path):
    d = path.read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, pe + 6)[0]
    so = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<I", d, opt + 28)[0]
    secs = []
    s = opt + so
    for _ in range(n):
        name = d[s:s + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, rp = struct.unpack_from("<IIII", d, s + 8)
        secs.append((name, image_base + va, max(vs, rs), rs, rp))
        s += 40
    return d, image_base, secs


def va_off(secs, va):
    for _n, base, size, rsize, rp in secs:
        if base <= va < base + min(size, rsize):
            return rp + (va - base)
    return None


# ------------------------------------------------------- IR predicates


def x87_form_ok(op, modrm):
    mod = modrm >> 6
    reg = (modrm >> 3) & 7
    if mod != 3:
        if op in (0xD8, 0xDC, 0xDA, 0xDE):
            return True
        if op == 0xD9:
            return reg != 1
        if op == 0xDB:
            return reg in (0, 1, 2, 3, 5, 7)
        if op == 0xDD:
            return reg != 5
        if op == 0xDF:
            return reg in (0, 1, 2, 3, 5, 6, 7)
        return False
    if op == 0xD8:
        return True
    if op == 0xD9:
        if reg in (0, 1) or 0xD8 <= modrm <= 0xDF:
            return True
        return modrm in (0xD0, 0xE0, 0xE1, 0xE4, 0xE5, 0xE8, 0xE9, 0xEA,
                         0xEB, 0xEC, 0xED, 0xEE, 0xF0, 0xF1, 0xF2, 0xF3,
                         0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC,
                         0xFD, 0xFE, 0xFF)
    if op == 0xDA:
        return modrm == 0xE9 or reg <= 3
    if op == 0xDB:
        return modrm in (0xE2, 0xE3) or reg <= 3 or reg in (5, 6)
    if op in (0xDC, 0xDE):
        return True
    if op == 0xDD:
        return reg in (0, 2, 3, 4, 5)
    if op == 0xDF:
        return modrm == 0xE0 or reg in (5, 6, 0)
    return False


def sse_supported(op2, pfx):
    if op2 in (0x10, 0x11, 0x51, 0xC2):
        return True
    if op2 in (0x28, 0x29, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x50,
               0x54, 0x55, 0x56, 0x57, 0xC6):
        return pfx in (0, 1)
    if op2 == 0x2B:
        return pfx in (0, 1)
    if op2 in (0x2E, 0x2F):
        return pfx in (0, 1)
    if op2 in (0x2A, 0x2C, 0x2D):
        return pfx in (2, 3)
    if op2 in (0x52, 0x53):
        return pfx in (0, 3)
    if op2 == 0x5A:
        return pfx in (0, 2, 3)
    if op2 == 0x5B:
        return pfx in (0, 1, 3)
    if op2 in (0x60, 0x61, 0x62, 0x63, 0x67, 0x68, 0x69, 0x6A, 0x6B,
               0x6C, 0x6D, 0xD8, 0xD9, 0xDA, 0xDC, 0xDD, 0xDE):
        return pfx == 1
    if op2 in (0x58, 0x59, 0x5C, 0x5D, 0x5E, 0x5F):
        return True
    if op2 in (0x6E, 0x7E, 0xD6):
        return pfx in (1, 3)
    if op2 in (0x6F, 0x7F):
        return pfx in (1, 3)
    if op2 == 0x5B:
        return pfx in (1, 3)
    if op2 in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x64, 0x65,
               0x66, 0xDB, 0xDF, 0xEB, 0xEF, 0xD4, 0xF8, 0xF9, 0xFA, 0xFB,
               0xFC, 0xFD, 0xFE, 0xD7, 0xC5, 0xC4, 0xD1, 0xD2, 0xD3, 0xE1,
               0xE2, 0xF1, 0xF2, 0xF3, 0xE7, 0xEA, 0xEE, 0xD5, 0xE5, 0xE0,
               0xF5):
        return pfx == 1
    if op2 == 0xE6:
        return pfx == 3
    if op2 == 0xAE:
        return pfx == 0
    return False


def split_prefixes(bs):
    pfx = 0
    i = 0
    while i < len(bs):
        b = bs[i]
        if b == 0x66:
            pfx = 1; i += 1
        elif b == 0xF2:
            pfx = 2; i += 1
        elif b == 0xF3:
            pfx = 3; i += 1
        elif b in (0xF0, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x67):
            i += 1
        else:
            break
    return pfx, bs[i:]


SSE_OP2 = (set(range(0x10, 0x18)) | set(range(0x28, 0x30)) |
           set(range(0x50, 0x80)) | set(range(0xC2, 0xC7)) |
           set(range(0xD0, 0x100)))


def classify(insn):
    """Return (kind, key, detail) for SSE/x87 forms IR may reject."""
    bs = insn.bytes
    pfx, rest = split_prefixes(bs)
    if not rest:
        return None
    if rest[0] == 0x0F and len(rest) >= 2:
        op2 = rest[1]
        if op2 not in SSE_OP2:
            return None
        no_pfx = (0x10 <= op2 <= 0x17 or 0x28 <= op2 <= 0x2F or
                  0x50 <= op2 <= 0x5F or op2 in (0xC2, 0xC6, 0xAE))
        mmx_only = (pfx == 0 and 0x60 <= op2 <= 0x7F)
        if mmx_only and insn.mnemonic not in ("emms", "femms"):
            return None            # MMX, tracked separately from SSE
        if op2 not in (0x77,) and (pfx != 0 or no_pfx) and not mmx_only \
                and not sse_supported(op2, pfx):
            names = {0: "np", 1: "66", 2: "F2", 3: "F3"}
            return ("sse", "SSE %s 0F %02X %s" % (names[pfx], op2, insn.mnemonic),
                    {"op2": op2, "pfx": pfx, "mnemonic": insn.mnemonic})
        return None
    if 0xD8 <= rest[0] <= 0xDF and len(rest) >= 2:
        if not x87_form_ok(rest[0], rest[1]):
            return ("x87", "x87 %02X /%d %s" % (rest[0], (rest[1] >> 3) & 7, insn.mnemonic),
                    {"op": rest[0], "modrm": rest[1], "mnemonic": insn.mnemonic})
    return None


# --------------------------------------------------------------- audit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=str(ROOT / "build_host" / "havok_audit.json"))
    ap.add_argument("--subset", default=str(ROOT / "build_host" / "havok_functions.json"))
    ap.add_argument("--ranges", default=str(ROOT / "build_host" / "havok_ranges.txt"))
    args = ap.parse_args()

    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False

    binb, lo, text_size = load_image()
    exe, image_base, secs = load_pe_sections(EXE)
    text = binb[16:16 + text_size]
    text_base = lo

    fj = json.loads(FUNCS.read_text(encoding="utf-8"))
    funcs = {int(k, 16): v for k, v in fj["functions"].items()}
    starts = sorted(funcs)
    # Function start -> (end, ranges)
    ranges = {}
    for fa in starts:
        rs = funcs[fa]
        if rs and isinstance(rs[0], list):
            rs = [tuple(r) for r in rs]
            ranges[fa] = (max(r[1] for r in rs), rs)
        else:
            ranges[fa] = (rs[1], [tuple(rs)])

    def func_at(va):
        i = bisect.bisect_right(starts, va) - 1
        if i < 0:
            return None
        fa = starts[i]
        end, rs = ranges[fa]
        if va < end:
            return fa
        return None

    # ---- strings in TESV .rdata/.data
    cand = {}
    for name, base, size, rsize, rp in secs:
        if name not in (".rdata", ".data"):
            continue
        raw = exe[rp:rp + rsize]
        for m in re.finditer(rb"[\x20-\x7e]{5,120}", raw):
            s = m.group().decode("latin1")
            if HAVOK_RE.search(s) or RTTI_RE.match(s):
                cand[base + m.start()] = s

    # ---- RTTI vtables
    vtables = {}
    td_cache = {}
    # Build VA -> file offset lookup for the two data sections.
    data_secs = [(b, s, rp) for n, b, s, _rs, rp in secs
                 if n in (".rdata", ".data")]

    def read32(va):
        for b, s, rp in data_secs:
            if b <= va < b + s - 3 and rp + va - b + 4 <= len(exe):
                return struct.unpack_from("<I", exe, rp + va - b)[0]
        return None

    def cstr(va):
        if va in td_cache:
            return td_cache[va]
        o = va_off(secs, va)
        if o is None:
            return None
        z = exe.find(b"\0", o, o + 256)
        if z < 0:
            return None
        s = exe[o:z].decode("latin1", "replace")
        td_cache[va] = s
        return s

    for base, size, rp in data_secs:
        avail = min(size, len(exe) - rp)
        for k in range(0, avail - 4, 4):
            v = struct.unpack_from("<I", exe, rp + k)[0]
            if not (image_base <= v < image_base + 0x1800000):
                continue
            td = read32(v + 12)
            col = cstr((td + 8) if td else 0)
            if col and col.startswith(".?AV") and RTTI_RE.match(col):
                vt = base + k + 4
                entries = []
                j = 0
                while j < 512:
                    fn = read32(vt + 4 * j)
                    if fn is None or fn not in funcs:
                        break
                    entries.append(fn)
                    j += 1
                if entries:
                    vtables[vt] = (col, entries)

    # ---- code references to Havok strings and vtables
    targets = set(cand)
    for vt in vtables:
        targets.add(vt)
    refs = collections.defaultdict(list)   # target -> [code VA]
    tset = targets
    for i in range(0, len(text) - 3, 4):
        v = struct.unpack_from("<I", text, i)[0]
        if v in tset:
            refs[v].append(text_base + i)

    havok = set()
    for vt, (_n, entries) in vtables.items():
        havok.update(fn for fn in entries if fn in funcs)
    for tgt, sites in refs.items():
        for site in sites:
            fa = func_at(site)
            if fa is not None:
                havok.add(fa)

    # Cluster into ranges.  Havok is statically linked as a dense code block;
    # a 4 KiB gap keeps it one cluster without chaining through the rest of
    # .text via sparse string references.
    hs = sorted(havok)
    clusters = []
    for fa in hs:
        end = ranges[fa][0]
        if clusters and fa - clusters[-1][1] <= 0x1000:
            clusters[-1][1] = max(clusters[-1][1], end)
        else:
            clusters.append([fa, end])

    # Every function that starts inside a Havok cluster is part of the module
    # address space, even if no string/vtable directly names it.
    region = []
    ci = 0
    for fa in starts:
        while ci < len(clusters) and clusters[ci][1] < fa:
            ci += 1
        if ci < len(clusters) and clusters[ci][0] <= fa <= clusters[ci][1]:
            region.append(fa)

    # ---- IR invalid verdicts
    invalid = {}
    with INVALID.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            invalid[int(row["addr"], 16)] = row["error"]

    # ---- decode and classify
    counts = collections.Counter()
    by_func = collections.defaultdict(collections.Counter)
    ir_bad = []
    total_insns = 0
    for fa in region:
        end, rs = ranges[fa]
        bad = collections.Counter()
        for (a, b) in rs:
            if b - a > 0x10000:
                b = a + 0x10000
            for insn in md.disasm(text[a - text_base:b - text_base], a):
                total_insns += 1
                c = classify(insn)
                if c:
                    counts[c[1]] += 1
                    bad[c[1]] += 1
                    by_func[fa][c[1]] += 1
        if bad:
            by_func[fa] = bad
        if fa in invalid:
            ir_bad.append((fa, invalid[fa]))

    # ---- output
    report = {
        "image": str(IMAGE),
        "image_text_size": text_size,
        "exe": str(EXE),
        "functions_total": len(funcs),
        "havok_strings": len(cand),
        "havok_vtables": len(vtables),
        "havok_functions": len(hs),
        "havok_region_functions": len(region),
        "havok_clusters": [[hex(a), hex(b)] for a, b in clusters],
        "decoded_insns_in_havok": total_insns,
        "ir_rejected_havok_functions": len(ir_bad),
        "ir_rejected": [[hex(a), e] for a, e in sorted(ir_bad)],
        "unsupported_forms": [
            {"form": k, "count": v,
             "functions": [hex(f) for f, _c in by_func.items() if k in by_func[f]][:20]}
            for k, v in counts.most_common()
        ],
        "vtable_names": sorted({n for n, _e in vtables.values()})[:200],
        "string_samples": [{"va": hex(a), "s": s} for a, s in sorted(cand.items())[:80]],
    }
    out = Path(args.json)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")

    rng = Path(args.ranges)
    rng.write_text("".join("%08x-%08x\n" % (a, b) for a, b in clusters), encoding="utf-8")

    # ---- lifter subset
    sub = dict(fj)
    sub["functions"] = {("%x" % fa): funcs[fa] for fa in region}
    # Keep only IAT/jump-table entries that the subset can reference; keeping
    # all of them is harmless and matches the original schema.
    Path(args.subset).write_text(json.dumps(sub), encoding="utf-8")

    print("havok strings     :", len(cand))
    print("havok RTTI vtables:", len(vtables))
    print("havok functions   :", len(hs), "direct,", len(region), "in", len(clusters), "clusters")
    print("decoded insns     :", total_insns)
    print("IR-rejected Havok :", len(ir_bad), "of", len(region))
    print("unsupported forms :", len(counts))
    for k, v in counts.most_common(15):
        print("  %-42s %6d" % (k, v))
    print("wrote", out, rng, args.subset)


if __name__ == "__main__":
    main()
