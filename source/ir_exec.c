/* IR interpreter: executes the compact decoded form with the same CPU state,
 * the same checked guest-memory helpers and the same flag/FPU helpers as AOT
 * code. Flags live in a per-function local exactly as in generated AOT code,
 * so a call in either direction preserves the caller's flags and gives the
 * callee a fresh {0} flag set (the project's AOT convention).
 */
#include "ir_core.h"
#include "guest.h"
#include "n3ds_atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef N3DS_WATCH_STORE_CHECKS
#define IR_WATCH(s) guest_watch_check_active(s)
#else
#define IR_WATCH(s) ((void)0)
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>

#define X87_PI 3.14159265358979323846
#define X87_LN2 0.69314718055994530942
#define X87_L2E 1.44269504088896340736
#define X87_L2T 3.32192809488736234787
#define X87_LG2 0.30102999566398119521

void rt_unimpl(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));
void rt_int3(CPU *c, uint32_t addr) __attribute__((noreturn));
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d);
uint64_t rt_rdtsc(void);
void n3ds_lift_set_ir_ip(uint32_t ip);

/* ---------------------------------------------------------------- state */

static uint32_t getr(CPU *c, unsigned idx, unsigned size) {
    if (size == IR_SIZE_8)
        return idx < 4 ? (c->r[idx] & 0xffu) : ((c->r[idx - 4] >> 8) & 0xffu);
    if (size == IR_SIZE_16) return c->r[idx] & 0xffffu;
    return c->r[idx];
}

static void setr(CPU *c, unsigned idx, unsigned size, uint32_t v) {
    if (size == IR_SIZE_8) {
        if (idx < 4) c->r[idx] = (c->r[idx] & ~0xffu) | (v & 0xffu);
        else c->r[idx - 4] = (c->r[idx - 4] & ~0xff00u) | ((v & 0xffu) << 8);
    } else if (size == IR_SIZE_16) {
        c->r[idx] = (c->r[idx] & 0xffff0000u) | (v & 0xffffu);
    } else {
        c->r[idx] = v;
    }
}

static uint32_t ld(CPU *c, unsigned size, uint32_t a) {
    (void)c;
    if (size == IR_SIZE_8) return M8(a);
    if (size == IR_SIZE_16) return M16(a);
    return M32(a);
}

static void st(CPU *c, unsigned size, uint32_t a, uint32_t v) {
    (void)c;
    if (size == IR_SIZE_8) M8(a) = (uint8_t)v;
    else if (size == IR_SIZE_16) M16(a) = (uint16_t)v;
    else M32(a) = v;
    IR_WATCH("ir-store");
}

static uint32_t ea_of(CPU *c, uint32_t w, uint32_t aw, int32_t disp) {
    uint32_t a = (uint32_t)disp;
    unsigned b = IR_A_BASE(aw), i = IR_A_INDEX(aw);
    if (b != 15u) a += c->r[b];
    if (i != 15u) a += c->r[i] << IR_A_SCALE(aw);
    if (IR_SEG(w) == IR_SEG_FS) a += c->fs_base;
    return a;
}

static uint32_t rd(CPU *c, uint32_t w, unsigned which, unsigned size, uint32_t memea) {
    if (which ? IR_SRCM(w) : IR_DSTM(w)) return ld(c, size, memea);
    return getr(c, which ? IR_SRC(w) : IR_DST(w), size);
}

/* The source operand is the immediate when one is encoded, not a register. */
static uint32_t rdsrc(CPU *c, uint32_t w, unsigned size, uint32_t memea, uint32_t imm) {
    if (IR_IMMK(w)) return imm;
    return rd(c, w, 1, size, memea);
}

static void wr(CPU *c, uint32_t w, unsigned which, unsigned size, uint32_t memea, uint32_t v) {
    if (which ? IR_SRCM(w) : IR_DSTM(w)) st(c, size, memea, v);
    else setr(c, which ? IR_SRC(w) : IR_DST(w), size, v);
}

static int cc_true(unsigned cc, const Flags *f) {
    switch (cc) {
    case 0: return (int)f->of;
    case 1: return !f->of;
    case 2: return (int)f->cf;
    case 3: return !f->cf;
    case 4: return (int)f->zf;
    case 5: return !f->zf;
    case 6: return (int)(f->cf || f->zf);
    case 7: return !f->cf && !f->zf;
    case 8: return (int)f->sf;
    case 9: return !f->sf;
    case 10: return (int)f->pf;
    case 11: return !f->pf;
    case 12: return f->sf != f->of;
    case 13: return f->sf == f->of;
    case 14: return f->zf || f->sf != f->of;
    default: return !f->zf && f->sf == f->of;
    }
}

/* Exact copy of the AOT rep-movsd helper (lift_prelude.h rt_movsd) so the IR
 * and AOT tiers agree on overlapping forward copies. */
static void rep_movsd(uint32_t n, uint32_t dst, uint32_t src) {
    if (!n) return;
    if (n > UINT32_MAX / 4) rt_unimpl(NULL, src, "rep movsd count");
    (void)n3ds_guest_ptr(dst, (size_t)n * 4);
    (void)n3ds_guest_ptr(src, (size_t)n * 4);
    if (dst <= src || (uint64_t)dst >= (uint64_t)src + (uint64_t)n * 4) {
        memmove(n3ds_guest_ptr(dst, (size_t)n * 4), n3ds_guest_ptr(src, (size_t)n * 4),
                (size_t)n * 4);
        IR_WATCH("ir-rep-movsd");
    } else {
        for (uint32_t k = 0; k < n; k++) M32(dst + 4 * k) = M32(src + 4 * k);
        IR_WATCH("ir-rep-movsd-overlap");
    }
}

static void trace_indirect_ir(CPU *c,uint32_t target,uint32_t from){
    static int trace_state;
    if(!trace_state){
        const char *v=getenv("N3DS_INDIRECT_TRACE");
        trace_state=(v&&v[0]&&strcmp(v,"0"))?2:1;
    }
    if(trace_state!=2)return;
    uint32_t ret=c?M32(c->r[4]):0;
    fprintf(stderr,"[indirect] target=%08lx block=%08lx ret=%08lx esp=%08lx eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx\n",
        (unsigned long)target,(unsigned long)from,(unsigned long)ret,
        c?(unsigned long)c->r[4]:0,(unsigned long)(c?c->r[0]:0),(unsigned long)(c?c->r[3]:0),
        (unsigned long)(c?c->r[1]:0),(unsigned long)(c?c->r[2]:0),
        (unsigned long)(c?c->r[6]:0),(unsigned long)(c?c->r[7]:0),(unsigned long)(c?c->r[5]:0));
    const uint8_t *p=n3ds_guest_ptr(from,48);
    if(p){
        fprintf(stderr,"[indirect] block bytes:");
        for(unsigned i=0;i<48;++i)fprintf(stderr," %02x",p[i]);
        fprintf(stderr,"\n");
        if(p[0]==0xff&&p[1]==0x25){
            uint32_t slot=(uint32_t)p[2]|((uint32_t)p[3]<<8)|((uint32_t)p[4]<<16)|((uint32_t)p[5]<<24);
            const uint8_t *q=n3ds_guest_ptr(slot,4);uint32_t val=0;
            if(q)val=(uint32_t)q[0]|((uint32_t)q[1]<<8)|((uint32_t)q[2]<<16)|((uint32_t)q[3]<<24);
            fprintf(stderr,"[indirect] thunk-slot=%08lx value=%08lx\n",(unsigned long)slot,(unsigned long)val);
        }
    }
}
static void dispatch(CPU *c, uint32_t target, int indirect, uint32_t from) {
    if (n3ds_rt_dispatch(c, target)) {
        trace_indirect_ir(c, target, from);
        rt_unimpl(c, target, indirect ? "unknown indirect function" : "unknown function/import");
    }
}

/* ---------------------------------------------------------------- string */

