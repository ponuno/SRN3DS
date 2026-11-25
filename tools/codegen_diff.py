"""Differential test for lifter semantics: eager flags vs lazy flags.

Assembles a small, self-authored i386 corpus (no game data), lifts it twice with
staged/lift.py (LIFT_LAZY_FLAGS=1 and 0), builds both host binaries and compares
the complete observable machine state after running every function on the same
deterministic input vectors: all eight GPRs, x87 top, DF and the EFLAGS saved by
`pushfd`/`lahf` at the end of each function.

Any divergence is a lifter bug. The corpus covers the flag producers/consumers
that the lazy analysis reasons about: add/sub/adc/sbb/cmp, logic/test/inc/dec/
neg, immediate and variable shifts/rotates, mul/imul, bt*/clc/stc/cmc, setcc/
cmovcc, jcc, sahf/lahf/pushfd/popfd, comiss/ucomiss, bsf/bsr, loop/jecxz and
repe cmpsb.

Usage: python tools/codegen_diff.py [WORKDIR] [--keep]
Exit code 0 only when the eager and lazy runs are byte-identical.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = ROOT / ".venv" / "Scripts" / "python.exe"
if not PY.exists():
    PY = Path(sys.executable)
CLANG = Path(r"C:\Program Files\LLVM\bin\clang.exe")
OBJCOPY = Path(r"c:\devkitPro\msys2\usr\bin\objcopy.exe")
NM = Path(r"c:\devkitPro\msys2\usr\bin\nm.exe")
VCVARS = Path(r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat")

BASE = 0x401000

# prologue: in = [esp+4], out = [esp+8]
PRO = """
    mov ebp, [esp+8]
    mov edi, [esp+4]
    mov eax, [edi+0]
    mov ebx, [edi+4]
    mov ecx, [edi+8]
    mov edx, [edi+12]
"""
# store registers, then the flags exactly as x86 would observe them
EPI = """
    mov [ebp+0], eax
    mov [ebp+4], ebx
    mov [ebp+8], ecx
    mov [ebp+12], edx
    mov [ebp+16], esi
    mov [ebp+20], edi
    mov [ebp+24], ebp
    pushfd
    pop eax
    mov [ebp+28], eax
    lahf
    movzx eax, ah
    mov [ebp+32], eax
    ret
"""

FUNCS = [
    ("arith_jcc", """
    add eax, ebx
    jz 1f
    sub ecx, edx
1:  adc eax, [edi+16]
    sbb ecx, [edi+20]
    cmp eax, ecx
    jl 2f
    neg edx
2:  inc ebx
    dec edx
    imul eax, ecx
    add eax, edx
    jecxz 3f
    sub ebx, 3
3:  nop
"""),
    ("logic_bits", """
    and eax, ebx
    or ecx, eax
    xor edx, edx
    test eax, ecx
    jnz 1f
    or edx, 1
1:  bt eax, ecx
    bts ebx, 3
    btr ecx, 5
    btc edx, 7
    setc al
    setz bl
    cmovc ecx, edx
    cmovnz eax, ebx
"""),
    ("shifts", """
    shl eax, 3
    shr ecx, 2
    sar eax, cl
    rol ecx, 5
    ror eax, 7
    shld eax, ecx, 4
    shrd ecx, eax, 6
    rcl eax, 1
    rcr ecx, 1
    xor edx, ecx
"""),
    ("mul", """
    imul ecx
    mov esi, eax
    mul ecx
    xor eax, esi
    imul eax, ecx
    imul eax, ecx, 7
    add eax, edx
"""),
    ("byte_word", """
    add al, bl
    adc ah, bh
    sub ax, bx
    cmp al, 0x7f
    setg cl
    movzx edx, al
    movsx esi, bl
    add dl, ch
    sbb cx, dx
    test al, al
    cmovs edx, esi
"""),
    ("flags_save", """
    pushfd
    add eax, 1
    popfd
    jnc 1f
    add ebx, 2
