"""Measure the real IR decoder on a real slice.

The decoder is the C implementation in source/ir_decode.c; this tool only
feeds it the executable bytes and the function ranges from functions.json, then
reports the bytes it produced. No number in docs/IR.md comes from a Python
re-implementation of the format.

Usage:
  python tools/ir_measure.py
  python tools/ir_measure.py --functions <functions.json> --exe E:/skyr/TESV.exe
  python tools/ir_measure.py --profile <profile.csv> --cache 8388608

Defaults mirror tools/codegen_bench.ps1: the 2,000-function slice under
E:/Skyrim_N3DS_build/cgbench/slice and the real TESV.exe.
"""
import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "staged"))

CLANG = Path(r"C:\Program Files\LLVM\bin\clang.exe")
AOT_BPI = 25.35           # measured, docs/CODEGEN.md 9000-function build
CLOSURE_INSNS = 2_900_299  # docs/HYBRID_EXECUTION.md


def read_profile(path):
    rows = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.lower().startswith("addr,"):
            continue
        a, h = line.split(",")[:2]
        rows.append((int(a, 16), int(h)))
    return rows


def function_ranges(functions, wanted=None):
    """Returns ([(entry, function_end, [(start,end)])], x86_bytes, function_count)."""
    out = []
    total = 0
    n = 0
    for key, ranges in functions["functions"].items():
        addr = int(key, 16)
        if wanted is not None and addr not in wanted:
            continue
        n += 1
        blocks = []
        for lo, hi in ranges:
            lo, hi = int(lo), int(hi)
            if hi <= lo:
                continue
            blocks.append((lo, hi))
            total += hi - lo
        if blocks:
            blocks.sort()
            out.append((addr, max(hi for _, hi in blocks), blocks))
    out.sort()
    return out, total, n


def build_input(exe, funcs, work, name="ir_measure_input.bin"):
    import peimg
    pe, base, img = peimg.load(str(exe), None)
    _ = pe
    img = bytes(img)
    lo = min(a for _, _, blocks in funcs for a, _ in blocks)
    hi = max(b for _, _, blocks in funcs for _, b in blocks)
    blob = img[lo - base:hi - base]
    payload = struct.pack("<IIII", 0x49524d31, lo, len(blob), len(funcs))
    payload += blob
    for entry, fend, blocks in funcs:
        payload += struct.pack("<III", entry, fend, len(blocks))
        payload += b"".join(struct.pack("<II", a, b) for a, b in blocks)
    path = work / name
    path.write_bytes(payload)
    return path


def classify_function(insns):
    """Return per-class instruction counts for one function's disassembly."""
    from capstone import x86
    out = {"x87": 0, "sse": 0, "mmx": 0, "other": 0}
    detail = {"x87": {}, "sse": {}, "mmx": {}, "other": {}}
    for i in insns:
        g = set(i.groups)
        b = i.bytes
        k = 0
        while k < len(b) and b[k] in (0x9b, 0x66, 0x67, 0xf0, 0xf2, 0xf3,
                                      0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65):
            k += 1
        op = b[k] if k < len(b) else 0
        if x86.X86_GRP_FPU in g or 0xD8 <= op <= 0xDF:
            cls = "x87"
            reg = (b[k + 1] >> 3) & 7 if k + 1 < len(b) else 0
            key = f"{op:02x}/{reg}"
        elif x86.X86_GRP_MMX in g:
            cls = "mmx"
            key = f"0f{op2:02x}" if (op2 := (b[k + 1] if k + 1 < len(b) else 0)) else "?"
        elif any(getattr(x86, n, -1) in g for n in
                 ("X86_GRP_SSE1", "X86_GRP_SSE2", "X86_GRP_SSE3",
                  "X86_GRP_SSSE3", "X86_GRP_SSE41", "X86_GRP_SSE42")):
            cls = "sse"
            key = f"0f{b[k + 1]:02x}" if k + 1 < len(b) else "?"
        else:
            cls = "other"
            key = i.mnemonic
        out[cls] += 1
        detail[cls][key] = detail[cls].get(key, 0) + 1
    return out, detail