static void do_string(CPU *c, Flags *f, uint32_t w) {
    unsigned kind = IR_DST(w), size = IR_SIZE(w), rep = IR_AUX(w);
    unsigned bytes = size == IR_SIZE_8 ? 1u : size == IR_SIZE_16 ? 2u : 4u;
    unsigned n = bytes * 8;
    uint32_t step = c->df ? (uint32_t)(-(int32_t)bytes) : bytes;
    uint32_t esi = c->r[6], edi = c->r[7];

#define BODY() do {                                                                    \
        if (kind == 0) { /* movs */                                                    \
            st(c, size, edi, ld(c, size, esi)); esi += step; edi += step;              \
        } else if (kind == 1) { /* stos */                                             \
            st(c, size, edi, c->r[0]); edi += step;                                    \
        } else if (kind == 2) { /* lods */                                             \
            setr(c, 0, size, ld(c, size, esi)); esi += step;                           \
        } else if (kind == 3) { /* cmps */                                             \
            uint32_t a_ = ld(c, size, esi), b_ = ld(c, size, edi);                     \
            if (n == 8) (void)sub8(a_, b_, f);                                         \
            else if (n == 16) (void)sub16(a_, b_, f);                                  \
            else (void)sub32(a_, b_, f);                                               \
            esi += step; edi += step;                                                  \
        } else { /* scas */                                                            \
            uint32_t a_ = getr(c, 0, size), b_ = ld(c, size, edi);                     \
            if (n == 8) (void)sub8(a_, b_, f);                                         \
            else if (n == 16) (void)sub16(a_, b_, f);                                  \
            else (void)sub32(a_, b_, f);                                               \
            edi += step;                                                               \
        }                                                                              \
    } while (0)

    uint32_t ecx = c->r[1];
    if (!rep) {
        BODY();
    } else if ((kind == 0 || kind == 1) && rep == 1) {
        if (kind == 0 && size == IR_SIZE_32 && !c->df) {
            rep_movsd(ecx, edi, esi);
            edi += ecx * 4; esi += ecx * 4; ecx = 0;
        } else {
            while (ecx) { BODY(); ecx--; }
        }
    } else if (rep == 1) {          /* repe / repz / rep on lods/cmps/scas */
        while (ecx) { BODY(); ecx--; if (!f->zf) break; }
    } else {                        /* repne / repnz */
        while (ecx) { BODY(); ecx--; if (f->zf) break; }
    }
#undef BODY
    c->r[1] = ecx; c->r[6] = esi; c->r[7] = edi;
}

/* Optional host-only instruction trace for diagnosing a divergence. */
#ifdef IR_TRACE_BUILD
#include <stdlib.h>
static int ir_trace_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("IR_TRACE") != NULL;
    return v;
}
#define IR_TR(...) do { if (ir_trace_on()) printf(__VA_ARGS__); } while (0)
#else
#define IR_TR(...) ((void)0)
#endif

/* ---------------------------------------------------------------- x87 */

static void x87_exec(CPU *c, Flags *f, uint32_t desc, uint32_t ea, int has_mem,
                     uint32_t block_start) {
    unsigned op = (desc >> 8) & 0xffu;
    unsigned modrm = desc & 0xffu;
    unsigned mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    unsigned s = rm & 7;
    if (mod != 3) {
        switch (op) {
        case 0xd8: case 0xdc: case 0xda: case 0xde: {
            double src = op == 0xd8 ? (double)MF32(ea) :
                         op == 0xdc ? MF64(ea) :
                         op == 0xda ? (double)(int32_t)M32(ea) :
                                      (double)(int16_t)M16(ea);
            switch (reg) {
            case 0: ST(0) = ST(0) + src; break;
            case 1: ST(0) = ST(0) * src; break;
            case 4: ST(0) = ST(0) - src; break;
            case 5: ST(0) = src - ST(0); break;
            case 6: ST(0) = ST(0) / src; break;
            case 7: ST(0) = src / ST(0); break;
            case 2: fcompare(c, ST(0), src); break;
            default: fcompare(c, ST(0), src); fpop(c); break;
            }
            break;
        }
        case 0xd9:
            switch (reg) {
            case 0: fpush(c, (double)MF32(ea)); break;
            case 2: MF32(ea) = (float)ST(0); break;
            case 3: MF32(ea) = (float)ST(0); fpop(c); break;
            case 4: fpu_env(c, ea, 1); break;
            case 5: c->fpu_cw = M16(ea); break;
            case 6: fpu_env(c, ea, 0); break;
            default: M16(ea) = c->fpu_cw; break;
            }
            break;
        case 0xdb:
            switch (reg) {
            case 0: fpush(c, (double)(int32_t)M32(ea)); break;
            case 1: M32(ea) = (uint32_t)fistt32(ST(0)); fpop(c); break;
            case 2: M32(ea) = (uint32_t)fist32(c, ST(0)); break;
            case 3: M32(ea) = (uint32_t)fist32(c, ST(0)); fpop(c); break;
            case 5: fpush(c, f80_load(ea)); break;
            default: f80_store(ea, ST(0)); fpop(c); break;
            }
            break;
        case 0xdd:
            switch (reg) {
            case 0: fpush(c, MF64(ea)); break;
            case 1: M64(ea) = (uint64_t)fistt64(ST(0)); fpop(c); break;
            case 2: MF64(ea) = ST(0); break;
            case 3: MF64(ea) = ST(0); fpop(c); break;
            case 4: fpu_env(c, ea, 3); break;
            case 6: fpu_env(c, ea, 2); break;
            default: M16(ea) = fstatus(c); break;
            }
            break;
        default: /* 0xdf */
            switch (reg) {
            case 0: fpush(c, (double)(int16_t)M16(ea)); break;
            case 1: M16(ea) = (uint16_t)fistt16(ST(0)); fpop(c); break;
            case 2: M16(ea) = (uint16_t)fist16(c, ST(0)); break;
            case 3: M16(ea) = (uint16_t)fist16(c, ST(0)); fpop(c); break;
            case 5: fpush(c, (double)(int64_t)M64(ea)); break;
            default: M64(ea) = (uint64_t)fist64(c, ST(0)); fpop(c); break;
            }
            break;
        }
        (void)has_mem;
        return;
    }

    switch (op) {
    case 0xd8:
        switch (reg) {
        case 0: ST(0) = ST(0) + ST(s); break;
        case 1: ST(0) = ST(0) * ST(s); break;
        case 4: ST(0) = ST(0) - ST(s); break;
        case 5: ST(0) = ST(s) - ST(0); break;
        case 6: ST(0) = ST(0) / ST(s); break;
        case 7: ST(0) = ST(s) / ST(0); break;
        case 2: fcompare(c, ST(0), ST(s)); break;
        default: fcompare(c, ST(0), ST(s)); fpop(c); break;
        }
        break;
    case 0xd9:
        if (reg == 0) { double v = ST(s); fpush(c, v); }
        else if (reg == 1) { double v = ST(0); ST(0) = ST(s); ST(s) = v; }
        else if (modrm == 0xd0) { /* fnop */ }
        else {
            switch (modrm) {
            case 0xe0: ST(0) = -ST(0); break;
            case 0xe1: ST(0) = fabs(ST(0)); break;
            case 0xe4: fcompare(c, ST(0), 0.0); break;
            case 0xe5: fxam(c); break;
            case 0xe8: fpush(c, 1.0); break;
            case 0xe9: fpush(c, X87_L2T); break;
            case 0xea: fpush(c, X87_L2E); break;
            case 0xeb: fpush(c, X87_PI); break;
            case 0xec: fpush(c, X87_LG2); break;
            case 0xed: fpush(c, X87_LN2); break;
            case 0xee: fpush(c, 0.0); break;
            case 0xf0: ST(0) = exp2(ST(0)) - 1.0; break;
            case 0xf1: ST(1) = ST(1) * log2(ST(0)); fpop(c); break;
            case 0xf2: ST(0) = tan(ST(0)); fpush(c, 1.0); c->c2 = 0; break;
            case 0xf3: ST(1) = atan2(ST(1), ST(0)); fpop(c); break;
            case 0xf5: fprem(c, 1); break;
            case 0xf6: c->top = (c->top - 1) & 7; break;
            case 0xf7: c->top = (c->top + 1) & 7; break;
            case 0xf8: fprem(c, 0); break;
            case 0xf9: ST(1) = ST(1) * log2(ST(0) + 1.0); fpop(c); break;
            case 0xfa: ST(0) = sqrt(ST(0)); break;
            case 0xfb: { double v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); c->c2 = 0; } break;
            case 0xfc: ST(0) = fround(c, ST(0)); break;
            case 0xfd: ST(0) = ldexp(ST(0), (int)trunc(ST(1))); break;
            case 0xfe: ST(0) = sin(ST(0)); c->c2 = 0; break;
            case 0xff: ST(0) = cos(ST(0)); c->c2 = 0; break;
            default: rt_unimpl(c, block_start, "IR: unsupported x87 form"); break;
            }
        }
        break;
    case 0xda:
        if (modrm == 0xe9) { fcompare(c, ST(0), ST(1)); fpop(c); fpop(c); }
        else if (reg <= 3) {
            int cond = reg == 0 ? (int)f->cf : reg == 1 ? (int)f->zf :
                       reg == 2 ? (int)(f->cf || f->zf) : (int)f->pf;
            if (cond) ST(0) = ST(s);
        } else rt_unimpl(c, block_start, "IR: unsupported x87 form");
        break;
    case 0xdb:
        if (modrm == 0xe2) { c->c0 = c->c1 = c->c2 = c->c3 = 0; }
        else if (modrm == 0xe3) {
            c->top = 0; c->fpu_cw = 0x37F;
            c->c0 = c->c1 = c->c2 = c->c3 = 0;
        } else if (reg <= 3) {
            int cond = reg == 0 ? !f->cf : reg == 1 ? !f->zf :
                       reg == 2 ? (!f->cf && !f->zf) : !f->pf;
            if (cond) ST(0) = ST(s);
        } else if (reg == 5 || reg == 6) {
            double a = ST(0), b = ST(s);
            if (a != a || b != b) { f->zf = f->pf = f->cf = 1; }
            else { f->zf = a == b; f->pf = 0; f->cf = a < b; }
            f->of = f->sf = 0;
        } else rt_unimpl(c, block_start, "IR: unsupported x87 form");
        break;
    case 0xdc:
        switch (reg) {
        case 0: ST(s) = ST(s) + ST(0); break;
        case 1: ST(s) = ST(s) * ST(0); break;
        case 4: ST(s) = ST(0) - ST(s); break;
        case 5: ST(s) = ST(s) - ST(0); break;
        case 6: ST(s) = ST(0) / ST(s); break;
        case 7: ST(s) = ST(s) / ST(0); break;
        case 2: fcompare(c, ST(0), ST(s)); break;
        default: fcompare(c, ST(0), ST(s)); fpop(c); break;
        }
        break;
    case 0xdd:
        switch (reg) {
        case 0: break;                       /* ffree: tag only */
        case 2: ST(s) = ST(0); break;
        case 3: ST(s) = ST(0); fpop(c); break;
        case 4: fcompare(c, ST(0), ST(s)); break;
        case 5: fcompare(c, ST(0), ST(s)); fpop(c); break;
        default: rt_unimpl(c, block_start, "IR: unsupported x87 form"); break;
        }
        break;
    case 0xde:
        if (modrm == 0xd9) { fcompare(c, ST(0), ST(1)); fpop(c); fpop(c); }
        else {
            switch (reg) {
            case 0: ST(s) = ST(s) + ST(0); fpop(c); break;
            case 1: ST(s) = ST(s) * ST(0); fpop(c); break;
            case 4: ST(s) = ST(0) - ST(s); fpop(c); break;
            case 5: ST(s) = ST(s) - ST(0); fpop(c); break;
            case 6: ST(s) = ST(0) / ST(s); fpop(c); break;
            case 7: ST(s) = ST(s) / ST(0); fpop(c); break;
            default: rt_unimpl(c, block_start, "IR: unsupported x87 form"); break;
            }
        }
        break;
    default: /* 0xdf */
        if (modrm == 0xe0) setr(c, 0, IR_SIZE_16, fstatus(c));
        else if (reg == 5 || reg == 6) {
            double a = ST(0), b = ST(s);
            if (a != a || b != b) { f->zf = f->pf = f->cf = 1; }
            else { f->zf = a == b; f->pf = 0; f->cf = a < b; }
            f->of = f->sf = 0;
            fpop(c);
        } else if (reg == 0) fpop(c);
        else rt_unimpl(c, block_start, "IR: unsupported x87 form");
        break;
    }
}