1:  mov ah, 0x8a
    sahf
    lahf
    pushfd
    pop esi
    xor eax, esi
"""),
    ("cmc_chain", """
    clc
    stc
    cmc
    jnc 1f
    add eax, 0x100
1:  cmc
    jc 2f
    add ebx, 1
2:  nop
"""),
    ("inc_preserves_cf", """
    stc
    inc eax
    adc ebx, 0
    dec ecx
    sbb edx, 0
    cmc
    adc eax, ebx
    stc
    inc ecx
    jc 1f
    xor edx, edx
1:  cmp eax, 5
"""),
    ("cmp_setcc", """
    cmp eax, ebx
    seta cl
    setb dl
    setl ch
    setge dh
    cmova eax, ebx
    cmovb ebx, eax
    cmovg ecx, edx
    cmovle edx, ecx
    test ecx, edx
    setne al
"""),
    ("comiss", """
    movd xmm0, eax
    movd xmm1, ebx
    comiss xmm0, xmm1
    jb 1f
    add eax, 1
1:  ucomiss xmm1, xmm0
    seta cl
    comisd xmm0, xmm1
    jne 2f
    add ebx, 1
2:  nop
"""),
    ("bsf_bsr", """
    or ecx, 0x100
    bsf eax, ecx
    bsr ebx, ecx
    shl eax, cl
    shr ebx, cl
    sar ecx, cl
    test eax, ebx
    cmovz eax, ecx
"""),
    ("loop_jecxz", """
    mov ecx, 5
1:  dec eax
    loop 1b
    mov ecx, 0
    jecxz 2f
    add ebx, 9
2:  inc ecx
    loop 2b
"""),
    ("string_cmps", """
    mov esi, edi
    lea edi, [esi+16]
    mov ecx, 8
    repe cmpsb
    pushfd
    pop eax
    mov edx, ecx
"""),
    ("block_flags", """
    cmp eax, ebx
    jg 1f
    test ecx, edx
    jz 2f
    add eax, 1
1:  dec ecx
    jns 3f
    neg edx
    jmp 4f
2:  or ebx, eax
3:  sub ecx, 5
4:  xor edx, edx
    add eax, 3
"""),
    ("call_target", """
    add eax, [esp+4]
    xor ebx, [esp+4]
    ret
""", "bare"),
    ("call_flags", """
    stc
    push 0
    call x_call_target
    add esp, 4
    jc 1f
    mov eax, 0
1:  clc
    push 1
    call x_call_target
    add esp, 4
    jnc 2f
    mov ebx, 0
2:  cmp eax, ebx
    jle 3f
    inc edx
3:  nop
"""),
    ("call_store", """
    mov eax, 5
    mov ebx, 6
    mov ecx, 7
    push 2
    call x_call_target
    add esp, 4
