"""Build the IR equivalence fixture from a self-authored i386 corpus.

Assembles ``corpus.s`` (no game data), splits it into basic blocks, runs the
real staged lift.py to produce the AOT tier, and writes two committed files:

  tests/ir_corpus.h        raw guest bytes + addresses
  tests/ir_aot_fixture.c   generated AOT functions and their FnEntry table

x_ir_helper is assembled in the same .text image but deliberately omitted from
functions.json, so it only exists in the interpreter tier. Its indirect call
from an AOT caller is the AOT -> IR -> AOT test.

Run from the repository root:
    .venv/Scripts/python.exe tools/ir_make_fixture.py
"""
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(os.environ.get("N3DS_IR_FIXTURE_WORK", r"E:\Skyrim_N3DS_build\irfix"))
CLANG = Path(r"C:\Program Files\LLVM\bin\clang.exe")
OBJCOPY = Path(r"c:\devkitPro\msys2\usr\bin\objcopy.exe")
NM = Path(r"c:\devkitPro\msys2\usr\bin\nm.exe")
PY = ROOT / ".venv" / "Scripts" / "python.exe"
if not PY.exists():
    PY = Path(sys.executable)

BASE = 0x501000
PILOT_ADDR = 0x802FA80

PRO = """
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov edi, [ebp+8]
    mov esi, [ebp+12]
    mov eax, [edi+0]
    mov ecx, [edi+4]
    mov edx, [edi+8]
    mov ebx, [edi+12]
"""

EPI = """
    mov [esi+0], eax
    mov [esi+4], ecx
    mov [esi+8], edx
    mov [esi+12], ebx
    mov [esi+16], ebp
    mov [esi+20], edi
    mov [esi+24], esi
    mov [esi+28], esp
    pushfd
    pop eax
    mov [esi+32], eax
    lahf
    movzx eax, ah
    mov [esi+36], eax
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
"""