/* ---------------------------------------------------------------- SSE */

static int sse_cmp_pred(unsigned pred, double a, double b) {
    switch (pred & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a != a || b != b;
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return a == a && b == b;
    }
}

static void sse_exec(CPU *c, Flags *f, uint32_t desc, uint32_t ea, int has_mem,
                     uint32_t block_start) {
    unsigned op2 = (desc >> 24) & 0xffu;
    unsigned pfx = (desc >> 22) & 3u;       /* 0 none, 1 66, 2 f2, 3 f3 */
    unsigned modrm = (desc >> 8) & 0xffu;
    unsigned imm8 = desc & 0xffu;
    unsigned mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    Xmm *d = &c->xmm[reg];
    Xmm *srcr = &c->xmm[rm];
    Xmm s;
    memset(&s, 0, sizeof s);
    if (!has_mem) s = *srcr;

#define LOAD16() do { s = has_mem ? n3ds_xmm_read(ea) : *srcr; } while (0)
#define LDM64()  do { s.q[0] = has_mem ? M64(ea) : srcr->q[0]; } while (0)

    switch (op2) {
    case 0x28: LOAD16(); *d = s; break;
    case 0x29:
        if (has_mem) { memcpy(n3ds_guest_ptr(ea, 16), d, 16); IR_WATCH("ir-sse-store"); } else *srcr = *d;
        break;
    case 0x10:
        if (pfx == 3) {
            if (has_mem) { d->u[0] = M32(ea); d->u[1] = d->u[2] = d->u[3] = 0; }
            else d->u[0] = srcr->u[0];
        } else if (pfx == 2) {
            if (has_mem) { d->q[0] = M64(ea); d->q[1] = 0; }
            else d->q[0] = srcr->q[0];
        } else { LOAD16(); *d = s; }
        break;
    case 0x11:
        if (pfx == 3) {
            if (has_mem) M32(ea) = d->u[0]; else srcr->u[0] = d->u[0];
        } else if (pfx == 2) {
            if (has_mem) M64(ea) = d->q[0]; else srcr->q[0] = d->q[0];
        } else if (has_mem) { memcpy(n3ds_guest_ptr(ea, 16), d, 16); IR_WATCH("ir-sse-store"); }
        else *srcr = *d;
        break;
    case 0x12:
        if (has_mem) d->q[0] = M64(ea); else d->q[0] = srcr->q[0];
        break;
    case 0x13:
        if (has_mem) M64(ea) = d->q[0]; else srcr->q[0] = d->q[0];
        break;
    case 0x14:
    case 0x15: {
        LOAD16();
        Xmm o = *d;
        if (pfx == 1) {
            if (op2 == 0x14) d->q[1] = s.q[0];
            else { d->q[0] = o.q[1]; d->q[1] = s.q[1]; }
        } else {
            unsigned h = op2 == 0x15 ? 2u : 0u;
            d->u[0] = o.u[h]; d->u[1] = s.u[h];
            d->u[2] = o.u[h + 1]; d->u[3] = s.u[h + 1];
        }
        break;
    }
    case 0x16:
        if (has_mem) d->q[1] = M64(ea); else d->q[1] = srcr->q[0]; /* movlhps */
        break;
    case 0x17:
        if (has_mem) M64(ea) = d->q[1]; else d->q[0] = srcr->q[1]; /* movhlps */
        break;
    case 0x50: {
        LOAD16();
        unsigned mask = 0;
        if (pfx == 1) {
            for (unsigned k = 0; k < 2; ++k) mask |= (unsigned)(s.q[k] >> 63) << k;
        } else {
            for (unsigned k = 0; k < 4; ++k) mask |= (s.u[k] >> 31) << k;
        }
        setr(c, reg, IR_SIZE_32, mask);
        break;
    }
    case 0x51: case 0x52: case 0x53: {
        int isd = pfx == 1 || pfx == 2;
        unsigned n = (pfx == 0) ? 4u : (pfx == 1) ? 2u : 1u;
        if (has_mem) {
            if (pfx == 3) { s.f[0] = MF32(ea); }
            else if (pfx == 2) { s.d[0] = MF64(ea); }
            else LOAD16();
        }
        for (unsigned k = 0; k < n; ++k) {
            if (isd) {
                double v = s.d[k];
                double r = op2 == 0x51 ? sqrt(v) :
                           op2 == 0x52 ? 1.0 / sqrt(v) : 1.0 / v;
                d->d[k] = r;
            } else {
                float v = s.f[k];
                float r = op2 == 0x51 ? sqrtf(v) :
                          op2 == 0x52 ? 1.0f / sqrtf(v) : 1.0f / v;
                d->f[k] = r;
            }
        }
        break;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: {
        LOAD16();
        for (unsigned k = 0; k < 2; ++k) {
            if (op2 == 0x54) d->q[k] &= s.q[k];
            else if (op2 == 0x55) d->q[k] = ~d->q[k] & s.q[k];
            else if (op2 == 0x56) d->q[k] |= s.q[k];
            else d->q[k] ^= s.q[k];
        }
        break;
    }
    case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        if (pfx == 3) {
            float b = has_mem ? MF32(ea) : srcr->f[0];
            float a = d->f[0];
            d->f[0] = op2 == 0x58 ? a + b : op2 == 0x59 ? a * b :
                      op2 == 0x5c ? a - b : op2 == 0x5d ? (a < b ? a : b) :
                      op2 == 0x5e ? a / b : (a > b ? a : b);
        } else if (pfx == 2) {
            double b = has_mem ? MF64(ea) : srcr->d[0];
            double a = d->d[0];
            d->d[0] = op2 == 0x58 ? a + b : op2 == 0x59 ? a * b :
                      op2 == 0x5c ? a - b : op2 == 0x5d ? (a < b ? a : b) :
                      op2 == 0x5e ? a / b : (a > b ? a : b);
        } else {
            LOAD16();
            unsigned n = pfx == 1 ? 2u : 4u;
            for (unsigned k = 0; k < n; ++k) {
                if (pfx == 1) {
                    double a = d->d[k], b = s.d[k];
                    d->d[k] = op2 == 0x58 ? a + b : op2 == 0x59 ? a * b :
                              op2 == 0x5c ? a - b : op2 == 0x5d ? (a < b ? a : b) :
                              op2 == 0x5e ? a / b : (a > b ? a : b);
                } else {
                    float a = d->f[k], b = s.f[k];
                    d->f[k] = op2 == 0x58 ? a + b : op2 == 0x59 ? a * b :
                              op2 == 0x5c ? a - b : op2 == 0x5d ? (a < b ? a : b) :
                              op2 == 0x5e ? a / b : (a > b ? a : b);
                }
            }
        }
        break;
    }
    case 0xc2: {
        int isd = pfx == 1 || pfx == 2;
        unsigned n = (pfx == 0) ? 4u : (pfx == 1) ? 2u : 1u;
        if (has_mem) {
            if (pfx == 3) s.f[0] = MF32(ea);
            else if (pfx == 2) s.d[0] = MF64(ea);
            else LOAD16();
        }
        for (unsigned k = 0; k < n; ++k) {
            if (isd) d->q[k] = sse_cmp_pred(imm8, d->d[k], s.d[k]) ? ~0ull : 0;
            else d->u[k] = sse_cmp_pred(imm8, d->f[k], s.f[k]) ? ~0u : 0;
        }
        break;
    }
    case 0xc6: {
        LOAD16();
        Xmm o = *d;
        if (pfx == 1) {
            d->q[0] = o.q[imm8 & 1];
            d->q[1] = s.q[(imm8 >> 1) & 1];
        } else {
            d->u[0] = o.u[imm8 & 3];
            d->u[1] = o.u[(imm8 >> 2) & 3];
            d->u[2] = s.u[(imm8 >> 4) & 3];
            d->u[3] = s.u[(imm8 >> 6) & 3];
        }
        break;
    }
    case 0x6f:
        if (has_mem) { Xmm t = n3ds_xmm_read(ea); *d = t; } else *d = *srcr;
        break;
    case 0x7f:
        if (has_mem) { memcpy(n3ds_guest_ptr(ea, 16), d, 16); IR_WATCH("ir-sse-store"); } else *srcr = *d;
        break;
    case 0x70: {
        LOAD16();
        Xmm o = s;
        for (unsigned k = 0; k < 4; ++k) d->u[k] = o.u[(imm8 >> (2 * k)) & 3];
        break;
    }
    case 0x74: case 0x75: case 0x76: {
        LOAD16();
        if (op2 == 0x74) for (unsigned k = 0; k < 16; ++k) d->b[k] = d->b[k] == s.b[k] ? 0xff : 0;
        else if (op2 == 0x75) for (unsigned k = 0; k < 8; ++k) d->w[k] = d->w[k] == s.w[k] ? 0xffff : 0;
        else for (unsigned k = 0; k < 4; ++k) d->u[k] = d->u[k] == s.u[k] ? ~0u : 0;
        break;
    }
    case 0x64: case 0x65: case 0x66: {
        LOAD16();
        if (op2 == 0x64) for (unsigned k = 0; k < 16; ++k) d->b[k] = (int8_t)d->b[k] > (int8_t)s.b[k] ? 0xff : 0;
        else if (op2 == 0x65) for (unsigned k = 0; k < 8; ++k) d->w[k] = (int16_t)d->w[k] > (int16_t)s.w[k] ? 0xffff : 0;
        else for (unsigned k = 0; k < 4; ++k) d->u[k] = d->i[k] > s.i[k] ? ~0u : 0;
        break;
    }
    case 0xdb: case 0xdf: case 0xeb: case 0xef: {
        LOAD16();
        for (unsigned k = 0; k < 2; ++k) {
            if (op2 == 0xdb) d->q[k] &= s.q[k];
            else if (op2 == 0xdf) d->q[k] = ~d->q[k] & s.q[k];
            else if (op2 == 0xeb) d->q[k] |= s.q[k];
            else d->q[k] ^= s.q[k];
        }
        break;
    }
    case 0xd4:
        LOAD16(); for (unsigned k = 0; k < 2; ++k) d->q[k] += s.q[k]; break;
    case 0xea:
        LOAD16(); for (unsigned k = 0; k < 8; ++k) if ((int16_t)s.w[k] < (int16_t)d->w[k]) d->w[k] = s.w[k]; break;
    case 0xee:
        LOAD16(); for (unsigned k = 0; k < 8; ++k) if ((int16_t)s.w[k] > (int16_t)d->w[k]) d->w[k] = s.w[k]; break;
    case 0xd5:
        LOAD16(); for (unsigned k = 0; k < 8; ++k) d->w[k] = (uint16_t)((int16_t)d->w[k] * (int16_t)s.w[k]); break;
    case 0xe5:
        LOAD16(); for (unsigned k = 0; k < 8; ++k) d->w[k] = (uint16_t)(((int32_t)(int16_t)d->w[k] * (int32_t)(int16_t)s.w[k]) >> 16); break;
    case 0xe0:
        LOAD16(); for (unsigned k = 0; k < 16; ++k) d->b[k] = (uint8_t)(((unsigned)d->b[k] + s.b[k] + 1) >> 1); break;
    case 0xf5:
        LOAD16();
        for (unsigned k = 0; k < 4; ++k) {
            int32_t a0 = (int16_t)d->w[2 * k], a1 = (int16_t)d->w[2 * k + 1];
            int32_t b0 = (int16_t)s.w[2 * k], b1 = (int16_t)s.w[2 * k + 1];
            d->i[k] = a0 * b0 + a1 * b1;
        }
        break;
    case 0xf8:
        LOAD16(); for (unsigned k = 0; k < 16; ++k) d->b[k] -= s.b[k]; break;
    case 0xf9:
        LOAD16(); for (unsigned k = 0; k < 8; ++k) d->w[k] -= s.w[k]; break;
    case 0xfa:
        LOAD16(); for (unsigned k = 0; k < 4; ++k) d->u[k] -= s.u[k]; break;
    case 0xfb:
        LOAD16(); for (unsigned k = 0; k < 2; ++k) d->q[k] -= s.q[k]; break;
    case 0xfc:
        LOAD16(); for (unsigned k = 0; k < 16; ++k) d->b[k] += s.b[k]; break;
    case 0xfd:
        LOAD16(); for (unsigned k = 0; k < 8; ++k) d->w[k] += s.w[k]; break;
    case 0xfe:
        LOAD16(); for (unsigned k = 0; k < 4; ++k) d->u[k] += s.u[k]; break;
    case 0xe6:
        LOAD16();
        d->d[0] = (double)s.i[0];
        d->d[1] = (double)s.i[1];
        break;
    case 0x5b:
        LOAD16();
        if (pfx == 1) for (unsigned k = 0; k < 4; ++k) d->f[k] = (float)s.i[k];
        else for (unsigned k = 0; k < 4; ++k) d->i[k] = cvt_f2i(truncf(s.f[k]));
        break;
    case 0xd7: {
        LOAD16();
        unsigned mask = 0;
        for (unsigned k = 0; k < 16; ++k) mask |= (unsigned)(s.b[k] >> 7) << k;
        setr(c, reg, IR_SIZE_32, mask);
        break;
    }
    case 0xc5:
        setr(c, reg, IR_SIZE_32, srcr->w[imm8 & 7]);
        break;
    case 0xc4:
        d->w[imm8 & 7] = (uint16_t)(has_mem ? M16(ea) : getr(c, rm, IR_SIZE_16));
        break;
    case 0x2a:
        if (pfx == 3) d->f[0] = (float)(int32_t)(has_mem ? M32(ea) : getr(c, rm, IR_SIZE_32));
        else d->d[0] = (double)(int32_t)(has_mem ? M32(ea) : getr(c, rm, IR_SIZE_32));
        break;
    case 0x2c: case 0x2d:
        if (pfx == 3) {
            float v = has_mem ? MF32(ea) : srcr->f[0];
            setr(c, reg, IR_SIZE_32, cvt_f2i(op2 == 0x2c ? truncf(v) : nearbyintf(v)));
        } else {
            double v = has_mem ? MF64(ea) : srcr->d[0];
            setr(c, reg, IR_SIZE_32, cvt_d2i(op2 == 0x2c ? trunc(v) : nearbyint(v)));
        }
        break;
    case 0x2e: case 0x2f:
        if (pfx == 1) {
            double a = d->d[0], b = has_mem ? MF64(ea) : srcr->d[0];
            if (a != a || b != b) { f->zf = f->pf = f->cf = 1; }
            else { f->zf = a == b; f->pf = 0; f->cf = a < b; }
        } else {
            float a = d->f[0], b = has_mem ? MF32(ea) : srcr->f[0];
            if (a != a || b != b) { f->zf = f->pf = f->cf = 1; }
            else { f->zf = a == b; f->pf = 0; f->cf = a < b; }
        }
        f->of = f->sf = 0;
        break;
    case 0x5a:
        if (pfx == 3) d->d[0] = (double)(has_mem ? MF32(ea) : srcr->f[0]);
        else if (pfx == 2) d->f[0] = (float)(has_mem ? MF64(ea) : srcr->d[0]);
        else {
            if (has_mem) { float f0 = MF32(ea), f1 = MF32(ea + 4); d->d[0] = f0; d->d[1] = f1; }
            else { d->d[0] = srcr->f[0]; d->d[1] = srcr->f[1]; }
        }
        break;
    case 0x6e:
        if (pfx == 1) {
            d->u[0] = has_mem ? M32(ea) : getr(c, rm, IR_SIZE_32);
            d->u[1] = d->u[2] = d->u[3] = 0;
        } else {
            d->q[0] = has_mem ? M64(ea) : srcr->q[0];
            d->q[1] = 0;
        }
        break;
    case 0x7e:
        if (pfx == 1) {
            if (has_mem) M32(ea) = d->u[0]; else setr(c, rm, IR_SIZE_32, d->u[0]);
        } else {
            if (has_mem) M64(ea) = d->q[0];
            else { srcr->q[0] = d->q[0]; srcr->q[1] = 0; }
        }
        break;
    case 0xd6:
        if (has_mem) M64(ea) = d->q[0]; else srcr->q[0] = d->q[0];
        break;
    case 0x71: case 0x72: case 0x73: {
        Xmm *t = srcr;
        unsigned n = imm8;
        if (op2 == 0x71) {
            if (reg == 2) for (unsigned k = 0; k < 8; ++k) t->w[k] = n >= 16 ? 0 : (uint16_t)(t->w[k] >> n);
            else if (reg == 4) for (unsigned k = 0; k < 8; ++k) t->w[k] = (uint16_t)((int16_t)t->w[k] >> (n >= 16 ? 15 : n));
            else if (reg == 6) for (unsigned k = 0; k < 8; ++k) t->w[k] = n >= 16 ? 0 : (uint16_t)(t->w[k] << n);
            else rt_unimpl(c, block_start, "IR: unsupported SSE form");
        } else if (op2 == 0x72) {
            if (reg == 2) for (unsigned k = 0; k < 4; ++k) t->u[k] = n >= 32 ? 0 : t->u[k] >> n;
            else if (reg == 4) for (unsigned k = 0; k < 4; ++k) t->i[k] = (int32_t)t->i[k] >> (n >= 32 ? 31 : n);
            else if (reg == 6) for (unsigned k = 0; k < 4; ++k) t->u[k] = n >= 32 ? 0 : t->u[k] << n;
            else rt_unimpl(c, block_start, "IR: unsupported SSE form");
        } else {
            if (reg == 2) for (unsigned k = 0; k < 2; ++k) t->q[k] = n >= 64 ? 0 : t->q[k] >> n;
            else if (reg == 6) for (unsigned k = 0; k < 2; ++k) t->q[k] = n >= 64 ? 0 : t->q[k] << n;
            else if (reg == 3 || reg == 7) {
                uint8_t tmp[16]; memset(tmp, 0, 16);
                if (n < 16) {
                    if (reg == 3) memcpy(tmp, t->b + n, 16 - n);
                    else memcpy(tmp + n, t->b, 16 - n);
                }
                memcpy(t->b, tmp, 16);
            } else rt_unimpl(c, block_start, "IR: unsupported SSE form");
        }
        break;
    }
    case 0xd1: case 0xd2: case 0xd3:
    case 0xe1: case 0xe2:
    case 0xf1: case 0xf2: case 0xf3: {
        LOAD16();
        unsigned n = (unsigned)(s.q[0] > 64 ? 64 : s.q[0]);
        unsigned cls = op2 & 0xf0u;
        if (op2 == 0xd1 || op2 == 0xe1 || op2 == 0xf1) {
            for (unsigned k = 0; k < 8; ++k)
                d->w[k] = cls == 0xf0 ? (n >= 16 ? 0 : (uint16_t)(d->w[k] << n)) :
                          cls == 0xe0 ? (uint16_t)((int16_t)d->w[k] >> (n >= 16 ? 15 : n)) :
                          (n >= 16 ? 0 : (uint16_t)(d->w[k] >> n));
        } else if (op2 == 0xd2 || op2 == 0xe2 || op2 == 0xf2) {
            for (unsigned k = 0; k < 4; ++k)
                d->u[k] = cls == 0xf0 ? (n >= 32 ? 0 : d->u[k] << n) :
                          cls == 0xe0 ? (uint32_t)((int32_t)d->i[k] >> (n >= 32 ? 31 : n)) :
                          (n >= 32 ? 0 : d->u[k] >> n);
        } else {
            for (unsigned k = 0; k < 2; ++k)
                d->q[k] = cls == 0xf0 ? (n >= 64 ? 0 : d->q[k] << n) :
                          (n >= 64 ? 0 : d->q[k] >> n);
        }
        break;
    }
    case 0xae:
        if (mod == 3 && reg >= 5) { /* lfence / mfence / sfence: no-op */ }
        else if (reg == 2 && has_mem) c->mxcsr = M32(ea);
        else if (reg == 3 && has_mem) M32(ea) = c->mxcsr;
        else rt_unimpl(c, block_start, "IR: unsupported SSE form");
        break;
    case 0xe7:
        if (has_mem) { memcpy(n3ds_guest_ptr(ea, 16), d, 16); IR_WATCH("ir-sse-store"); } else *srcr = *d;
        break;
    default:
        rt_unimpl(c, block_start, "IR: unsupported SSE form");
    }
#undef LOAD16
#undef LDM64
}