"""),
]


def sh(cmd, cwd=None, env=None):
    p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, shell=isinstance(cmd, str))
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit(f"command failed ({p.returncode}): {cmd if isinstance(cmd, str) else ' '.join(map(str, cmd))}")
    return p.stdout


def build_pe(text, funcs, outdir):
    """One .text section at BASE. funcs: {addr: [(block_start, block_end)]}."""
    raw = (len(text) + 0x1FF) & ~0x1FF
    image = bytearray(0x200 + raw)
    def w16(p, v): struct.pack_into('<H', image, p, v)
    def w32(p, v): struct.pack_into('<I', image, p, v)
    w16(0, 0x5a4d); w32(0x3c, 0x80)
    image[0x80:0x84] = b'PE\0\0'
    w16(0x84, 0x14c); w16(0x86, 1); w16(0x94, 224); w16(0x96, 0x102)
    o = 0x98
    w16(o, 0x10b); w32(o + 4, 0x200); w32(o + 16, 0x1000); w32(o + 20, 0x1000)
    w32(o + 28, 0x400000); w32(o + 32, 0x1000); w32(o + 36, 0x200); w16(o + 40, 4)
    w16(o + 48, 4); w32(o + 56, 0x2000); w32(o + 60, 0x200); w16(o + 68, 3)
    w32(o + 72, 0x100000); w32(o + 76, 0x1000); w32(o + 80, 0x100000); w32(o + 84, 0x1000); w32(o + 92, 16)
    s = o + 224
    image[s:s + 8] = b'.text\0\0\0'
    w32(s + 8, raw); w32(s + 12, 0x1000); w32(s + 16, raw); w32(s + 20, 0x200); w32(s + 36, 0x60000020)
    image[0x200:0x200 + len(text)] = text
    (outdir / "fixture.exe").write_bytes(image)
    (outdir / "functions.json").write_text(json.dumps({
        "iat": {}, "jump_tables": {},
        "functions": {hex(a): [[x, y] for x, y in blocks] for a, blocks in funcs.items()}},
        indent=0), encoding="utf-8")


def basic_blocks(text):
    """Split assembled .text into per-function basic blocks (offsets from 0)."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    insns = list(md.disasm(text, 0))
    leaders = {0}
    end = len(text)
    for i in insns:
        nxt = i.address + i.size
        op = i.operands[0] if i.operands else None
        tgt = op.imm if op is not None and op.type == capstone.x86.X86_OP_IMM else None
        if i.mnemonic == "call":
            leaders.add(nxt)
        elif i.mnemonic in ("loop", "jecxz"):
            leaders.add(nxt)
            if tgt is not None and 0 <= tgt < end:
                leaders.add(tgt)
        elif i.mnemonic == "jmp":
            if tgt is not None and 0 <= tgt < end:
                leaders.add(tgt)
            leaders.add(nxt)
        elif i.mnemonic.startswith("j"):
            if tgt is not None and 0 <= tgt < end:
                leaders.add(tgt)
            leaders.add(nxt)
        elif i.mnemonic == "ret":
            leaders.add(nxt)
    bounds = sorted(x for x in leaders if 0 <= x < end) + [end]
    return [(bounds[k], bounds[k + 1]) for k in range(len(bounds) - 1)]