def histogram(exe, functions, fail_path):
    import peimg
    import capstone
    from capstone import x86
    _ = x86
    pe, base, img = peimg.load(str(exe), None)
    _ = pe
    img = bytes(img)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    fails = [int(l.strip(), 16) for l in Path(fail_path).read_text(encoding="utf-8").splitlines()
             if l.strip()]
    fn_has = {"x87": 0, "sse": 0, "mmx": 0, "other": 0}
    ins = {"x87": 0, "sse": 0, "mmx": 0, "other": 0}
    det = {"x87": {}, "sse": {}, "mmx": {}, "other": {}}
    missing = 0
    for addr in fails:
        ranges = functions.get(f"0x{addr:x}")
        if not ranges:
            missing += 1
            continue
        all_insns = []
        for lo, hi in ranges:
            lo, hi = int(lo), int(hi)
            all_insns.extend(md.disasm(img[lo - base:hi - base], lo))
        counts, detail = classify_function(all_insns)
        for cls, n in counts.items():
            ins[cls] += n
            if n:
                fn_has[cls] += 1
            for k, v in detail[cls].items():
                det[cls][k] = det[cls].get(k, 0) + v
    print(f"\nopcode classes in {len(fails)} unsupported functions "
          f"({missing} missing from functions.json):")
    for cls in ("x87", "sse", "mmx", "other"):
        print(f"  {cls:5s} functions {fn_has[cls]:6d}  instructions {ins[cls]:9d}")
    for cls in ("x87", "sse", "mmx", "other"):
        if not det[cls]:
            continue
        top = sorted(det[cls].items(), key=lambda kv: (-kv[1], kv[0]))[:20]
        print(f"  {cls} top: " + ", ".join(f"{k}={v}" for k, v in top))
    return {"functions": fn_has, "instructions": ins,
            "detail": {k: dict(v) for k, v in det.items()}}


