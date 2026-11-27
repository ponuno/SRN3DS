"""Standalone differential + integrity tests for source/ir_container.c.

Run from the repository root:

    .venv/Scripts/python.exe tests/ir_container_test.py
    .venv/Scripts/python.exe tests/ir_container_test.py --keep

It compiles the real C decoder/packer, builds a container from the self-authored
tests/ir_corpus.h image (no game data), then runs:

  * AOT vs IR vs container equivalence for every corpus function;
  * explicit unsupported-entry faults;
  * AOT overlay precedence and no duplicate dispatch;
  * no runtime re-decode (decode-cache block counter + stats.redecodes);
  * open/lookup/LRU/pin/unpin/eviction;
  * header/version/index-CRC/record-CRC/truncated/short-read failures.

No file outside the build directory is modified.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLANG = Path(r"C:\Program Files\LLVM\bin\clang.exe")

# Must match source/ir_container.h / tests/ir_container_integrity.c
ERR_OPEN = -2
ERR_HEADER = -3
ERR_VERSION = -4
ERR_INDEX_CRC = -5
ERR_RECORD_SHORT = -9
ERR_RECORD_CRC = -10
ERR_UNDECODED = -12


def parse_corpus(path):
    text = Path(path).read_text(encoding="utf-8")
    base = int(re.search(r"#define IR_CORPUS_BASE\s+0x([0-9a-fA-F]+)", text).group(1), 16)
    size = int(re.search(r"#define IR_CORPUS_BYTES\s+(\d+)", text).group(1))
    body = re.search(r"ir_corpus_image\[IR_CORPUS_BYTES\] = \{(.*?)\};", text, re.S).group(1)
    image = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))
    if len(image) != size:
        raise SystemExit(f"corpus image size {len(image)} != {size}")
    fns = [(int(a, 16), int(b, 16), name) for a, b, name in
           re.findall(r"\{0x([0-9a-fA-F]+)u,\s*0x([0-9a-fA-F]+)u,\s*\"([^\"]+)\"\}", text)]
    macros = {}
    for name, val in re.findall(r"#define (IR_[A-Z_]+)\s+0x([0-9a-fA-F]+)u", text):
        macros[name] = int(val, 16)
    return base, size, image, fns, macros


def basic_blocks(image, base, start, end):
    """Split [start,end) exactly like tools/ir_make_fixture.basic_blocks."""
    try:
        import capstone
    except ImportError:
        raise SystemExit("capstone is required (use .venv/Scripts/python.exe)")

    data = image[start - base:end - base]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    leaders = {start}
    for insn in md.disasm(data, start):
        nxt = insn.address + insn.size
        tgt = None
        if insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            tgt = insn.operands[0].imm
        m = insn.mnemonic
        if m == "call":
            leaders.add(nxt)
        elif m in ("loop", "jecxz"):
            leaders.add(nxt)
            if tgt is not None and start <= tgt < end:
                leaders.add(tgt)
        elif m == "jmp":
            if tgt is not None and start <= tgt < end:
                leaders.add(tgt)
            leaders.add(nxt)
        elif m.startswith("j"):
            if tgt is not None and start <= tgt < end:
                leaders.add(tgt)
            leaders.add(nxt)
        elif m == "ret":
            leaders.add(nxt)
    bounds = sorted(x for x in leaders if start <= x < end) + [end]
    return [(bounds[k], bounds[k + 1]) for k in range(len(bounds) - 1)]


def callee_end(image, base, start, limit):
    try:
        import capstone
    except ImportError:
        raise SystemExit("capstone is required (use .venv/Scripts/python.exe)")
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for insn in md.disasm(image[start - base:limit - base], start):
        if insn.mnemonic == "ret":
            return insn.address + insn.size
    return limit


def build_input(corpus_path, out_path):
    base, size, image, fns, macros = parse_corpus(corpus_path)
    helper = macros["IR_HELPER_ADDR"]
    helper_end = macros["IR_HELPER_END"]
    callee = macros["IR_CALLEE_ADDR"]
    trap = macros["IR_TRAP_ADDR"]
    trap_end = macros["IR_TRAP_END"]
    unsupported = macros["IR_UNSUPPORTED_ADDR"]
    unsupported_end = macros["IR_UNSUPPORTED_END"]

    funcs = []
    for addr, end, name in fns:
        funcs.append((addr, end, basic_blocks(image, base, addr, end), name))
    cend = callee_end(image, base, callee, helper)
    funcs.append((callee, cend, basic_blocks(image, base, callee, cend), "callee"))
    funcs.append((helper, helper_end,
                  basic_blocks(image, base, helper, helper_end), "irhelper"))
    funcs.append((trap, trap_end, [(trap, trap_end)], "trap"))
    funcs.append((unsupported, unsupported_end, [(unsupported, unsupported_end)],
                  "unsupported"))
    funcs.sort()

    lo = base
    hi = base + size
    payload = struct.pack("<IIII", 0x49524D31, lo, hi - lo, len(funcs))
    payload += image
    for addr, end, blocks, _ in funcs:
        payload += struct.pack("<III", addr, end, len(blocks))
        payload += b"".join(struct.pack("<II", a, b) for a, b in blocks)
    Path(out_path).write_bytes(payload)
    return len(funcs), payload, macros


def build_preentry_input(out_path, include_cold=True):
    """Discovery lists the cold range, but it lies before the callable entry."""
    base = 0x1000
    image = bytes([
        0x8d, 0x42, 0xff, 0x5b, 0xc3,             # cold: lea/pop/ret
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
        0x53, 0x31, 0xc0, 0x74, 0xeb,             # entry: push/xor/je cold
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    ])
    entry, end = 0x1010, 0x1015
    payload = struct.pack('<IIII', 0x49524d31, base, len(image), 1) + image
    if include_cold:
        # The cold leader is present in discovery but precedes the callable
        # entry; the old packer skipped it unconditionally.
        payload += struct.pack('<IIIIIII', entry, end, 2, 0x1000, 0x1005, entry, end)
    else:
        payload += struct.pack('<IIIII', entry, end, 1, entry, end)
    Path(out_path).write_bytes(payload)
    return len(payload)


def run(cmd, cwd=None, env=None):
    p = subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True,
                       text=True, env=env)
    return p


def build(clang, srcs, out, extra=None, work=None):
    cmd = [clang, "-std=gnu11", "-O1", "-g", "-Istaged", "-Isource", "-Itests",
           "-fwrapv", "-fno-strict-aliasing", "-Wno-deprecated-declarations",
           "-Wno-format", "-Wno-unused-function", "-Wno-unused-variable"]
    if extra:
        cmd += extra
    cmd += [str(s) for s in srcs] + ["-o", str(out)]
    p = run(cmd, cwd=work or ROOT)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit(f"build failed: {out}")
    return out


def read_index(path):
    data = Path(path).read_bytes()
    if len(data) < 64 or data[:8] != b"IRCNT001":
        raise SystemExit("bad container")
    nfun = struct.unpack_from("<I", data, 20)[0]
    entry_bytes = struct.unpack_from("<I", data, 36)[0]
    index_off = struct.unpack_from("<I", data, 28)[0]
    if entry_bytes != 40:
        raise SystemExit("bad index entry size")
    out = []
    for i in range(nfun):
        off = index_off + i * 40
        addr, end, flags, status, blocks, insns, rec_off, rec_size, rec_crc, _res = \
            struct.unpack_from("<IIIHHIIIII", data, off)
        out.append({"addr": addr, "end": end, "flags": flags, "status": status,
                    "blocks": blocks, "insns": insns, "record_offset": rec_off,
                    "record_size": rec_size, "record_crc": rec_crc})
    return data, out


def corrupt_variants(work, container, index):
    data = bytearray(Path(container).read_bytes())
    first = next(e for e in index if e["flags"] & 1)
    variants = {}

    d = bytearray(data); d[0] ^= 0xFF
    variants["bad_magic"] = (work / "bad_magic.irc", d, ERR_HEADER)
    d = bytearray(data); struct.pack_into("<I", d, 8, 99)
    variants["bad_version"] = (work / "bad_version.irc", d, ERR_VERSION)
    d = bytearray(data); d[16] ^= 0x01
    variants["bad_header_crc"] = (work / "bad_header_crc.irc", d, ERR_HEADER)
    d = bytearray(data); d[64 + 7] ^= 0x01
    variants["bad_index_crc"] = (work / "bad_index_crc.irc", d, ERR_INDEX_CRC)
    d = bytearray(data); d[first["record_offset"] + 40] ^= 0xFF
    variants["bad_record_crc"] = (work / "bad_record_crc.irc", d, ERR_RECORD_CRC)
    variants["truncated"] = (work / "truncated.irc", data[:128], ERR_HEADER)

    paths = {}
    for name, (path, blob, code) in variants.items():
        Path(path).write_bytes(blob)
        paths[name] = (path, code)
    return paths, first


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--out", type=Path, default=ROOT / "build_ir_container")
    args = ap.parse_args()

    clang = CLANG
    if not clang.exists():
        clang = Path(shutil.which("clang") or "")
    if not clang.exists():
        raise SystemExit("clang not found")

    work = args.out
    if work.exists() and not args.keep:
        shutil.rmtree(work)
    work.mkdir(parents=True, exist_ok=True)

    print(f"work: {work}")
    packer = build(clang, [ROOT / "tests" / "ir_container_pack.c",
                           ROOT / "source" / "ir_decode.c"],
                   work / "ir_container_pack.exe")
    integ = build(clang, [ROOT / "tests" / "ir_container_integrity.c",
                          ROOT / "source" / "ir_container.c",
                          ROOT / "source" / "ir_core.c",
                          ROOT / "source" / "guest.c",
                          ROOT / "staged" / "fpu.c"],
                  work / "ir_container_integrity.exe")
    equiv = build(clang, [ROOT / "tests" / "ir_container_equiv.c",
                          ROOT / "source" / "ir_container.c",
                          ROOT / "source" / "ir_decode.c",
                          ROOT / "source" / "ir_exec.c",
                          ROOT / "source" / "ir_core.c",
                          ROOT / "source" / "guest.c",
                          ROOT / "staged" / "fpu.c",
                          ROOT / "tests" / "ir_aot_fixture.c"],
                  work / "ir_container_equiv.exe")
    preentry = build(clang, [ROOT / "tests" / "ir_container_preentry.c",
                             ROOT / "source" / "ir_container.c",
                             ROOT / "source" / "ir_decode.c",
                             ROOT / "source" / "ir_exec.c",
                             ROOT / "source" / "ir_core.c",
                             ROOT / "source" / "guest.c",
                             ROOT / "staged" / "fpu.c"],
                     work / "ir_container_preentry.exe")
    preentry_dispatch = build(clang, [ROOT / "tests" / "ir_container_preentry_dispatch.c",
                                     ROOT / "source" / "guest.c",
                                     ROOT / "source" / "ir_container.c",
                                     ROOT / "source" / "ir_decode.c",
                                     ROOT / "source" / "ir_exec.c",
                                     ROOT / "source" / "ir_core.c",
                                     ROOT / "source" / "n3ds_lift_runtime.c",
                                     ROOT / "staged" / "fpu.c"],
                              work / "ir_container_preentry_dispatch.exe")
    integration = build(clang, [ROOT / "tests" / "ir_container_integration.c",
                                ROOT / "source" / "guest.c",
                                ROOT / "source" / "ir_container.c",
                                ROOT / "source" / "ir_decode.c",
                                ROOT / "source" / "ir_exec.c",
                                ROOT / "source" / "ir_core.c",
                                ROOT / "source" / "n3ds_lift_runtime.c",
                                ROOT / "staged" / "fpu.c",
                                ROOT / "tests" / "ir_aot_fixture.c"],
                  work / "ir_container_integration.exe")

    inp = work / "corpus_input.bin"
    nfun, payload, macros = build_input(ROOT / "tests" / "ir_corpus.h", inp)
    container = work / "corpus.irc"
    report = work / "corpus_container.json"
    csv = work / "corpus_index.csv"
    p = run([packer, inp, container, "--report", report, "--index-csv", csv])
    print(p.stdout.strip())
    if p.returncode != 0:
        print(p.stderr)
        return 1
    rep = json.loads(report.read_text(encoding="utf-8"))
    print(f"packed: {nfun} functions, {rep['decoded']} decoded, "
          f"{rep['invalid']} invalid, {rep['file_bytes']} bytes")

    print("== pre-entry cold-block regression ==")
    preentry_input = work / "preentry_input.bin"
    build_preentry_input(preentry_input)
    preentry_container = work / "preentry.irc"
    p = run([packer, preentry_input, preentry_container])
    print(p.stdout.rstrip())
    if p.returncode != 0:
        print(p.stderr)
        return 1
    p = run([preentry, preentry_container, "preentry"])
    print(p.stdout.rstrip())
    if p.returncode != 0 or "IR container pre-entry block PASS" not in p.stdout:
        print(p.stderr)
        return 1
    p = run([preentry_dispatch, preentry_container])
    print(p.stdout.rstrip())
    if p.returncode != 0 or "IR container pre-entry dispatch PASS" not in p.stdout:
        print(p.stderr)
        return 1

    missing_input = work / "preentry_missing_input.bin"
    build_preentry_input(missing_input, include_cold=False)
    missing_container = work / "preentry_missing.irc"
    p = run([packer, missing_input, missing_container])
    if p.returncode != 0:
        print(p.stderr)
        return 1
    p = run([preentry, missing_container, "missing"])
    if p.returncode == 0 or "Guest fault at 00001000: unknown function/import" not in p.stderr:
        print("FAIL: an undiscovered pre-entry target was guessed into the frame")
        print(p.stdout)
        print(p.stderr)
        return 1
    print("missing pre-entry leader remains a named refusal PASS")

    print("== differential: AOT vs IR vs container ==")
    p = run([equiv, container])
    print(p.stdout.rstrip())
    if p.returncode != 0:
        print(p.stderr)
        return 1
    if "IR container equivalence PASS" not in p.stdout:
        return 1

    print("== integrated runtime growth-style test ==")
    env = dict(os.environ)
    env["N3DS_IRC_CACHE_RECORDS"] = "1"  # force bounded-cache eviction
    p = run([integration, container], env=env)
    print(p.stdout.rstrip())
    if p.returncode != 0:
        print(p.stderr)
        return 1
    if "IR container integration PASS" not in p.stdout:
        return 1
    if "container->AOT" not in p.stdout:
        return 1
    p = run([integration, container, "container-callee"], env=dict(env, N3DS_IRC_CACHE_RECORDS="2"))
    print(p.stdout.rstrip())
    if p.returncode != 0 or "container->container" not in p.stdout:
        print(p.stderr)
        return 1

    print("== required container named fault ==")
    missing = work / "missing_required.irc"
    env_req = dict(os.environ)
    env_req["N3DS_IRC_REQUIRED"] = "1"
    p = run([integration, missing, "require-missing"], env=env_req)
    print(p.stdout.rstrip())
    if p.returncode != 0 or "IR container unavailable" not in p.stdout:
        print(p.stderr)
        return 1
    bad_req = work / "required_bad.irc"
    blob = bytearray(container.read_bytes())
    blob[64 + 7] ^= 0x01
    bad_req.write_bytes(blob)
    p = run([integration, bad_req, "require-missing"], env=env_req)
    print(p.stdout.rstrip())
    if p.returncode != 0 or "index crc mismatch" not in p.stdout:
        print(p.stderr)
        return 1

    print("== integrity / LRU ==")
    p = run([integ, "open", container])
    print(p.stdout.strip())
    if p.returncode != 0:
        return 1

    data, index = read_index(container)
    first = next(e for e in index if e["flags"] & 1)

    p = run([integ, "cache", container])
    print(p.stdout.strip())
    if p.returncode != 0:
        return 1

    unsupported = macros["IR_UNSUPPORTED_ADDR"]
    p = run([integ, "expect-load", container, f"{unsupported:08x}", str(ERR_UNDECODED)])
    print(p.stdout.strip())
    if p.returncode != 0:
        return 1

    variants, first = corrupt_variants(work, container, index)
    for name, (path, code) in variants.items():
        if code == ERR_RECORD_CRC:
            p = run([integ, "expect-load", path, f"{first['addr']:08x}", str(code)])
        else:
            p = run([integ, "expect-open", path, str(code)])
        print(f"{name}: {p.stdout.strip()}")
        if p.returncode != 0:
            return 1

    short_copy = work / "short.irc"
    shutil.copyfile(container, short_copy)
    p = run([integ, "short", short_copy, f"{first['addr']:08x}"])
    print(p.stdout.strip())
    if p.returncode != 0:
        return 1

    # The valid container must still open after the deliberate truncation test.
    p = run([integ, "open", container])
    if p.returncode != 0:
        print("valid container was damaged by the short-read test")
        return 1

    missing = work / "does_not_exist.irc"
    p = run([integ, "expect-open", missing, str(ERR_OPEN)])
    print(f"missing: {p.stdout.strip()}")
    if p.returncode != 0:
        return 1

    print("== AOT / forced overlay flags ==")
    aot_list = work / "aot.txt"
    forced_list = work / "forced.txt"
    first_addr = index[0]["addr"]
    aot_list.write_text(f"{first_addr:08x}\n", encoding="ascii")
    forced_list.write_text(f"{unsupported:08x}\n", encoding="ascii")
    overlay = work / "corpus_overlay.irc"
    p = run([packer, inp, overlay, "--aot", aot_list, "--forced", forced_list])
    if p.returncode != 0:
        print(p.stderr)
        return 1
    _data, oindex = read_index(overlay)
    by_addr = {e["addr"]: e for e in oindex}
    if not (by_addr[first_addr]["flags"] & 2):
        print("FAIL: AOT flag missing")
        return 1
    if not (by_addr[unsupported]["flags"] & 4):
        print("FAIL: forced flag missing")
        return 1
    p = run([integ, "open", overlay])
    print(p.stdout.strip())
    if p.returncode != 0:
        return 1
    print("AOT/forced flags ok")

    print("IR container tests PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