HARNESS = r'''/* Generated by tools/codegen_diff.py: deterministic state dump.
 * Self-contained: a flat guest memory and the handful of runtime hooks the
 * generated code needs, so this test does not depend on the Win32/guest sources
 * that unrelated work may be editing. Both codegen variants use the same stubs,
 * so the comparison is purely about generated code semantics. */
#include "x86.h"
#include "rt_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#define MEM_SIZE 0x100000u
static uint8_t mem[MEM_SIZE];

extern const FnEntry g_fn_table_fixture[];
extern const unsigned g_fn_count_fixture;

void *n3ds_guest_ptr(uint32_t addr, size_t n) {
    if ((uint64_t)addr + n > MEM_SIZE) {
        fprintf(stderr, "guest fault at %08X + %u\n", addr, (unsigned)n);
        exit(2);
    }
    return mem + addr;
}
void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c; fprintf(stderr, "unimplemented %08X %s\n", addr, what ? what : "");
    exit(3);
}
void rt_int3(CPU *c, uint32_t addr) { rt_unimpl(c, addr, "int3"); }
static const FnEntry *find_fn(uint32_t addr) {
    for (unsigned i = 0; i < g_fn_count_fixture; ++i)
        if (g_fn_table_fixture[i].addr == addr) return &g_fn_table_fixture[i];
    return NULL;
}
void rt_call(CPU *c, uint32_t addr) {
    const FnEntry *e = find_fn(addr);
    if (!e) rt_unimpl(c, addr, "unknown function");
    e->fn(c);
}
void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache) {
    const FnEntry *e = find_fn(addr);
    if (!e) rt_unimpl(c, addr, "unknown indirect function");
    *cache = e; e->fn(c);
}
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t jb) { rt_unimpl(c, jb, "setjmp"); }
void rt_setjmp3(CPU *c) { rt_unimpl(c, 0, "setjmp3"); }
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d) {
    (void)c; *a = *b = *cc = *d = 0;
}
uint64_t rt_rdtsc(void) { return 0; }

static int invoke(CPU *c, uint32_t addr, const uint32_t *args, unsigned argc) {
    const FnEntry *e = find_fn(addr);
    if (!e) return -1;
    uint32_t sp = c->r[4], base = sp - (argc + 1) * 4;
    if (base + (argc + 1) * 4 > MEM_SIZE) return -1;
    M32(base) = 0xffffffffu;
    for (unsigned i = 0; i < argc; ++i) M32(base + 4 + i * 4) = args[i];
    c->r[4] = base;
    e->fn(c);
    int ok = c->r[4] == base + 4 || c->r[4] == sp;
    c->r[4] = sp;
    return ok ? 0 : -1;
}

static uint32_t rnd(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

int main(void) {
    const uint32_t IN = 0x10000, OUT = 0x20000, SP = 0x80000;
    CPU c; uint32_t r;
    for (unsigned f = 0; f < g_fn_count_fixture; ++f) {
        for (unsigned it = 0; it < 16; ++it) {
            memset(mem + IN, 0, 64);
            memset(mem + OUT, 0, 64);
            uint32_t s = 0x9e3779b9u ^ (g_fn_table_fixture[f].addr * 2654435761u) ^ (it * 0x85ebca6bu);
            for (unsigned w = 0; w < 16; ++w) {
                uint32_t v = rnd(&s);
                if (w % 5 == 0) v = 0;
                if (w % 7 == 0) v = 0x80000000u;
                if (w % 11 == 0) v = 0xffffffffu;
                M32(IN + 4 * w) = v;
            }
            memset(&c, 0, sizeof c);
            c.r[4] = SP;
            c.fpu_cw = 0x37f; c.mxcsr = 0x1f80;
            for (unsigned k = 0; k < 8; ++k) c.r[k] = rnd(&s);
            c.r[4] = SP;
            c.df = rnd(&s) & 1;
            c.eflags_hi = rnd(&s) & 0x200000;
            uint32_t args[2] = {IN, OUT};
            int rc = invoke(&c, g_fn_table_fixture[f].addr, args, 2);
            printf("%X %u rc=%d", g_fn_table_fixture[f].addr, it, rc);
            for (unsigned k = 0; k < 8; ++k) printf(" %08X", c.r[k]);
            printf(" top=%u df=%u hi=%X out=", c.top, c.df, c.eflags_hi);
            for (unsigned k = 0; k < 40; ++k) printf("%02X", M8(OUT + k));
            printf("\n");
        }
    }
    return 0;
}
'''

LEGACY = ["staged/fpu.c"]


def host_compile(srcs, out, incdir):
    flags = ["-std=gnu11", "-O1", "-g", "-Istaged", "-Isource", "-fwrapv", "-fno-strict-aliasing",
             "-Wno-deprecated-declarations", "-Wno-format", "-Wno-unused-function", "-Wno-unused-variable",
             "-I" + str(incdir)]
    args = " ".join(f'"{x}"' for x in [str(CLANG)] + flags + [str(x) for x in srcs] + ["-o", str(out)])
    sh(f'call "{VCVARS}" >nul && {args}')