/* ---------------------------------------------------------------- block */

/* Returns 1 when the guest function returned, 0 when *nextp is the next block
 * inside the same function. */
static int exec_block(CPU *c, const IrExecBlock *b, Flags *f,
                      uint32_t frame_start, uint32_t func_end, uint32_t *nextp) {
    const uint32_t *p = b->stream;
    const uint32_t *pend = p + b->stream_words;
    uint32_t block_end = b->start + b->x86_len;

    while (p < pend) {
        uint32_t w = *p++;
        uint32_t aw = 0;
        int32_t disp = 0;
        uint32_t imm = 0;
        if (IR_HAS_ADDR(w)) {
            aw = *p++;
            unsigned dk = IR_A_DISPK(aw);
            if (dk == 2) disp = (int32_t)*p++;
            else if (dk == 1) disp = IR_A_DISP8(aw);
        }
        switch (IR_IMMK(w)) {
        case IR_IMM_S8: imm = (uint32_t)IR_IMM8(w); break;
        case IR_IMM_U8: imm = IR_IMM8U(w); break;
        case IR_IMM_32: imm = *p++; break;
        default: break;
        }
        uint32_t memea = IR_HAS_ADDR(w) ? ea_of(c, w, aw, disp) : 0;
        unsigned size = IR_SIZE(w);
        unsigned op = IR_OP(w);
        IR_TR("ir %08x op=%u dst=%u%c src=%u%c sz=%u ea=%08x imm=%08x\n",
              b->start, op, IR_DST(w), IR_DSTM(w) ? 'm' : 'r', IR_SRC(w),
              IR_SRCM(w) ? 'm' : 'r', size, memea, imm);

        if (op >= IR_OP_JCC_BASE && op < IR_OP_JCC_BASE + 16) {
            uint32_t tgt = block_end + (int32_t)imm;
            uint32_t nxt = cc_true(op - IR_OP_JCC_BASE, f) ? tgt : block_end;
            if (nxt >= frame_start && nxt < func_end) { *nextp = nxt; return 0; }
            dispatch(c, nxt, 0, b->start);
            return 1;
        }
        if (op >= IR_OP_SETCC_BASE && op < IR_OP_SETCC_BASE + 16) {
            wr(c, w, 0, IR_SIZE_8, memea,
               cc_true(op - IR_OP_SETCC_BASE, f) ? 1u : 0u);
            continue;
        }
        if (op >= IR_OP_CMOVCC_BASE && op < IR_OP_CMOVCC_BASE + 16) {
            if (cc_true(op - IR_OP_CMOVCC_BASE, f))
                wr(c, w, 0, size, memea, rdsrc(c, w, size, memea, imm));
            continue;
        }

        switch (op) {
        case IR_OP_NOP:
            break;
        case IR_OP_MOV:
            wr(c, w, 0, size, memea, rdsrc(c, w, size, memea, imm));
            break;
        case IR_OP_MOVZX8:
            wr(c, w, 0, size, memea, rd(c, w, 1, IR_SIZE_8, memea));
            break;
        case IR_OP_MOVZX16:
            wr(c, w, 0, size, memea, rd(c, w, 1, IR_SIZE_16, memea));
            break;
        case IR_OP_MOVSX8: {
            uint32_t v = rd(c, w, 1, IR_SIZE_8, memea);
            wr(c, w, 0, size, memea, (uint32_t)(int32_t)(int8_t)v);
            break;
        }
        case IR_OP_MOVSX16: {
            uint32_t v = rd(c, w, 1, IR_SIZE_16, memea);
            wr(c, w, 0, size, memea, (uint32_t)(int32_t)(int16_t)v);
            break;
        }
        case IR_OP_LEA:
            setr(c, IR_DST(w), size, memea);
            break;

#define BIN(fn) do {                                                                 \
            uint32_t a_ = rd(c, w, 0, size, memea);                                   \
            uint32_t b_ = rdsrc(c, w, size, memea, imm);                                   \
            uint32_t r_;                                                              \
            if (size == IR_SIZE_8) r_ = fn##8(a_, b_, f);                             \
            else if (size == IR_SIZE_16) r_ = fn##16(a_, b_, f);                      \
            else r_ = fn##32(a_, b_, f);                                              \
            wr(c, w, 0, size, memea, r_);                                             \
        } while (0)
#define BIN_NW(fn) do {                                                              \
            uint32_t a_ = rd(c, w, 0, size, memea);                                   \
            uint32_t b_ = rdsrc(c, w, size, memea, imm);                                   \
            if (size == IR_SIZE_8) (void)fn##8(a_, b_, f);                            \
            else if (size == IR_SIZE_16) (void)fn##16(a_, b_, f);                     \
            else (void)fn##32(a_, b_, f);                                             \
        } while (0)
        case IR_OP_ADD: BIN(add); break;
        case IR_OP_ADC: BIN(adc); break;
        case IR_OP_SBB: BIN(sbb); break;
        case IR_OP_SUB: BIN(sub); break;
        case IR_OP_OR:
        case IR_OP_AND:
        case IR_OP_XOR: {
            uint32_t a_ = rd(c, w, 0, size, memea);
            uint32_t b_ = rdsrc(c, w, size, memea, imm);
            uint32_t v_ = op == IR_OP_OR ? (a_ | b_) : op == IR_OP_AND ? (a_ & b_) : (a_ ^ b_);
            uint32_t r_;
            if (size == IR_SIZE_8) r_ = logic8((uint8_t)v_, f);
            else if (size == IR_SIZE_16) r_ = logic16((uint16_t)v_, f);
            else r_ = logic32(v_, f);
            wr(c, w, 0, size, memea, r_);
            break;
        }
        case IR_OP_CMP: BIN_NW(sub); break;
        case IR_OP_TEST: {
            uint32_t a_ = rd(c, w, 0, size, memea);
            uint32_t b_ = rdsrc(c, w, size, memea, imm);
            if (size == IR_SIZE_8) (void)logic8((uint8_t)(a_ & b_), f);
            else if (size == IR_SIZE_16) (void)logic16((uint16_t)(a_ & b_), f);
            else (void)logic32(a_ & b_, f);
            break;
        }
#undef BIN
#undef BIN_NW
        case IR_OP_INC: {
            uint32_t a_ = rd(c, w, 0, size, memea); uint32_t r_;
            if (size == IR_SIZE_8) r_ = inc8(a_, f);
            else if (size == IR_SIZE_16) r_ = inc16(a_, f);
            else r_ = inc32(a_, f);
            wr(c, w, 0, size, memea, r_);
            break;
        }
        case IR_OP_DEC: {
            uint32_t a_ = rd(c, w, 0, size, memea); uint32_t r_;
            if (size == IR_SIZE_8) r_ = dec8(a_, f);
            else if (size == IR_SIZE_16) r_ = dec16(a_, f);
            else r_ = dec32(a_, f);
            wr(c, w, 0, size, memea, r_);
            break;
        }
        case IR_OP_NEG: {
            uint32_t a_ = rd(c, w, 0, size, memea); uint32_t r_;
            if (size == IR_SIZE_8) r_ = neg8(a_, f);
            else if (size == IR_SIZE_16) r_ = neg16(a_, f);
            else r_ = neg32(a_, f);
            wr(c, w, 0, size, memea, r_);
            break;
        }
        case IR_OP_NOT: {
            uint32_t a_ = rd(c, w, 0, size, memea);
            wr(c, w, 0, size, memea, ~a_);
            break;
        }

#define SHIFT(fn) do {                                                               \
            uint32_t a_ = rd(c, w, 0, size, memea);                                   \
            uint32_t n_ = IR_IMMK(w) ? imm :                                         \
                          (IR_AUX(w) == IR_AUX_SHIFT_CL ? (c->r[1] & 0xffu) : 1u);    \
            uint32_t r_;                                                              \
            if (size == IR_SIZE_8) r_ = fn##8(a_, n_, f);                             \
            else if (size == IR_SIZE_16) r_ = fn##16(a_, n_, f);                      \
            else r_ = fn##32(a_, n_, f);                                              \
            wr(c, w, 0, size, memea, r_);                                             \
        } while (0)
        case IR_OP_SHL: SHIFT(shl); break;
        case IR_OP_SHR: SHIFT(shr); break;
        case IR_OP_SAR: SHIFT(sar); break;
        case IR_OP_ROL: SHIFT(rol); break;
        case IR_OP_ROR: SHIFT(ror); break;
        case IR_OP_RCL: SHIFT(rcl); break;
        case IR_OP_RCR: SHIFT(rcr); break;