# name -> body (PRO/EPI are added unless the entry is marked bare)
FUNCS = [
    ("arith", """
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
    ("logic", """
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
    ("muldiv", """
    mov ecx, [edi+4]
    and ecx, 0x7fffffff
    or ecx, 1
    mov eax, [edi+0]
    xor edx, edx
    div ecx
    mov eax, [edi+0]
    cdq
    idiv ecx
    mov eax, [edi+0]
    imul ecx
    mov eax, [edi+0]
    imul eax, ecx
    imul eax, ecx, 7
    mov eax, [edi+0]
    xor edx, edx
    mul ecx
    cwde
    cdq
"""),
    ("byteops", """
    add al, bl
    adc ah, bh
    sub ax, bx
    cmp al, 0x7f
    setg cl
    movzx edx, al
    movsx ebx, bl
    add dl, ch
    sbb cx, dx
    test al, al
    cmovs edx, ebx
    xchg eax, ebx
    bswap eax
    not ecx
    neg dx
    dec cl
    inc ch
"""),
    ("memops", """
    mov ecx, [edi+4]
    and ecx, 3
    lea eax, [edi + ecx*4 + 32]
    mov [eax], edx
    mov ebx, [eax]
    push edx
    push ebx
    pop ecx
    pop edx
    mov eax, [edi+16]
    mov [eax], ecx
    mov ebx, [eax + 8]
    mov [eax + 4], ebx
    mov ecx, [edi+20]
    mov [ecx + 4], eax
    mov ebx, [ecx + 4]
    xchg [ecx], ebx
"""),
    ("strings", """
    mov eax, [edi+16]
    mov ebx, [edi+20]
    mov esi, eax
    mov edi, ebx
    mov ecx, 4
    rep movsd
    mov ecx, 4
    rep stosd
    mov ecx, 8
    repe cmpsb
    mov ecx, 3
    lodsd
    mov ecx, 2
    scasd
    mov edi, [ebp+8]
    mov esi, [ebp+12]
"""),
    ("flags", """
    pushfd
    add eax, 1
    popfd
    jnc 1f
    add ebx, 2
1:  mov ah, 0x8a
    sahf
    lahf
    pushfd
    pop ecx
    xor eax, ecx
    clc
    stc
    cld
    std
    cld
"""),
    # Permanent regression for lift.py's FLAG_EFFECT["cmc"]. Correct x86 gives
    # CF=0 here (clc; stc; cmc). While lift.py models cmc as not reading CF it
    # drops stc and gives CF=1; tests/ir_equiv.c recognises exactly that known
    # divergence and starts failing (as a normal pass) once lift.py is fixed.
    ("cmc", """
    clc
    stc
    cmc
"""),
    ("loopback", """
    mov ecx, 5
1:  add eax, 3
    loop 1b
    mov ecx, 4
    jecxz 2f
    sub ebx, 1
2:  cmp eax, ebx
    jne 3f
    or edx, 1
3:  nop
"""),
    ("direct", """
    push dword ptr [edi+0]
    call x_callee
    add esp, 4
    add eax, ecx
    push dword ptr [edi+4]
    call x_callee
    add esp, 4
    add ebx, eax
"""),
    ("indirect", """
    push dword ptr [edi+0]
    call dword ptr [edi+8]
    add esp, 4
    add ecx, eax
    mov edx, eax
"""),
    ("pilotcall", """
    mov eax, [edi+16]
    push eax
    mov eax, [edi+20]
    push eax
    mov eax, [edi+24]
    push eax
    mov eax, 0x802FA80
    call eax
    mov edx, eax
"""),
    ("trap", """
    mov eax, [edi+24]
    mov eax, [eax]
    add eax, 1
"""),
    ("x87", """
    fld dword ptr [edi+0]
    fstp dword ptr [edi+4]
"""),
    ("x87ops", """
    mov eax, [edi+16]
    fld dword ptr [eax]
    fld qword ptr [eax+8]
    fadd st, st(1)
    fmul st, st(1)
    fsub st, st(1)
    fdiv st, st(1)
    fxch st(1)
    fchs
    fabs
    fcom st(1)
    fnstsw ax
    mov [esi+40], eax
    fnstcw [eax+32]
    fldcw [eax+32]
    fild dword ptr [eax+16]
    fistp dword ptr [eax+20]
    fstp st(0)
    fstp st(0)
    fnclex
    fld dword ptr [eax]
    .byte 0xd9,0xd9          # fstp st(1): the D9 D8..DF register form
    fstp st(0)
    fld dword ptr [eax]
    fbstp tbyte ptr [eax+64]
"""),
    # AOT supports push es (the lifter materialises the selector); v1 IR does
    # not, so this is the permanent explicit-unsupported-opcode test.
    ("unsupported", """
    push es
    add esp, 4
"""),
    ("sseops", """
    mov ecx, [edi+16]
    movaps xmm0, [ecx]
    movaps xmm1, [ecx+16]
    addps xmm0, xmm1
    mulps xmm0, xmm1
    subps xmm0, xmm1
    andps xmm0, xmm1
    xorps xmm0, xmm1
    shufps xmm0, xmm1, 0x1b
    cmpps xmm0, xmm1, 1
    movss xmm2, [ecx]
    addss xmm2, xmm1
    movsd xmm3, [ecx]
    addsd xmm3, xmm1
    movdqa xmm4, [ecx]
    movdqa [ecx+32], xmm4
    pshufd xmm5, xmm4, 0x1b
    pcmpeqd xmm5, xmm4
    pmaxsw xmm5, xmm4
    paddq xmm5, xmm4
    psrlq xmm5, 3
    orps xmm0, xmm1
    ucomiss xmm2, xmm1
    movd xmm6, eax
    movd edx, xmm6
    cvtsi2sd xmm7, eax
    cvttsd2si ebx, xmm3
    movmskps eax, xmm0
    mov [esi+40], eax
    movdqa [ecx+48], xmm0
    movdqa [ecx+64], xmm2
    movdqa [ecx+80], xmm3
    movdqa [ecx+96], xmm5
    cvtdq2ps xmm0, xmm4
    punpcklwd xmm0, xmm1
    punpckhwd xmm0, xmm1
    punpckldq xmm0, xmm1
    punpckhdq xmm0, xmm1
    packssdw xmm0, xmm1
    paddusw xmm0, xmm1
    movntps [ecx+112], xmm0
"""),
    # Every newly wired MMX form, with random register/memory inputs supplied by
    # ir_equiv's four seeded variants. EMMS follows FLD1 so the differential also
    # proves it clears modeled MMX state without resetting the x87 TOP pointer.
    ("mmxops", """
    mov eax, [edi+0]
    mov ecx, [edi+16]
    movq mm0, [ecx]
    movq mm1, [ecx+8]
    movq mm2, mm0
    movq [ecx+32], mm2
    movq mm3, [ecx+16]
    movq mm5, [ecx+24]
    movd mm4, eax
    movd mm6, [ecx+8]
    movd edx, mm4
    movd [ecx+96], mm4
    movd [ecx+108], mm6
    paddq mm0, mm3
    psubq mm1, mm3
    pmullw mm0, mm1
    pmulhw mm1, mm0
    packssdw mm0, mm1
    pand mm0, mm3
    por mm1, mm3
    pxor mm2, mm3
    pcmpeqd mm0, mm3
    pcmpgtd mm1, mm3
    pcmpgtw mm0, mm3
    paddw mm1, mm3
    psubw mm2, mm3
    pavgb mm0, mm1
    psllw mm0, 3
    psrlw mm1, 2
    psraw mm2, 1
    pslld mm0, 4
    psrld mm1, 5
    psrad mm2, 6
    psllq mm0, 7
    psrlq mm1, 8
    psllw mm0, mm5
    psrlw mm1, mm5
    psraw mm2, mm5
    pslld mm0, mm5
    psrld mm1, mm5
    psrad mm2, mm5
    psllq mm0, mm5
    psrlq mm1, mm5
    movq [ecx+32], mm0
    movq [ecx+40], mm1
    movq [ecx+48], mm2
    movq [ecx+56], mm3
    movq [ecx+64], mm4
    movq [ecx+72], mm5
    movq [ecx+80], mm6
    movd [ecx+100], mm0
    fld1
    emms
    """),
    # Long deterministic loops: the ARM benchmark uses these so per-call setup
    # does not hide the tier's throughput.
    ("bench", """
    mov ecx, 2000
1: add eax, ebx
    xor edx, eax
    imul eax, 3
    add ebx, edx
    dec ecx
    jnz 1b
"""),
    ("bench87", """
    mov eax, [edi+16]
    fld dword ptr [eax]
    mov ecx, 1000
1: fld st(0)
    fadd st, st(1)
    fstp st(1)
    dec ecx
    jnz 1b
    fstp st(0)
"""),
    ("bench128", """
    mov eax, [edi+16]
    movaps xmm0, [eax]
    movaps xmm1, [eax+16]
    mov ecx, 1000
1: addps xmm0, xmm1
    mulps xmm0, xmm1
    dec ecx
    jnz 1b
    movaps [eax+32], xmm0
"""),
]

BARE = {
    "callee": """
    push ebp
    mov ebp, esp
    mov eax, [ebp+8]
    imul eax, eax, 3
    add eax, 7
    pop ebp
    ret
""",
    "irhelper": """
    push ebp
    mov ebp, esp
    push dword ptr [ebp+8]
    call x_callee
    add esp, 4
    pop ebp
    ret
""",
}


def sh(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit(f"command failed: {cmd}")
    return p.stdout


def basic_blocks(text):
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


def build_pe(text, funcs, outdir):
    raw = (len(text) + 0x1FF) & ~0x1FF
    image = bytearray(0x200 + raw)

    def w16(p, v): struct.pack_into("<H", image, p, v)
    def w32(p, v): struct.pack_into("<I", image, p, v)

    w16(0, 0x5a4d); w32(0x3c, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    w16(0x84, 0x14c); w16(0x86, 1); w16(0x94, 224); w16(0x96, 0x102)
    o = 0x98
    w16(o, 0x10b); w32(o + 4, 0x200); w32(o + 16, 0x1000); w32(o + 20, 0x1000)
    w32(o + 28, 0x500000); w32(o + 32, 0x1000); w32(o + 36, 0x200); w16(o + 40, 4)
    w16(o + 48, 4); w32(o + 56, 0x2000); w32(o + 60, 0x200); w16(o + 68, 3)
    w32(o + 72, 0x100000); w32(o + 76, 0x1000); w32(o + 80, 0x100000)
    w32(o + 84, 0x1000); w32(o + 92, 16)
    s = o + 224
    image[s:s + 8] = b".text\0\0\0"
    w32(s + 8, raw); w32(s + 12, 0x1000); w32(s + 16, raw); w32(s + 20, 0x200)
    w32(s + 36, 0x60000020)
    image[0x200:0x200 + len(text)] = text
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "fixture.exe").write_bytes(image)
    (outdir / "functions.json").write_text(json.dumps({
        "iat": {}, "jump_tables": {},
        "functions": {hex(a): [[x, y] for x, y in blocks] for a, blocks in funcs.items()},
    }, indent=0), encoding="utf-8")


def main():
    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)

    asm = [".intel_syntax noprefix", ".text"]
    names = []
    for name, body in FUNCS:
        sym = "x_" + name
        names.append(sym)
        asm += [".globl " + sym, sym + ":", PRO, body, EPI]
        asm += [".globl " + sym + "_end", sym + "_end:", ".balign 16"]
    for name, body in BARE.items():
        sym = "x_" + name
        names.append(sym)
        asm += [".globl " + sym, sym + ":", body]
        asm += [".globl " + sym + "_end", sym + "_end:", ".balign 16"]
    (WORK / "corpus.s").write_text("\n".join(asm), encoding="utf-8")

    obj = WORK / "corpus.obj"
    bin_ = WORK / "corpus.bin"
    sh([str(CLANG), "--target=i386-pc-windows-msvc", "-c", str(WORK / "corpus.s"), "-o", str(obj)])
    sh([str(OBJCOPY), "-O", "binary", "--only-section=.text", str(obj), str(bin_)])
    text = bin_.read_bytes()
    if len(text) > 0x8000:
        raise SystemExit("corpus too large")

    syms = {}
    for line in sh([str(NM), str(obj)]).splitlines():
        p = line.split()
        if len(p) == 3 and p[2].startswith("x_"):
            syms[p[2]] = int(p[0], 16)

    funcs = {}
    ends = {}
    for sym in names:
        if sym == "x_irhelper":
            continue
        start = BASE + syms[sym]
        end = BASE + syms[sym + "_end"]
        blocks = [(start + a, start + b) for a, b in basic_blocks(text[syms[sym]:syms[sym + "_end"]])]
        funcs[start] = blocks
        ends[start] = end
    build_pe(text, funcs, WORK / "pe")

    gen = WORK / "gen"
    gen.mkdir()
    env = dict(os.environ)
    env.update({"LIFT_LAZY_FLAGS": "1", "LIFT_SPILL": "1", "LIFT_GLOBALREGS": "1"})
    p = subprocess.run([str(PY), str(ROOT / "staged" / "lift.py"),
                        str(WORK / "pe" / "fixture.exe"),
                        str(WORK / "pe" / "functions.json"), str(gen), "ira"],
                       env=env, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("lift.py failed")
    print(p.stdout.strip())

    # Combine the generated function bodies and table into one committed file.
    bodies = []
    for f in sorted(gen.glob("ira_*.c")):
        if f.name == "ira_table.c":
            continue
        for line in f.read_text(encoding="utf-8").splitlines():
            if line.startswith("#include"):
                continue
            bodies.append(line)
    table_lines = [l for l in (gen / "ira_table.c").read_text(encoding="utf-8").splitlines()
                   if l.strip().startswith("{")]
    count = len(table_lines)

    fixture = ["/* Generated by tools/ir_make_fixture.py from corpus.s. Do not edit. */",
               '#include "lift_prelude.h"', '#include "ir_corpus.h"', ""]
    protos = [l for l in (gen / "funcs_ira.h").read_text(encoding="utf-8").splitlines()
              if l.startswith("void f_")]
    fixture += protos + [""]
    fixture += bodies
    fixture += ["", "const FnEntry g_fn_table_ira[] = {"]
    fixture += table_lines
    fixture += ["};", f"const unsigned g_fn_count_ira = {count};", ""]
    (ROOT / "tests" / "ir_aot_fixture.c").write_text("\n".join(fixture), encoding="utf-8")

    # corpus header
    hdr = ["/* Generated by tools/ir_make_fixture.py. Do not edit. */",
           "#pragma once", "#include <stdint.h>", "",
           f"#define IR_CORPUS_BASE 0x{BASE:08x}u",
           f"#define IR_CORPUS_BYTES {len(text)}u",
           f"#define IR_PILOT_ADDR 0x{PILOT_ADDR:08x}u",
           "static const uint8_t ir_corpus_image[IR_CORPUS_BYTES] = {"]
    for i in range(0, len(text), 16):
        hdr.append("    " + ",".join(f"0x{b:02x}" for b in text[i:i + 16]) + ",")
    hdr += ["};", "",
            "typedef struct { uint32_t addr, end; const char *name; } IrCorpusFn;",
            "static const IrCorpusFn ir_corpus_fns[] = {"]
    for name, _ in FUNCS:
        if name in ("trap", "x87", "unsupported"):
            continue
        start = BASE + syms["x_" + name]
        hdr.append(f'    {{0x{start:08x}u, 0x{ends[start]:08x}u, "{name}"}},')
    hdr += ["};",
            "#define IR_CORPUS_FN_COUNT (sizeof(ir_corpus_fns)/sizeof(ir_corpus_fns[0]))",
            f"#define IR_HELPER_ADDR 0x{BASE + syms['x_irhelper']:08x}u",
            f"#define IR_HELPER_END  0x{BASE + syms['x_irhelper_end']:08x}u",
            f"#define IR_TRAP_ADDR   0x{BASE + syms['x_trap']:08x}u",
            f"#define IR_TRAP_END    0x{BASE + syms['x_trap_end']:08x}u",
            f"#define IR_UNSUPPORTED_ADDR 0x{BASE + syms['x_unsupported']:08x}u",
            f"#define IR_UNSUPPORTED_END  0x{BASE + syms['x_unsupported_end']:08x}u",
            f"#define IR_CALLEE_ADDR 0x{BASE + syms['x_callee']:08x}u",
            ""]
    (ROOT / "tests" / "ir_corpus.h").write_text("\n".join(hdr), encoding="utf-8")
    print(f"wrote tests/ir_corpus.h ({len(text)} bytes) and tests/ir_aot_fixture.c "
          f"({count} AOT functions)")


if __name__ == "__main__":
    main()