def main():
    work = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r"E:\Skyrim_N3DS_build\cgbench\diff")
    keep = "--keep" in sys.argv
    if work.exists():
        shutil.rmtree(work)
    (work / "pe").mkdir(parents=True)

    asm = [".intel_syntax noprefix", ".text"]
    names = []
    for i, spec in enumerate(FUNCS):
        name, body = spec[0], spec[1]
        bare = len(spec) > 2 and spec[2] == "bare"
        sym = f"x_{name}"
        names.append(sym)
        asm += [".globl " + sym, sym + ":"]
        asm += [body] if bare else [PRO, body, EPI]
        asm += [".globl " + sym + "_end", sym + "_end:"]
        asm += [".balign 16"]
    (work / "diff.s").write_text("\n".join(asm), encoding="utf-8")

    sh([str(CLANG), "--target=i386-pc-windows-msvc", "-c", str(work / "diff.s"), "-o", str(work / "diff.obj")])
    sh([str(OBJCOPY), "-O", "binary", "--only-section=.text", str(work / "diff.obj"), str(work / "diff.bin")])
    text = (work / "diff.bin").read_bytes()
    nmout = sh([str(NM), str(work / "diff.obj")])
    syms = {}
    for line in nmout.splitlines():
        p = line.split()
        if len(p) == 3 and p[2].startswith("x_"):
            syms[p[2]] = int(p[0], 16)
    funcs = {}
    total = 0
    for i, sym in enumerate(names):
        start = BASE + syms[sym]
        end = BASE + syms[sym + "_end"]
        blocks = [(start + x, start + y) for x, y in basic_blocks(text[start - BASE:end - BASE])]
        funcs[start] = blocks
        total += end - start
    build_pe(text, funcs, work / "pe")
    print(f"corpus: {len(funcs)} functions, {len(text)} x86 bytes, {total} in functions")

    harness = work / "diff_host.c"
    harness.write_text(HARNESS, encoding="utf-8")
    outputs = {}
    modes = (("eager", {"LIFT_LAZY_FLAGS": "0", "LIFT_SPILL": "0", "LIFT_GLOBALREGS": "0"}),
             ("lazy", {"LIFT_LAZY_FLAGS": "1", "LIFT_SPILL": "0", "LIFT_GLOBALREGS": "0"}),
             ("selective", {"LIFT_LAZY_FLAGS": "1", "LIFT_SPILL": "1", "LIFT_GLOBALREGS": "0"}),
             ("global", {"LIFT_LAZY_FLAGS": "1", "LIFT_SPILL": "1", "LIFT_GLOBALREGS": "1"}))
    for mode, vars_ in modes:
        gen = work / mode / "gen"
        gen.mkdir(parents=True)
        env = dict(os.environ, **vars_)
        p = subprocess.run([str(PY), str(ROOT / "staged" / "lift.py"), str(work / "pe" / "fixture.exe"),
                            str(work / "pe" / "functions.json"), str(gen), "fixture"],
                           env=env, capture_output=True, text=True)
        if p.returncode != 0:
            print(p.stdout, p.stderr)
            raise SystemExit(f"lift ({mode}) failed")
        srcs = LEGACY + [harness] + sorted(gen.glob("fixture_*.c"))
        exe = work / f"{mode}.exe"
        host_compile(srcs, exe, work / "pe")
        run = subprocess.run([str(exe)], capture_output=True, text=True, cwd=work)
        if run.returncode != 0:
            print(run.stdout, run.stderr)
            raise SystemExit(f"{mode} run failed")
        outputs[mode] = run.stdout

    ref = outputs["eager"]
    for mode in ("lazy", "selective", "global"):
        if outputs[mode] != ref:
            (work / "eager.txt").write_text(ref, encoding="utf-8")
            (work / f"{mode}.txt").write_text(outputs[mode], encoding="utf-8")
            el, ll = ref.splitlines(), outputs[mode].splitlines()
            shown = 0
            for k in range(min(len(el), len(ll))):
                if el[k] != ll[k]:
                    print(f"DIVERGENCE ({mode}) line {k}:\n  eager: {el[k]}\n  {mode}: {ll[k]}")
                    shown += 1
                    if shown >= 8:
                        break
            raise SystemExit(f"FAIL: {mode} lift disagrees with eager")

    lines = ref.splitlines()
    print(f"PASS: eager, lazy, selective and global agree on {len(lines)} state snapshots "
          f"({len(funcs)} functions x 16 inputs)")
    if not keep:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