#undef SHIFT
        case IR_OP_SHLD: {
            uint32_t a_ = rd(c, w, 0, size, memea);
            uint32_t b_ = rd(c, w, 1, size, memea);
            uint32_t n_ = IR_IMMK(w) ? imm : (c->r[1] & 0xffu);
            wr(c, w, 0, size, memea, shld32(a_, b_, n_, f));
            break;
        }
        case IR_OP_SHRD: {
            uint32_t a_ = rd(c, w, 0, size, memea);
            uint32_t b_ = rd(c, w, 1, size, memea);
            uint32_t n_ = IR_IMMK(w) ? imm : (c->r[1] & 0xffu);
            wr(c, w, 0, size, memea, shrd32(a_, b_, n_, f));
            break;
        }
        case IR_OP_IMUL1: {
            uint32_t s_ = rd(c, w, 1, size, memea);
            if (size == IR_SIZE_32) {
                int64_t r_ = (int64_t)(int32_t)c->r[0] * (int32_t)s_;
                c->r[0] = (uint32_t)r_; c->r[2] = (uint32_t)(r_ >> 32);
                f->cf = f->of = r_ != (int32_t)r_;
            } else if (size == IR_SIZE_16) {
                int32_t r_ = (int16_t)c->r[0] * (int16_t)s_;
                setr(c, 0, IR_SIZE_16, (uint32_t)r_);
                setr(c, 2, IR_SIZE_16, (uint32_t)(r_ >> 16));
                f->cf = f->of = r_ != (int16_t)r_;
            } else {
                int32_t r_ = (int8_t)c->r[0] * (int8_t)s_;
                setr(c, 0, IR_SIZE_16, (uint32_t)r_);
                f->cf = f->of = r_ != (int8_t)r_;
            }
            break;
        }
        case IR_OP_IMUL2:
        case IR_OP_IMUL3: {
            uint32_t a_ = rd(c, w, 0, size, memea);
            uint32_t b_ = op == IR_OP_IMUL3 ? imm : rd(c, w, 1, size, memea);
            int64_t r_;
            if (size == IR_SIZE_32) r_ = (int64_t)(int32_t)a_ * (int32_t)b_;
            else if (size == IR_SIZE_16) r_ = (int64_t)(int16_t)a_ * (int16_t)b_;
            else r_ = (int64_t)(int8_t)a_ * (int8_t)b_;
            wr(c, w, 0, size, memea, (uint32_t)r_);
            f->cf = f->of = size == IR_SIZE_8 ? r_ != (int8_t)r_ :
                            size == IR_SIZE_16 ? r_ != (int16_t)r_ : r_ != (int32_t)r_;
            break;
        }
        case IR_OP_MUL1: {
            uint32_t s_ = rd(c, w, 1, size, memea);
            if (size == IR_SIZE_32) {
                uint64_t r_ = (uint64_t)c->r[0] * s_;
                c->r[0] = (uint32_t)r_; c->r[2] = (uint32_t)(r_ >> 32);
                f->cf = f->of = c->r[2] != 0;
            } else if (size == IR_SIZE_16) {
                uint32_t r_ = (uint16_t)c->r[0] * (uint32_t)s_;
                setr(c, 0, IR_SIZE_16, r_);
                setr(c, 2, IR_SIZE_16, r_ >> 16);
                f->cf = f->of = (r_ >> 16) != 0;
            } else {
                uint32_t r_ = (uint8_t)c->r[0] * (uint32_t)s_;
                setr(c, 0, IR_SIZE_16, r_);
                f->cf = f->of = (r_ >> 8) != 0;
            }
            break;
        }
        case IR_OP_DIV:
        case IR_OP_IDIV: {
            uint32_t s_ = rd(c, w, 1, size, memea);
            if (!s_) rt_unimpl(c, b->start, "IR: divide by zero");
            if (size == IR_SIZE_32) {
                if (op == IR_OP_DIV) {
                    uint64_t n_ = ((uint64_t)c->r[2] << 32) | c->r[0];
                    uint32_t d_ = s_;
                    c->r[0] = (uint32_t)(n_ / d_);
                    c->r[2] = (uint32_t)(n_ % d_);
                } else {
                    int64_t n_ = (int64_t)(((uint64_t)c->r[2] << 32) | c->r[0]);
                    int32_t d_ = (int32_t)s_;
                    c->r[0] = (uint32_t)(int32_t)(n_ / d_);
                    c->r[2] = (uint32_t)(int32_t)(n_ % d_);
                }
            } else if (size == IR_SIZE_16) {
                if (op == IR_OP_DIV) {
                    uint32_t n_ = (((uint32_t)(uint16_t)c->r[2]) << 16) | (uint16_t)c->r[0];
                    uint32_t d_ = (uint16_t)s_;
                    setr(c, 0, IR_SIZE_16, n_ / d_);
                    setr(c, 2, IR_SIZE_16, n_ % d_);
                } else {
                    int32_t n_ = (int32_t)((((uint32_t)(uint16_t)c->r[2]) << 16) |
                                           (uint16_t)c->r[0]);
                    int32_t d_ = (int16_t)s_;
                    setr(c, 0, IR_SIZE_16, (uint32_t)(int16_t)(n_ / d_));
                    setr(c, 2, IR_SIZE_16, (uint32_t)(int16_t)(n_ % d_));
                }
            } else {
                if (op == IR_OP_DIV) {
                    uint32_t n_ = (uint16_t)c->r[0];
                    uint32_t d_ = (uint8_t)s_;
                    c->r[0] = (c->r[0] & 0xffffff00u) | ((n_ / d_) & 0xffu);
                    c->r[0] = (c->r[0] & 0xffff00ffu) | (((n_ % d_) & 0xffu) << 8);
                } else {
                    int32_t n_ = (int32_t)(int16_t)(uint16_t)c->r[0];
                    int32_t d_ = (int8_t)s_;
                    c->r[0] = (c->r[0] & 0xffffff00u) | ((uint32_t)(n_ / d_) & 0xffu);
                    c->r[0] = (c->r[0] & 0xffff00ffu) | (((uint32_t)(n_ % d_) & 0xffu) << 8);
                }
            }
            break;
        }
        case IR_OP_CDQ:
            if (size == IR_SIZE_16) setr(c, 2, IR_SIZE_16, (uint32_t)((int16_t)c->r[0] >> 15));
            else c->r[2] = (uint32_t)((int32_t)c->r[0] >> 31);
            break;
        case IR_OP_CWDE:
            if (size == IR_SIZE_16) setr(c, 0, IR_SIZE_16, (uint32_t)(int16_t)(int8_t)c->r[0]);
            else c->r[0] = (uint32_t)(int32_t)(int16_t)c->r[0];
            break;
        case IR_OP_CBW:
            setr(c, 0, IR_SIZE_16, (uint32_t)(int16_t)(int8_t)c->r[0]);
            break;
        case IR_OP_CWD:
            setr(c, 2, IR_SIZE_16, (uint32_t)(uint16_t)((int16_t)c->r[0] >> 15));
            break;
        case IR_OP_BSWAP:
            wr(c, w, 0, IR_SIZE_32, memea, __builtin_bswap32(rd(c, w, 0, IR_SIZE_32, memea)));
            break;
        case IR_OP_BSF:
        case IR_OP_BSR: {
            uint32_t v_ = rd(c, w, 1, size, memea);
            f->zf = v_ == 0;
            if (v_) {
                uint32_t r_ = op == IR_OP_BSR ? (31u - (unsigned)__builtin_clz(v_))
                                              : (unsigned)__builtin_ctz(v_);
                wr(c, w, 0, size, memea, r_);
            }
            break;
        }
        case IR_OP_BT:
        case IR_OP_BTS:
        case IR_OP_BTR:
        case IR_OP_BTC: {
            uint32_t bit = IR_IMMK(w) ? (imm & 31u)
                          : (rd(c, w, 1, IR_SIZE_32, memea) & 31u);
            if (IR_DSTM(w)) {
                uint32_t waddr = memea;
                if (!IR_IMMK(w)) {
                    uint32_t r = rd(c, w, 1, IR_SIZE_32, memea);
                    waddr = memea + (uint32_t)(((int32_t)r >> 5) * 4);
                    bit = r & 31u;
                }
                uint32_t old = M32(waddr);
                f->cf = (old >> bit) & 1u;
                if (op == IR_OP_BTS) M32(waddr) = old | (1u << bit);
                else if (op == IR_OP_BTR) M32(waddr) = old & ~(1u << bit);
                else if (op == IR_OP_BTC) M32(waddr) = old ^ (1u << bit);
            } else {
                uint32_t old = rd(c, w, 0, IR_SIZE_32, memea);
                f->cf = (old >> bit) & 1u;
                if (op == IR_OP_BTS) wr(c, w, 0, IR_SIZE_32, memea, old | (1u << bit));
                else if (op == IR_OP_BTR) wr(c, w, 0, IR_SIZE_32, memea, old & ~(1u << bit));
                else if (op == IR_OP_BTC) wr(c, w, 0, IR_SIZE_32, memea, old ^ (1u << bit));
            }
            break;
        }
        case IR_OP_XCHG: {
            uint32_t a_ = rd(c, w, 0, size, memea);
            uint32_t b_ = rdsrc(c, w, size, memea, imm);
            wr(c, w, 0, size, memea, b_);
            wr(c, w, 1, size, memea, a_);
            break;
        }
        case IR_OP_XADD: {
            uint32_t s_ = rd(c, w, 1, size, memea);
            if (IR_DSTM(w)) {
                uint32_t o_;
                if (size == IR_SIZE_8) o_ = N3DS_FETCH_ADD(N3DS_PTR(uint8_t, memea), (uint8_t)s_, 0);
                else if (size == IR_SIZE_16) o_ = N3DS_FETCH_ADD(N3DS_PTR(uint16_t, memea), (uint16_t)s_, 0);
                else o_ = N3DS_FETCH_ADD(N3DS_PTR(uint32_t, memea), s_, 0);
                uint32_t r_;
                if (size == IR_SIZE_8) r_ = add8(o_, s_, f);
                else if (size == IR_SIZE_16) r_ = add16(o_, s_, f);
                else r_ = add32(o_, s_, f);
                (void)r_;
                wr(c, w, 1, size, memea, o_);
                IR_WATCH("ir-atomic-xadd");
            } else {
                uint32_t d_ = rd(c, w, 0, size, memea);
                uint32_t r_;
                if (size == IR_SIZE_8) r_ = add8(d_, s_, f);
                else if (size == IR_SIZE_16) r_ = add16(d_, s_, f);
                else r_ = add32(d_, s_, f);
                wr(c, w, 1, size, memea, d_);
                wr(c, w, 0, size, memea, r_);
            }
            break;
        }
        case IR_OP_CMPXCHG: {
            uint32_t src_ = rd(c, w, 1, size, memea);
            if (IR_DSTM(w)) {
                uint32_t exp_ = getr(c, 0, size);
                uint32_t old_;
                if (size == IR_SIZE_8) old_ = N3DS_CAS(N3DS_PTR(uint8_t, memea), &exp_, (uint8_t)src_, 0, 0, 0);
                else if (size == IR_SIZE_16) old_ = N3DS_CAS(N3DS_PTR(uint16_t, memea), &exp_, (uint16_t)src_, 0, 0, 0);
                else old_ = N3DS_CAS(N3DS_PTR(uint32_t, memea), &exp_, src_, 0, 0, 0);
                (void)old_;
                uint32_t a_ = getr(c, 0, size);
                if (size == IR_SIZE_8) (void)sub8(a_, exp_, f);
                else if (size == IR_SIZE_16) (void)sub16(a_, exp_, f);
                else (void)sub32(a_, exp_, f);
                if (!f->zf) setr(c, 0, size, exp_);
                IR_WATCH("ir-atomic-cmpxchg");
            } else {
                uint32_t d_ = rd(c, w, 0, size, memea);
                uint32_t a_ = getr(c, 0, size);
                if (size == IR_SIZE_8) (void)sub8(a_, d_, f);
                else if (size == IR_SIZE_16) (void)sub16(a_, d_, f);
                else (void)sub32(a_, d_, f);
                if (f->zf) wr(c, w, 0, size, memea, src_);
                else setr(c, 0, size, d_);
            }
            break;
        }
        case IR_OP_CMPXCHG8B: {
            uint64_t exp_ = ((uint64_t)c->r[2] << 32) | c->r[0];
            uint64_t des_ = ((uint64_t)c->r[1] << 32) | c->r[3];
            if (N3DS_CAS(N3DS_PTR(uint64_t, memea), &exp_, des_, 0, 0, 0)) {
                f->zf = 1;
            } else {
                f->zf = 0;
                c->r[0] = (uint32_t)exp_;
                c->r[2] = (uint32_t)(exp_ >> 32);
            }
            IR_WATCH("ir-atomic-cmpxchg8b");
            break;
        }
        case IR_OP_PUSH: {
            if (IR_IMMK(w)) {
                uint32_t v_ = imm;
                c->r[4] -= 4; M32(c->r[4]) = v_;
            } else {
                uint32_t v_ = rd(c, w, 1, size, memea);
                if (size == IR_SIZE_16) { c->r[4] -= 2; M16(c->r[4]) = (uint16_t)v_; }
                else { c->r[4] -= 4; M32(c->r[4]) = v_; }
            }
            break;
        }
        case IR_OP_POP: {
            uint32_t v_ = M32(c->r[4]);
            c->r[4] += 4;
            if (IR_DSTM(w)) {
                uint32_t a2 = ea_of(c, w, aw, disp);
                st(c, size, a2, v_);
            } else if (IR_DST(w) == 4) {
                c->r[4] = v_;
            } else {
                setr(c, IR_DST(w), size, v_);
            }
            break;
        }
        case IR_OP_PUSHA: {
            uint32_t s_ = c->r[4];
            c->r[4] -= 32;
            M32(c->r[4] + 28) = c->r[0]; M32(c->r[4] + 24) = c->r[1];
            M32(c->r[4] + 20) = c->r[2]; M32(c->r[4] + 16) = c->r[3];
            M32(c->r[4] + 12) = s_;      M32(c->r[4] + 8) = c->r[5];
            M32(c->r[4] + 4) = c->r[6];  M32(c->r[4]) = c->r[7];
            break;
        }
        case IR_OP_POPA:
            c->r[7] = M32(c->r[4]);      c->r[6] = M32(c->r[4] + 4);
            c->r[5] = M32(c->r[4] + 8);  c->r[3] = M32(c->r[4] + 16);
            c->r[2] = M32(c->r[4] + 20); c->r[1] = M32(c->r[4] + 24);
            c->r[0] = M32(c->r[4] + 28); c->r[4] += 32;
            break;
        case IR_OP_PUSHFD:
            c->r[4] -= 4;
            M32(c->r[4]) = f->cf | 2u | (f->pf << 2) | (f->zf << 6) | (f->sf << 7) |
                           (c->df << 10) | (f->of << 11) | 0x200u | c->eflags_hi;
            break;
        case IR_OP_POPFD: {
            uint32_t v_ = M32(c->r[4]);
            c->r[4] += 4;
            f->cf = v_ & 1u; f->pf = (v_ >> 2) & 1u; f->zf = (v_ >> 6) & 1u;
            f->sf = (v_ >> 7) & 1u; c->df = (v_ >> 10) & 1u; f->of = (v_ >> 11) & 1u;
            c->eflags_hi = v_ & 0x00200000u;
            break;
        }
        case IR_OP_LEAVE:
            c->r[4] = c->r[5];
            c->r[5] = M32(c->r[4]);
            c->r[4] += 4;
            break;
        case IR_OP_SAHF: {
            uint32_t h_ = (c->r[0] >> 8) & 0xffu;
            f->cf = h_ & 1u; f->pf = (h_ >> 2) & 1u;
            f->zf = (h_ >> 6) & 1u; f->sf = (h_ >> 7) & 1u;
            break;
        }
        case IR_OP_LAHF:
            c->r[0] = (c->r[0] & 0xffff00ffu) |
                      ((f->cf | 2u | (f->pf << 2) | (f->zf << 6) | (f->sf << 7)) << 8);
            break;
        case IR_OP_CLC: f->cf = 0; break;
        case IR_OP_STC: f->cf = 1; break;
        case IR_OP_CMC: f->cf = !f->cf; break;
        case IR_OP_CLD: c->df = 0; break;
        case IR_OP_STD: c->df = 1; break;

        case IR_OP_JMP: {
            uint32_t t = block_end + (int32_t)imm;
            if (t >= frame_start && t < func_end) { *nextp = t; return 0; }
            dispatch(c, t, 0, b->start);
            return 1;
        }
        case IR_OP_JMP_IND: {
            uint32_t t = rd(c, w, 1, IR_SIZE_32, memea);
            if (t >= frame_start && t < func_end) { *nextp = t; return 0; }
            dispatch(c, t, 1, b->start);
            return 1;
        }
        case IR_OP_CALL: {
            uint32_t ret = block_end;
            uint32_t t = block_end + (int32_t)imm;
            c->r[4] -= 4; M32(c->r[4]) = ret;
            dispatch(c, t, 0, b->start);
            *nextp = ret; return 0;
        }
        case IR_OP_CALL_IND: {
            uint32_t ret = block_end;
            uint32_t t = rd(c, w, 1, IR_SIZE_32, memea);
            c->r[4] -= 4; M32(c->r[4]) = ret;
            dispatch(c, t, 1, b->start);
            *nextp = ret; return 0;
        }
        case IR_OP_RET:
            c->r[4] += 4 + (IR_IMMK(w) ? imm : 0);
            return 1;
        case IR_OP_LOOP:
        case IR_OP_LOOPE:
        case IR_OP_LOOPNE: {
            c->r[1] -= 1;
            int taken = c->r[1] != 0;
            if (op == IR_OP_LOOPE) taken = taken && f->zf;
            else if (op == IR_OP_LOOPNE) taken = taken && !f->zf;
            uint32_t nxt = taken ? block_end + (int32_t)imm : block_end;
            if (nxt >= frame_start && nxt < func_end) { *nextp = nxt; return 0; }
            dispatch(c, nxt, 0, b->start);
            return 1;
        }
        case IR_OP_JECXZ: {
            uint32_t nxt = c->r[1] == 0 ? block_end + (int32_t)imm : block_end;
            if (nxt >= frame_start && nxt < func_end) { *nextp = nxt; return 0; }
            dispatch(c, nxt, 0, b->start);
            return 1;
        }
        case IR_OP_STRING:
            do_string(c, f, w);
            break;
        case IR_OP_SSE:
            sse_exec(c, f, (uint32_t)imm, memea, (int)IR_SRCM(w), b->start);
            break;
        case IR_OP_X87:
            x87_exec(c, f, (uint32_t)imm, memea, (int)IR_SRCM(w), b->start);
            break;
        case IR_OP_CPUID:
            rt_cpuid(c, &c->r[0], &c->r[3], &c->r[1], &c->r[2]);
            break;
        case IR_OP_RDTSC: {
            uint64_t t_ = rt_rdtsc();
            c->r[0] = (uint32_t)t_;
            c->r[2] = (uint32_t)(t_ >> 32);
            break;
        }
        case IR_OP_INT3:
            rt_int3(c, imm);
            break;
        case IR_OP_INT:
            rt_unimpl(c, imm, "int");
            break;
        case IR_OP_XLATB:
            setr(c, 0, IR_SIZE_8, M8(c->r[3] + (uint8_t)c->r[0]));
            break;
        default:
            rt_unimpl(c, b->start, "IR: unsupported opcode in decoded stream");
        }
    }
    rt_unimpl(c, b->start, "IR: block has no terminator");
}

/* ---------------------------------------------------------------- entry */

void n3ds_ir_run(CPU *c, uint32_t addr, uint32_t func_end) {
    Flags f = {0};
    uint32_t ip = addr;
    for (;;) {
        IrExecBlock b;
        n3ds_lift_set_ir_ip(ip);
        if (n3ds_ir_fetch_block(c, ip, func_end, &b))
            rt_unimpl(c, ip, "IR: block does not fit the decode cache");
        uint32_t next = 0;
        if (exec_block(c, &b, &f, addr, func_end, &next)) return;
        ip = next;
    }
}

void n3ds_ir_call(CPU *c, uint32_t addr) {
    uint32_t end = n3ds_ir_end(addr);
    if (!end) rt_unimpl(c, addr, "unknown function/import");
    n3ds_ir_run(c, addr, end);
}
