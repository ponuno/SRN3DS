"""Stage a small, single-threaded Skyrim_Android lifter subset without modifying its checkout."""
import argparse
from pathlib import Path
import re
import shutil
import sys


def change(text, old, new, count):
    if text.count(old) != count:
        raise ValueError(f"upstream changed: expected {count} occurrences of {old!r}, found {text.count(old)}")
    return text.replace(old, new)


def stage(upstream, out):
    runtime, tools = upstream / 'runtime', upstream / 'tools'
    x86 = (runtime / 'x86.h').read_text(encoding='utf-8')
    x86 = change(x86, 'extern uint8_t *MEM;', 'void *n3ds_guest_ptr(uint32_t addr, size_t bytes);', 1)
    for typ, size in [('uint8_t', 1), ('u16u', 2), ('u32u', 4), ('u64u', 8), ('f32u', 4), ('f64u', 8)]:
        x86 = change(x86, f'({typ} *)(MEM + (uint32_t)(a))', f'({typ} *)n3ds_guest_ptr((uint32_t)(a), {size})', 1)
    x86 = change(x86, 'void *n3ds_guest_ptr(uint32_t addr, size_t bytes);',
                 'void *n3ds_guest_ptr(uint32_t addr, size_t bytes);\n'
                 'static inline Xmm n3ds_xmm_read(uint32_t addr) { Xmm x; memcpy(&x, n3ds_guest_ptr(addr,16),16); return x; }', 1)
    pre = (runtime / 'lift_prelude.h').read_text(encoding='utf-8')
    pre = change(pre, 'memmove(MEM + dst, MEM + src, (size_t)n * 4);',
                 'memmove(n3ds_guest_ptr(dst,(size_t)n*4), n3ds_guest_ptr(src,(size_t)n*4),(size_t)n*4);', 1)
    pre = change(pre, 'if (dst <= src || dst >= src + n * 4)',
                 'if (!n) return;\n    if (n > UINT32_MAX/4) rt_unimpl(NULL,src,"rep movsd count");\n    (void)n3ds_guest_ptr(dst,(size_t)n*4);\n    (void)n3ds_guest_ptr(src,(size_t)n*4);\n    if (dst <= src || (uint64_t)dst >= (uint64_t)src + (uint64_t)n * 4)', 1)
    # This subset supports only one guest thread. No AArch64 host memory path.
    pre = pre[:pre.index('/* Memory ordering')]+'''/* N3DS single-threaded subset. */
#include "n3ds_atomic.h"
#define ST_FENCE() ((void)0)
#define FULL_FENCE() ((void)0)
#define LD_FENCE() ((void)0)
#define TL8(a) M8(a)
#define TL16(a) M16(a)
#define TL32(a) M32(a)
#define TS8(a,v) (M8(a)=(v))
#define TS16(a,v) (M16(a)=(v))
#define TS32(a,v) (M32(a)=(v))
/* Partial spill masks used by the current lifter and by
   tools/instrument_cecdc0.py (bit i selects CPU.r[i]). */
#define SPILLN(m) do { if ((m) & 1u) c->r[0] = eax; if ((m) & 2u) c->r[1] = ecx; \\
    if ((m) & 4u) c->r[2] = edx; if ((m) & 8u) c->r[3] = ebx; \\
    if ((m) & 16u) c->r[4] = esp; if ((m) & 32u) c->r[5] = ebp; \\
    if ((m) & 64u) c->r[6] = esi; if ((m) & 128u) c->r[7] = edi; } while (0)
#define RELOADN(m) do { if ((m) & 1u) eax = c->r[0]; if ((m) & 2u) ecx = c->r[1]; \\
    if ((m) & 4u) edx = c->r[2]; if ((m) & 8u) ebx = c->r[3]; \\
    if ((m) & 16u) esp = c->r[4]; if ((m) & 32u) ebp = c->r[5]; \\
    if ((m) & 64u) esi = c->r[6]; if ((m) & 128u) edi = c->r[7]; } while (0)
'''
    lift = (tools / 'lift.py').read_text(encoding='utf-8')
    lift = change(lift, '(%s *)(MEM + %s)', 'N3DS_PTR(%s, %s)', 3)
    lift = change(lift, '(%s *)(MEM + la_)', 'N3DS_PTR(%s, la_)', 2)
    lift = change(lift, '(uint32_t *)(MEM + w_)', 'N3DS_PTR(uint32_t, w_)', 1)
    lift = change(lift, '(uint64_t *)(MEM + %s)', 'N3DS_PTR(uint64_t, %s)', 1)
    lift = change(lift, 'memcpy(MEM + %s, &%s, 16)', 'memcpy(n3ds_guest_ptr(%s,16), &%s, 16)', 2)
    lift = change(lift, '(*(Xmm *)(MEM + %s))', 'n3ds_xmm_read(%s)', 2)
    # Only guest-memory atomics, not the native indirect-call pointer cache.
    for old, new, count in [
        ('__atomic_exchange_n(', 'N3DS_EXCHANGE(', 1),
        ('__atomic_fetch_or', 'N3DS_FETCH_OR', 1),
        ('__atomic_fetch_and', 'N3DS_FETCH_AND', 1),
        ('__atomic_fetch_xor', 'N3DS_FETCH_XOR', 1),
        ('__atomic_load_n(N3DS_PTR', 'N3DS_LOAD(N3DS_PTR', 1),
        ('__atomic_compare_exchange_n(N3DS_PTR', 'N3DS_CAS(N3DS_PTR', 3),
        ('__atomic_fetch_add(', 'N3DS_FETCH_ADD(', 1),
    ]:
        lift = change(lift, old, new, count)
    # x87 forms Havok's serialiser/maths use but NET 4.1 missed:
    #   D9 D8..DF  FSTP ST(i)   (capstone calls it fstpnce)
    #   DF /6      FBSTP m80bcd
    lift = change(lift, '"xabort", "int1", "fstpnce", "lock sbb",',
                 '"xabort", "int1", "lock sbb",', 1)
    lift = change(lift, '''            elif modrm == 0xD0:
                pass
            else:
                ONE = {''', '''            elif modrm == 0xD0:
                pass
            elif 0xD8 <= modrm <= 0xDF:
                e("%s = ST(0); fpop(c);" % s)
            else:
                ONE = {''', 1)
    lift = change(lift, '''                elif reg == 5:
                    e("fpush(c, (double)(int64_t)M64(%s));" % a)
                elif reg == 7:
                    e("M64(%s) = (uint64_t)fist64(c, ST(0)); fpop(c);" % a)''',
                 '''                elif reg == 5:
                    e("fpush(c, (double)(int64_t)M64(%s));" % a)
                elif reg == 6:
                    e("fbstp_store(%s, ST(0)); fpop(c);" % a)
                elif reg == 7:
                    e("M64(%s) = (uint64_t)fist64(c, ST(0)); fpop(c);" % a)''', 1)
    # SSE2 packed unsigned saturating add/sub on XMM operands (paddusw was the
    # only SSE instruction Havok still trapped on).
    lift = change(lift, '''        elif mn == "packssdw":
            x = self.xmm(i, ops[0])
            e("{ Xmm d_ = %s, s_ = %s; for (int k_ = 0; k_ < 4; k_++) { int32_t a_ = d_.i[k_], b_ = s_.i[k_]; "''',
                 '''        elif mn in ("paddusb", "paddusw", "psubusb", "psubusw"):
            x = self.xmm(i, ops[0])
            lane, n, lim = ("b", 16, 255) if mn.endswith("b") else ("w", 8, 65535)
            if mn.startswith("padd"):
                body = ("for (int k_ = 0; k_ < %d; k_++) { unsigned v_ = (unsigned)%s.%s[k_] + s_.%s[k_]; "
                        "%s.%s[k_] = (uint%s_t)(v_ > %d ? %d : v_); }" %
                        (n, x, lane, lane, x, lane, "8" if lane == "b" else "16", lim, lim))
            else:
                body = ("for (int k_ = 0; k_ < %d; k_++) { unsigned a_ = %s.%s[k_], b_ = s_.%s[k_]; "
                        "%s.%s[k_] = (uint%s_t)(a_ > b_ ? a_ - b_ : 0); }" %
                        (n, x, lane, lane, x, lane, "8" if lane == "b" else "16"))
            e("{ Xmm s_ = %s; %s }" % (src128(ops[1]), body))
        elif mn == "packssdw":
            x = self.xmm(i, ops[0])
            e("{ Xmm d_ = %s, s_ = %s; for (int k_ = 0; k_ < 4; k_++) { int32_t a_ = d_.i[k_], b_ = s_.i[k_]; "''', 1)
    fpu = (runtime / 'fpu.c').read_text(encoding='utf-8')
    fpu = change(fpu, '#include "rt.h"', '#include "x86.h"', 1)
    fpu = change(fpu, 'memset(MEM + a, 0, op == 0 ? 28 : 108)',
                 'memset(n3ds_guest_ptr(a,op == 0 ? 28 : 108), 0, op == 0 ? 28 : 108)', 1)
    # Havok uses FBSTP (DF /6) to serialise 80-bit BCD.  Keep it here, next to
    # the other x87 helpers, so both AOT and the IR interpreter share it.
    fpu = change(fpu, 'void fxam(CPU *c) {', '''void fbstp_store(uint32_t a, double v) {
    /* FBSTP: 18 packed BCD digits, low byte first, sign in bit 7 of byte 9. */
    int neg = v < 0;
    long double a_ = fabsl((long double)v);
    unsigned long long n = (unsigned long long)nearbyintl(a_);
    for (int i = 0; i < 9; i++) {
        unsigned lo = (unsigned)(n % 10); n /= 10;
        unsigned hi = (unsigned)(n % 10); n /= 10;
        M8(a + i) = (uint8_t)(lo | (hi << 4));
    }
    M8(a + 9) = (uint8_t)(neg ? 0x80 : 0x00);
}

void fxam(CPU *c) {''', 1)
    x86 = change(x86, 'void f80_store(uint32_t addr, double v);',
                 'void f80_store(uint32_t addr, double v);\nvoid fbstp_store(uint32_t addr, double v);', 1)
    for name, data in [('x86.h',x86),('lift_prelude.h',pre),('lift.py',lift),('fpu.c',fpu)]:
        if re.search(r'\bMEM\s*\+',data):
            raise ValueError(f'unhandled flat guest address in {name}')
    compile(lift, 'lift.py', 'exec')
    if out.exists():
        raise ValueError('output already exists; choose a fresh directory')
    out.mkdir(parents=True)
    for name, data in [('x86.h',x86),('lift_prelude.h',pre),('lift.py',lift),('fpu.c',fpu)]:
        (out / name).write_text(data,encoding='utf-8')
    shutil.copyfile(runtime/'rt_table.h',out/'rt_table.h')
    shutil.copyfile(tools/'peimg.py',out/'peimg.py')
    print('Staged lifter, x86 helpers and FPU helpers at',out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('upstream',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    try:
        stage(args.upstream,args.output)
    except (OSError,ValueError) as exc:
        sys.exit(str(exc))