def arm_size():
    """Compile the three IR sources for ARM11 and report .text."""
    gcc = Path(r"C:\devkitPro\devkitARM\bin\arm-none-eabi-gcc.exe")
    size = Path(r"C:\devkitPro\devkitARM\bin\arm-none-eabi-size.exe")
    if not gcc.exists() or not size.exists():
        print("ARM size: arm-none-eabi toolchain not found")
        return None
    import tempfile
    tmp = Path(tempfile.mkdtemp(prefix="irarm_"))
    total = 0
    rows = []
    for name in ("ir_core.c", "ir_decode.c", "ir_exec.c"):
        obj = tmp / (name + ".o")
        cmd = [str(gcc), "-O2", "-g", "-std=gnu11", "-march=armv6k", "-mtune=mpcore",
               "-mfloat-abi=hard", "-mtp=soft", "-D__3DS__", "-fwrapv",
               "-fno-strict-aliasing", f"-I{ROOT / 'source'}", f"-I{ROOT / 'staged'}",
               "-IC:/devkitPro/libctru/include", "-c", str(ROOT / "source" / name),
               "-o", str(obj)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            print(f"ARM size: {name} failed\n{p.stderr}")
            return None
        out = subprocess.run([str(size), str(obj)], capture_output=True, text=True).stdout
        line = out.splitlines()[1].split()
        n = int(line[0])
        rows.append((name, n))
        total += n
    print("ARM11 interpreter code (-O2, .text):")
    for name, n in rows:
        print(f"  {name:14s} {n:8,d} B")
    print(f"  {'total':14s} {total:8,d} B ({total / 2**20:.2f} MiB)")
    return total


def compile_harness(work):
    exe = work / "ir_measure.exe"
    srcs = [str(ROOT / "source" / "ir_decode.c"), str(ROOT / "tests" / "ir_measure.c")]
    cmd = [str(CLANG), "-std=gnu11", "-O2", "-g", f"-I{ROOT / 'staged'}",
           f"-I{ROOT / 'source'}", "-fwrapv", "-fno-strict-aliasing",
           "-Wno-unused-function", "-Wno-unused-variable"] + srcs + ["-o", str(exe)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("clang failed")
    return exe


def union_bytes(ranges):
    spans = sorted((int(a), int(b)) for a, b in ranges)
    total = 0
    lo = hi = None
    for a, b in spans:
        if lo is None:
            lo, hi = a, b
        elif a <= hi:
            hi = max(hi, b)
        else:
            total += hi - lo
            lo, hi = a, b
    if lo is not None:
        total += hi - lo
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, default=Path(r"E:\skyr\TESV.exe"))
    ap.add_argument("--functions", type=Path,
                    default=Path(r"E:\Skyrim_N3DS_build\cgbench\slice\slice_functions.json"))
    ap.add_argument("--profile", type=Path,
                    help="restrict to the functions named by this addr,hits CSV")
    ap.add_argument("--work", type=Path, default=Path(r"E:\Skyrim_N3DS_build\irmeasure"))
    ap.add_argument("--cache", type=int, default=8 << 20,
                    help="decode-cache budget to evaluate (default 8 MiB)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--fail-list", type=Path,
                    help="write the entry addresses of functions the v1 decoder cannot fully decode")
    ap.add_argument("--histogram", action="store_true",
                    help="classify the --fail-list functions with capstone (x87/SSE/MMX/other)")
    ap.add_argument("--unique", action="store_true",
                    help="also measure unique basic blocks (discovery ranges are shared across functions)")
    ap.add_argument("--arm-size", action="store_true",
                    help="compile the IR sources for ARM11 and report interpreter .text")
    args = ap.parse_args()

    if not args.exe.exists():
        raise SystemExit(f"missing exe: {args.exe}")
    if not args.functions.exists():
        raise SystemExit(f"missing functions.json: {args.functions}")
    args.work.mkdir(parents=True, exist_ok=True)

    functions = json.loads(args.functions.read_text(encoding="utf-8"))
    wanted = None
    profile_rows = []
    if args.profile:
        profile_rows = read_profile(args.profile)
        wanted = {a for a, _ in profile_rows}
        missing = [a for a, _ in profile_rows if f"0x{a:x}" not in functions["functions"]]
        if missing:
            print(f"warning: {len(missing)} profile addresses are not in functions.json")

    ranges, x86_bytes, nfuncs = function_ranges(functions, wanted)
    if not ranges:
        raise SystemExit("no ranges selected")
    inp = build_input(args.exe, ranges, args.work)
    exe = compile_harness(args.work)
    p = subprocess.run([str(exe), str(inp)], capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("measurement harness failed")
    line = [l for l in p.stdout.splitlines() if l.startswith("{")][-1]
    m = json.loads(line)
    failed_addrs = []
    if args.fail_list:
        failed_addrs = [l.split()[1] for l in p.stdout.splitlines() if l.startswith("FAILED ")]
        args.fail_list.parent.mkdir(parents=True, exist_ok=True)
        args.fail_list.write_text("".join(a + "\n" for a in failed_addrs), encoding="utf-8")
        print(f"wrote {len(failed_addrs)} v1-unsupported entry addresses to {args.fail_list}")

    uniq = None
    if args.unique:
        starts = sorted({int(a) for _, _, blocks_ in ranges for a, _ in blocks_})
        fend = max(f for _, f, _ in ranges)
        synth = [(starts[0], fend, [(a, fend) for a in starts])]
        inp2 = build_input(args.exe, synth, args.work, "ir_unique_input.bin")
        p2 = subprocess.run([str(exe), str(inp2)], capture_output=True, text=True)
        if p2.returncode != 0:
            print(p2.stdout)
            print(p2.stderr)
            raise SystemExit("unique measurement failed")
        uniq = json.loads([l for l in p2.stdout.splitlines() if l.startswith("{")][-1])
        ubpi = uniq["ir_bytes"] / uniq["insns"] if uniq["insns"] else 0.0
        print(f"unique blocks: {uniq['blocks']:,} blocks, {uniq['insns']:,} instructions, "
              f"{uniq['ir_bytes']:,} IR bytes ({ubpi:.2f} B/insn), "
              f"{uniq['failed_blocks']} undecodable blocks")

    insns = m["insns"]
    ir_bytes = m["ir_bytes"]
    bpi = ir_bytes / insns if insns else 0.0
    blocks = m["blocks"]
    failed_funcs = m["failed_functions"]
    proj_bpi = (uniq["ir_bytes"] / uniq["insns"]) if uniq and uniq["insns"] else bpi

    all_ranges = [r for _, _, blocks_ in ranges for r in blocks_]
    total_union = union_bytes(all_ranges)
    failed_set = {int(a, 16) for a in failed_addrs}
    fail_ranges = [r for entry, _, blocks_ in ranges if entry in failed_set for r in blocks_]
    fail_union = union_bytes(fail_ranges)

    print(f"input: {args.functions}")
    print(f"functions: {nfuncs} ({m['functions_ok']} decode)  "
          f"x86 bytes: {x86_bytes:,} (sum over functions)  union: {total_union:,}")
    print(f"decoded (per-function pass): {insns:,} x86 instructions in {blocks:,} IR blocks")
    print(f"IR bytes (per-function pass): {ir_bytes:,}  ({bpi:.2f} B per x86 instruction)")
    print(f"functions with an unsupported opcode: {failed_funcs}; "
          f"their union bytes: {fail_union:,} ({100.0 * fail_union / max(total_union, 1):.2f}% of union)")
    if failed_funcs:
        print(f"first unsupported: {m['first_error']}")

    if insns:
        print()
        print(f"closure projection (2,900,299 x86 instructions, {proj_bpi:.2f} B/insn):")
        for label, aot_share in (("100% IR", 0.0), ("20% AOT / 80% IR", 0.2),
                                 ("50% AOT / 50% IR", 0.5), ("100% AOT", 1.0)):
            aot = CLOSURE_INSNS * aot_share * AOT_BPI
            ir = CLOSURE_INSNS * (1.0 - aot_share) * proj_bpi
            total = aot + ir
            print(f"  {label:18s} AOT {aot / 2**20:7.1f} MiB + IR {ir / 2**20:7.1f} MiB"
                  f" = {total / 2**20:7.1f} MiB")

    # Decode-cache working set: blocks are decoded once and never evicted while
    # the working set fits. A whole-arena flush is the eviction policy.
    if blocks:
        print()
        print(f"decode cache: {args.cache / 2**20:.1f} MiB holds "
              f"{int(args.cache / max(bpi, 0.001)):,} instructions "
              f"({int(args.cache / max(ir_bytes / blocks, 1.0)):,} blocks at "
              f"{ir_bytes / blocks:.1f} B/block); this run needs "
              f"{ir_bytes / 2**20:.2f} MiB for the whole selected set")
        if args.profile and profile_rows:
            total_hits = sum(h for _, h in profile_rows)
            print(f"profile: {len(profile_rows)} functions, {total_hits} calls; "
                  f"the selected working set fits in {ir_bytes / 2**20:.2f} MiB")
            if ir_bytes > args.cache:
                print("  WARNING: profile working set exceeds the cache budget; "
                      "the arena will flush")
    if args.json:
        print(json.dumps({"insns": insns, "ir_bytes": ir_bytes, "bpi": bpi,
                          "blocks": blocks, "failed_functions": failed_funcs,
                          "x86_bytes": x86_bytes, "functions": nfuncs}))
    if args.arm_size:
        arm_size()
    if args.histogram:
        if not args.fail_list:
            raise SystemExit("--histogram requires --fail-list")
        hist = histogram(args.exe, functions["functions"], args.fail_list)
        if args.json:
            print(json.dumps({"histogram": hist}))


if __name__ == "__main__":
    main()
