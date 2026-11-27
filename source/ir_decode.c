/* x86-32 -> compact IR decoder and bounded decode cache.
 *
 * Decoding is on demand, one basic block at a time, straight from guest
 * memory through n3ds_guest_ptr(). Nothing is written to disk. The cache is a
 * single flat arena with a hash index; when the arena cannot hold the next
 * block it is flushed as a whole (see n3ds_ir_cache_stats). Eviction is
 * behaviour-neutral because decoding is a pure function of the guest bytes.
 *
 * Opcode coverage is deliberately explicit: anything the decoder does not
 * model raises "IR: unsupported opcode <name> at <addr>" through rt_unimpl, so
 * a missing instruction can never be skipped silently.
 */
#include "ir_core.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rt_unimpl(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));

#define IR_MAX_BLOCK_X86 65535u   /* x86_len is uint16 */
#define IR_MAX_BLOCK_INS 255u     /* count is uint8 */

typedef struct {
    CPU *c;
    uint32_t start, func_end, pos;
    uint32_t *out;
    size_t words, used;
    unsigned count;
    int soft;
    uint16_t term_op;
    jmp_buf jb;
    char err[56];
} Dec;

typedef struct {
    unsigned op;
    unsigned dst, src;
    unsigned dstm, srcm;
    unsigned size;          /* IR_SIZE_* */
    unsigned immk;
    uint32_t imm;           /* value for immk 1/2/3 */
    unsigned seg;
    unsigned aux;
    unsigned base, index, scale, dispk;
    int32_t disp;
} Ins;

/* ---------------------------------------------------------------- helpers */

static void dc_fail(Dec *d, uint32_t addr, const char *text) {
    char buf[96];
    snprintf(buf, sizeof buf, "%s at %08lx", text, (unsigned long)addr);
    if (d->soft) {
        snprintf(d->err, sizeof d->err, "%.*s", (int)sizeof d->err - 1, buf);
        longjmp(d->jb, 1);
    }
    rt_unimpl(d->c, addr, buf);
}

static void dc_fail_name(Dec *d, uint32_t addr, const char *name) {
    char buf[64];
    snprintf(buf, sizeof buf, "IR: unsupported opcode %s", name);
    dc_fail(d, addr, buf);
}

static void need_words(Dec *d, size_t n) {
    if (d->out && d->used + n > d->words)
        dc_fail(d, d->pos, "IR: block larger than the decode buffer");
}

static void put(Dec *d, uint32_t w) {
    need_words(d, 1);
    if (d->out) d->out[d->used] = w;
    d->used++;
}

static uint8_t fetch(Dec *d, uint32_t addr) {
    if (addr >= d->func_end)
        dc_fail(d, addr, "IR: block runs past function end");
    return *(const uint8_t *)n3ds_guest_ptr(addr, 1);
}

static const char *unsupported_name(unsigned op, int two) {
    if (!two) {
        if (op >= 0xd8 && op <= 0xdf) return "x87";
        switch (op) {
        case 0x06: return "push es";
        case 0x07: return "pop es";
        case 0x0e: return "push cs";
        case 0x16: return "push ss";
        case 0x17: return "pop ss";
        case 0x1e: return "push ds";
        case 0x1f: return "pop ds";
        case 0x27: return "daa";
        case 0x2f: return "das";
        case 0x37: return "aaa";
        case 0x3f: return "aas";
        case 0x62: return "bound";
        case 0x63: return "arpl";
        case 0x8c: return "mov from sreg";
        case 0x8e: return "mov to sreg";
        case 0x9a: return "callf";
        case 0xa0: case 0xa1: case 0xa2: case 0xa3: return "moffs mov";
        case 0xce: return "into";
        case 0xcf: return "iretd";
        case 0xd4: return "aam";
        case 0xd5: return "aad";
        case 0xd6: return "salc";
        case 0xea: return "jmpf";
        case 0xec: case 0xed: case 0xee: case 0xef: return "in/out";
        case 0xf1: return "int1";
        case 0xf4: return "hlt";
        case 0xfa: return "cli";
        case 0xfb: return "sti";
        default: return "one-byte";
        }
    }
    switch (op) {
    case 0x00: case 0x01: return "group6 sldt";
    case 0x05: return "syscall";
    case 0x06: case 0x07: case 0x08: case 0x09: return "group6";
    case 0x0b: return "ud2";
    case 0x0d: return "prefetchw";
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: return "sse mov";
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d:
    case 0x2e: case 0x2f: return "sse";
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
    case 0x56: case 0x57: case 0x58: case 0x59: case 0x5a: case 0x5b:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f: return "sse";
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65:
    case 0x66: case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b:
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: return "mmx/sse";
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79: case 0x7a: case 0x7b:
    case 0x7c: case 0x7d: case 0x7e: case 0x7f: return "mmx/sse";
    case 0xa0: case 0xa1: case 0xa8: case 0xa9: return "push/pop fs/gs";
    case 0xae: case 0xaf: return "group15";
    case 0xb8: return "popcnt";
    case 0xba: return "group8";
    case 0xc2: case 0xc3: return "sse";
    case 0xc4: case 0xc5: case 0xc6: return "sse";
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4: case 0xd5:
    case 0xd6: case 0xd7: case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf: return "mmx/sse";
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5:
    case 0xe6: case 0xe7: case 0xe8: case 0xe9: case 0xea: case 0xeb:
    case 0xec: case 0xed: case 0xee: case 0xef: return "mmx/sse";
    case 0xf0: case 0xf1: case 0xf2: case 0xf3: case 0xf4: case 0xf5:
    case 0xf6: case 0xf7: case 0xf8: case 0xf9: case 0xfa: case 0xfb:
    case 0xfc: case 0xfd: case 0xfe: case 0xff: return "mmx/sse";
    default: return "two-byte";
    }
}

/* ---------------------------------------------------------------- ModRM */

typedef struct {
    unsigned reg, rm, mem;
    unsigned raw;
    unsigned base, index, scale;
    unsigned dispk;
    int32_t disp;
} ModRM;

static ModRM read_modrm(Dec *d, uint32_t *pp) {
    ModRM m;
    memset(&m, 0, sizeof m);
    uint8_t b = fetch(d, (*pp)++);
    unsigned mod = b >> 6, reg = (b >> 3) & 7, rm = b & 7;
    m.raw = b;
    m.reg = reg; m.rm = rm;
    if (mod == 3) return m;
    m.mem = 1;
    m.base = 15; m.index = 15; m.scale = 0;
    if (rm == 4) {
        uint8_t sib = fetch(d, (*pp)++);
        unsigned ss = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        m.scale = ss;
        if (idx != 4) m.index = idx;
        if (base == 5 && mod == 0) {
            m.dispk = 2;
            m.disp = (int32_t)((uint32_t)fetch(d, *pp) | ((uint32_t)fetch(d, *pp + 1) << 8) |
                               ((uint32_t)fetch(d, *pp + 2) << 16) | ((uint32_t)fetch(d, *pp + 3) << 24));
            *pp += 4;
        } else {
            m.base = base;
        }
    } else if (mod == 0 && rm == 5) {
        m.dispk = 2;
        m.disp = (int32_t)((uint32_t)fetch(d, *pp) | ((uint32_t)fetch(d, *pp + 1) << 8) |
                           ((uint32_t)fetch(d, *pp + 2) << 16) | ((uint32_t)fetch(d, *pp + 3) << 24));
        *pp += 4;
    } else {
        m.base = rm;
    }
    if (mod == 1) {
        m.dispk = 1;
        m.disp = (int8_t)fetch(d, (*pp)++);
    } else if (mod == 2) {
        m.dispk = 2;
        m.disp = (int32_t)((uint32_t)fetch(d, *pp) | ((uint32_t)fetch(d, *pp + 1) << 8) |
                           ((uint32_t)fetch(d, *pp + 2) << 16) | ((uint32_t)fetch(d, *pp + 3) << 24));
        *pp += 4;
    }
    return m;
}

/* ---------------------------------------------------------------- emit */

static void ins_mem(Ins *in, const ModRM *m) {
    in->base = m->base; in->index = m->index;
    in->scale = m->scale; in->dispk = m->dispk;
    in->disp = m->disp;
}

static void emit(Dec *d, const Ins *in) {
    unsigned immk = in->immk;
    uint32_t imm = in->imm;
    if (immk == IR_IMM_S8 || immk == IR_IMM_U8) {
        int32_t v = (int32_t)imm;
        if ((immk == IR_IMM_S8 && (v < -128 || v > 127)) ||
            (immk == IR_IMM_U8 && (v < 0 || v > 255))) {
            immk = IR_IMM_32;
            imm = (uint32_t)v;
        }
    }
    uint32_t w = (uint32_t)in->op | ((uint32_t)in->dst << 8) |
                 ((uint32_t)in->dstm << 11) | ((uint32_t)in->src << 12) |
                 ((uint32_t)in->srcm << 15) | ((uint32_t)in->size << 16) |
                 ((uint32_t)immk << 18) | ((uint32_t)in->seg << 20) |
                 ((uint32_t)in->aux << 22);
    if (immk == IR_IMM_S8 || immk == IR_IMM_U8)
        w |= (uint32_t)(imm & 0xffu) << 24;
    put(d, w);
    if (in->dstm || in->srcm) {
        if (in->dispk == 2) {
            put(d, (uint32_t)in->base | ((uint32_t)in->index << 4) |
                   ((uint32_t)in->scale << 8) | (2u << 10));
            put(d, (uint32_t)in->disp);
        } else {
            uint32_t a = (uint32_t)in->base | ((uint32_t)in->index << 4) |
                         ((uint32_t)in->scale << 8) | ((uint32_t)in->dispk << 10);
            if (in->dispk == 1) a |= (uint32_t)((int32_t)in->disp & 0xff) << 24;
            put(d, a);
        }
    }
    if (immk == IR_IMM_32) put(d, imm);
}

/* ---------------------------------------------------------------- decode */

/* Forms the interpreter implements. Anything else is rejected at decode time,
 * so a missed x87 form is an explicit "IR: unsupported opcode x87 at <addr>",
 * never a runtime surprise. */
static int x87_form_ok(unsigned op, unsigned modrm) {
    unsigned mod = modrm >> 6, reg = (modrm >> 3) & 7;
    if (mod != 3) {
        switch (op) {
        case 0xd8: case 0xdc: case 0xda: case 0xde: return 1;
        case 0xd9: return reg != 1;
        case 0xdb: return reg == 0 || reg == 1 || reg == 2 || reg == 3 ||
                          reg == 5 || reg == 7;
        case 0xdd: return reg != 5;
        case 0xdf: return reg == 0 || reg == 1 || reg == 2 || reg == 3 ||
                          reg == 5 || reg == 6 || reg == 7;
        default: return 0;
        }
    }
    switch (op) {
    case 0xd8: return 1;
    case 0xd9:
        if (reg == 0 || reg == 1) return 1;
        if (modrm >= 0xd8 && modrm <= 0xdf) return 1;   /* fstp st(i) */
        switch (modrm) {
        case 0xd0: case 0xe0: case 0xe1: case 0xe4: case 0xe5: case 0xe8:
        case 0xe9: case 0xea: case 0xeb: case 0xec: case 0xed: case 0xee:
        case 0xf0: case 0xf1: case 0xf2: case 0xf3: case 0xf5: case 0xf6:
        case 0xf7: case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc:
        case 0xfd: case 0xfe: case 0xff:
            return 1;
        default: return 0;
        }
    case 0xda: return modrm == 0xe9 || reg <= 3;
    case 0xdb: return modrm == 0xe2 || modrm == 0xe3 || reg <= 3 || reg == 5 || reg == 6;
    case 0xdc: return 1;
    case 0xdd: return reg == 0 || reg == 2 || reg == 3 || reg == 4 || reg == 5;
    case 0xde: return 1;
    case 0xdf: return modrm == 0xe0 || reg == 5 || reg == 6 || reg == 0;
    default: return 0;
    }
}

static int sse_has_imm(unsigned op2) {
    return op2 == 0xc2 || op2 == 0xc6 || op2 == 0x70 || op2 == 0xc4 ||
           op2 == 0xc5 || op2 == 0x71 || op2 == 0x72 || op2 == 0x73;
}

static int sse_supported(unsigned op2, unsigned pfx) {
    switch (op2) {
    case 0x10: case 0x11: case 0x51: case 0xc2:
        return 1;
    case 0x28: case 0x29: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x50: case 0x54: case 0x55: case 0x56:
    case 0x57: case 0xc6:
        return pfx == 0 || pfx == 1;
    case 0x2b:                                  /* movntps / movntpd */
        return pfx == 0 || pfx == 1;
    case 0x2e: case 0x2f:
        return pfx == 0 || pfx == 1;
    case 0x2a: case 0x2c: case 0x2d:
        return pfx == 2 || pfx == 3;
    case 0x52: case 0x53:
        return pfx == 0 || pfx == 3;
    case 0x5a:
        return pfx == 0 || pfx == 2 || pfx == 3;
    case 0x5b:                                  /* cvtdq2ps / cvtps2dq / cvttps2dq */
        return pfx == 0 || pfx == 1 || pfx == 3;
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x67: case 0x68: case 0x69: case 0x6a:
    case 0x6b: case 0x6c: case 0x6d:
        return pfx == 1;                        /* packed interleave/pack */
    case 0xd8: case 0xd9: case 0xda: case 0xdc:
    case 0xdd: case 0xde:
        return pfx == 1;                        /* unsigned saturating add/sub */
    case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        return 1;
    case 0x6e: case 0x7e: case 0xd6:
        return pfx == 1 || pfx == 3;
    case 0x6f: case 0x7f:
        return pfx == 1 || pfx == 3;
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x64: case 0x65:
    case 0x66: case 0xdb: case 0xdf: case 0xeb: case 0xef: case 0xd4:
    case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd:
    case 0xfe: case 0xd7: case 0xc5: case 0xc4:
    case 0xd1: case 0xd2: case 0xd3: case 0xe1: case 0xe2:
    case 0xf1: case 0xf2: case 0xf3: case 0xe7:
    case 0xea: case 0xee: case 0xd5: case 0xe5: case 0xe0: case 0xf5:
        return pfx == 1;
    case 0xe6:
        return pfx == 3;
    case 0xae:
        return pfx == 0;
    default:
        return 0;
    }
}

/* Returns 1 when the instruction is a block terminator. */
static int decode_one(Dec *d) {
    uint32_t p = d->pos;
    unsigned opsz = 32, seg = IR_SEG_NONE, rep = 0, pfx = 0;
    for (;;) {
        uint8_t b = fetch(d, p);
        if (b == 0x66) { opsz = 16; pfx = 1; p++; }
        else if (b == 0x67) dc_fail(d, p, "IR: unsupported opcode address-size prefix");
        else if (b == 0xf0) { p++; }                 /* lock: single-threaded guest */
        else if (b == 0xf2) { rep = 3; pfx = 2; p++; }  /* repne / sse prefix */
        else if (b == 0xf3) { rep = 1; pfx = 3; p++; }  /* rep / sse prefix */
        else if (b == 0x2e || b == 0x36 || b == 0x3e || b == 0x26) { p++; }
        else if (b == 0x64) { seg = IR_SEG_FS; p++; }
        else if (b == 0x65) { seg = IR_SEG_GS; p++; }
        else break;
    }
    if (seg == IR_SEG_GS) dc_fail(d, p, "IR: unsupported opcode gs segment");

    uint8_t op = fetch(d, p++);
    Ins in;
    memset(&in, 0, sizeof in);
    in.seg = seg;
    in.base = 15; in.index = 15;
    int term = 0;

    if (op >= 0xd8 && op <= 0xdf) {
        ModRM m = read_modrm(d, &p);
        if (!x87_form_ok(op, m.raw))
            dc_fail_name(d, d->pos, "x87");
        in.op = IR_OP_X87;
        in.immk = IR_IMM_32;
        in.imm = ((uint32_t)op << 8) | m.raw;
        if (m.mem) { in.srcm = 1; ins_mem(&in, &m); }
        emit(d, &in);
        d->pos = p;
        d->count++;
        return 0;
    }

#define SZ(b) (((b) == 16) ? IR_SIZE_16 : IR_SIZE_32)
#define REJECT(name) dc_fail(d, d->pos, "IR: unsupported opcode " name)
#define BAD() dc_fail_name(d, d->pos, unsupported_name(op, 0))
#define BAD2(x) dc_fail_name(d, d->pos, unsupported_name((x), 1))

    if (op < 0x40 && (op & 7) <= 5) {
        unsigned form = op & 7, alu = op >> 3;
        static const unsigned alu_op[8] = {
            IR_OP_ADD, IR_OP_OR, IR_OP_ADC, IR_OP_SBB,
            IR_OP_AND, IR_OP_SUB, IR_OP_XOR, IR_OP_CMP
        };
        if (alu >= 8) BAD();
        in.op = alu_op[alu];
        if (form <= 3) {
            ModRM m = read_modrm(d, &p);
            in.size = (form == 0 || form == 2) ? IR_SIZE_8 : SZ(opsz);
            if (form == 0 || form == 1) {
                in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
                in.src = m.reg;
            } else {
                in.dst = m.reg;
                in.src = m.rm; in.srcm = m.mem; if (m.mem) ins_mem(&in, &m);
            }
        } else if (form == 4) {
            in.size = IR_SIZE_8;
            in.dst = 0;  /* al */
            in.immk = IR_IMM_U8;
            in.imm = fetch(d, p++);
        } else {
            in.size = SZ(opsz);
            in.dst = 0;  /* eax / ax */
            in.immk = IR_IMM_32;
            if (opsz == 16) { in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8); p += 2; }
            else {
                in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                         ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
                p += 4;
            }
        }
    } else if (op >= 0x40 && op <= 0x47) {
        in.op = IR_OP_INC; in.dst = op & 7; in.size = SZ(opsz);
    } else if (op >= 0x48 && op <= 0x4f) {
        in.op = IR_OP_DEC; in.dst = op & 7; in.size = SZ(opsz);
    } else if (op >= 0x50 && op <= 0x57) {
        in.op = IR_OP_PUSH; in.src = op & 7; in.size = SZ(opsz);
    } else if (op >= 0x58 && op <= 0x5f) {
        in.op = IR_OP_POP; in.dst = op & 7; in.size = SZ(opsz);
    } else if (op == 0x60) {
        if (opsz != 32) dc_fail(d, d->pos, "IR: unsupported opcode pusha16");
        in.op = IR_OP_PUSHA; in.size = SZ(opsz);
    } else if (op == 0x61) {
        if (opsz != 32) dc_fail(d, d->pos, "IR: unsupported opcode popa16");
        in.op = IR_OP_POPA; in.size = SZ(opsz);
    } else if (op == 0x68) {
        in.op = IR_OP_PUSH; in.size = SZ(opsz); in.immk = IR_IMM_32;
        if (opsz == 16) { in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8); p += 2; }
        else {
            in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                     ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
            p += 4;
        }
    } else if (op == 0x6a) {
        in.op = IR_OP_PUSH; in.size = SZ(opsz); in.immk = IR_IMM_S8;
        in.imm = (int8_t)fetch(d, p++);
    } else if (op == 0x69 || op == 0x6b) {
        ModRM m = read_modrm(d, &p);
        in.op = IR_OP_IMUL3;
        in.dst = m.reg;
        in.src = m.rm; in.srcm = m.mem; if (m.mem) ins_mem(&in, &m);
        in.size = SZ(opsz);
        if (op == 0x6b) { in.immk = IR_IMM_S8; in.imm = (int8_t)fetch(d, p++); }
        else {
            in.immk = IR_IMM_32;
            if (opsz == 16) { in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8); p += 2; }
            else {
                in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                         ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
                p += 4;
            }
        }
    } else if (op >= 0x70 && op <= 0x7f) {
        in.op = IR_OP_JCC_BASE + (op & 0xf);
        in.immk = IR_IMM_S8; in.imm = (int8_t)fetch(d, p++);
        term = 1;
    } else if (op == 0x80 || op == 0x81 || op == 0x83) {
        ModRM m = read_modrm(d, &p);
        unsigned alu = m.reg;
        static const unsigned alu_op[8] = {
            IR_OP_ADD, IR_OP_OR, IR_OP_ADC, IR_OP_SBB,
            IR_OP_AND, IR_OP_SUB, IR_OP_XOR, IR_OP_CMP
        };
        in.op = alu_op[alu];
        in.size = op == 0x80 ? IR_SIZE_8 : SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        if (op == 0x81) {
            in.immk = IR_IMM_32;
            if (opsz == 16) { in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8); p += 2; }
            else {
                in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                         ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
                p += 4;
            }
        } else {
            in.immk = (op == 0x83) ? IR_IMM_S8 : IR_IMM_U8;
            in.imm = op == 0x83 ? (int8_t)fetch(d, p++) : fetch(d, p++);
        }
    } else if (op == 0x84 || op == 0x85) {
        ModRM m = read_modrm(d, &p);
        in.op = IR_OP_TEST; in.size = op == 0x84 ? IR_SIZE_8 : SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        in.src = m.reg;
    } else if (op == 0x86 || op == 0x87) {
        ModRM m = read_modrm(d, &p);
        in.op = IR_OP_XCHG; in.size = op == 0x86 ? IR_SIZE_8 : SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        in.src = m.reg;
    } else if (op == 0x88 || op == 0x89) {
        ModRM m = read_modrm(d, &p);
        in.op = IR_OP_MOV; in.size = op == 0x88 ? IR_SIZE_8 : SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        in.src = m.reg;
    } else if (op == 0x8a || op == 0x8b) {
        ModRM m = read_modrm(d, &p);
        in.op = IR_OP_MOV; in.size = op == 0x8a ? IR_SIZE_8 : SZ(opsz);
        in.dst = m.reg;
        in.src = m.rm; in.srcm = m.mem; if (m.mem) ins_mem(&in, &m);
    } else if (op == 0x8c) {
        ModRM m = read_modrm(d, &p);
        static const uint32_t sel[8] = {0x2b, 0x23, 0x2b, 0x2b, 0x53, 0x2b, 0, 0};
        if (m.reg >= 6) dc_fail(d, d->pos, "IR: unsupported opcode mov from sreg");
        in.op = IR_OP_MOV; in.size = SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        in.immk = IR_IMM_32; in.imm = sel[m.reg];
    } else if (op == 0x8d) {
        ModRM m = read_modrm(d, &p);
        if (!m.mem) dc_fail(d, d->pos, "IR: unsupported opcode lea register");
        in.op = IR_OP_LEA; in.size = SZ(opsz); in.dst = m.reg;
        in.srcm = 1; ins_mem(&in, &m);
    } else if (op == 0x8f) {
        ModRM m = read_modrm(d, &p);
        in.op = IR_OP_POP; in.size = SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
    } else if (op == 0x90) {
        in.op = IR_OP_NOP;
    } else if (op >= 0x91 && op <= 0x97) {
        in.op = IR_OP_XCHG; in.size = SZ(opsz); in.dst = 0; in.src = op & 7;
    } else if (op == 0x98) {
        in.op = IR_OP_CWDE; in.size = SZ(opsz);
    } else if (op == 0x99) {
        in.op = IR_OP_CDQ; in.size = SZ(opsz);
    } else if (op == 0x9b) {
        in.op = IR_OP_NOP;
    } else if (op == 0x9c) {
        if (opsz != 32) dc_fail(d, d->pos, "IR: unsupported opcode pushfw");
        in.op = IR_OP_PUSHFD; in.size = SZ(opsz);
    } else if (op == 0x9d) {
        if (opsz != 32) dc_fail(d, d->pos, "IR: unsupported opcode popfw");
        in.op = IR_OP_POPFD; in.size = SZ(opsz);
    } else if (op == 0x9e) {
        in.op = IR_OP_SAHF;
    } else if (op == 0x9f) {
        in.op = IR_OP_LAHF;
    } else if (op >= 0xa0 && op <= 0xa3) {
        uint32_t addr = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                        ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
        p += 4;
        in.op = IR_OP_MOV;
        in.base = 15; in.index = 15; in.dispk = 2; in.disp = (int32_t)addr;
        if (op == 0xa0 || op == 0xa1) {
            in.size = op == 0xa0 ? IR_SIZE_8 : SZ(opsz);
            in.dst = 0; in.srcm = 1;
        } else {
            in.size = op == 0xa2 ? IR_SIZE_8 : SZ(opsz);
            in.src = 0; in.dstm = 1;
        }
    } else if (op == 0xa4 || op == 0xa5) {
        in.op = IR_OP_STRING; in.dst = 0; /* movs */
        in.size = op == 0xa4 ? IR_SIZE_8 : SZ(opsz); in.aux = rep;
    } else if (op == 0xa6 || op == 0xa7) {
        in.op = IR_OP_STRING; in.dst = 3; /* cmps */
        in.size = op == 0xa6 ? IR_SIZE_8 : SZ(opsz); in.aux = rep;
    } else if (op == 0xaa || op == 0xab) {
        in.op = IR_OP_STRING; in.dst = 1; /* stos */
        in.size = op == 0xaa ? IR_SIZE_8 : SZ(opsz); in.aux = rep;
    } else if (op == 0xac || op == 0xad) {
        in.op = IR_OP_STRING; in.dst = 2; /* lods */
        in.size = op == 0xac ? IR_SIZE_8 : SZ(opsz); in.aux = rep;
    } else if (op == 0xae || op == 0xaf) {
        in.op = IR_OP_STRING; in.dst = 4; /* scas */
        in.size = op == 0xae ? IR_SIZE_8 : SZ(opsz); in.aux = rep;
    } else if (op == 0xa8 || op == 0xa9) {
        in.op = IR_OP_TEST; in.size = op == 0xa8 ? IR_SIZE_8 : SZ(opsz);
        in.dst = 0;
        in.immk = IR_IMM_32;
        if (op == 0xa8) { in.immk = IR_IMM_U8; in.imm = fetch(d, p++); }
        else if (opsz == 16) { in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8); p += 2; }
        else {
            in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                     ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
            p += 4;
        }
    } else if (op >= 0xb0 && op <= 0xb7) {
        in.op = IR_OP_MOV; in.size = IR_SIZE_8; in.dst = op & 7;
        in.immk = IR_IMM_U8; in.imm = fetch(d, p++);
    } else if (op >= 0xb8 && op <= 0xbf) {
        in.op = IR_OP_MOV; in.size = SZ(opsz); in.dst = op & 7;
        in.immk = IR_IMM_32;
        if (opsz == 16) { in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8); p += 2; }
        else {
            in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                     ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
            p += 4;
        }
    } else if (op == 0xc0 || op == 0xc1 || op == 0xd0 || op == 0xd1 ||
               op == 0xd2 || op == 0xd3) {
        ModRM m = read_modrm(d, &p);
        static const unsigned sh[8] = {
            IR_OP_ROL, IR_OP_ROR, IR_OP_RCL, IR_OP_RCR,
            IR_OP_SHL, IR_OP_SHR, IR_OP_SHL, IR_OP_SAR
        };
        in.op = sh[m.reg];
        in.size = (op == 0xc0 || op == 0xd0 || op == 0xd2) ? IR_SIZE_8 : SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        if (op == 0xc0 || op == 0xc1) { in.immk = IR_IMM_U8; in.imm = fetch(d, p++); }
        else if (op == 0xd0 || op == 0xd1) { in.immk = IR_IMM_U8; in.imm = 1; }
        else in.aux = IR_AUX_SHIFT_CL;
    } else if (op == 0xc2) {
        in.op = IR_OP_RET; in.immk = IR_IMM_32;
        in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8);
        p += 2; term = 1;
    } else if (op == 0xc3) {
        in.op = IR_OP_RET; term = 1;
    } else if (op == 0xc6 || op == 0xc7) {
        ModRM m = read_modrm(d, &p);
        in.op = IR_OP_MOV; in.size = op == 0xc6 ? IR_SIZE_8 : SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        if (op == 0xc6) { in.immk = IR_IMM_U8; in.imm = fetch(d, p++); }
        else if (opsz == 16) {
            in.immk = IR_IMM_32;
            in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8); p += 2;
        } else {
            in.immk = IR_IMM_32;
            in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                     ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
            p += 4;
        }
    } else if (op == 0xc9) {
        in.op = IR_OP_LEAVE;
    } else if (op == 0xd7) {
        in.op = IR_OP_XLATB;
    } else if (op == 0xcc) {
        in.op = IR_OP_INT3; in.immk = IR_IMM_32; in.imm = d->pos; term = 1;
    } else if (op == 0xcd) {
        in.op = IR_OP_INT; in.immk = IR_IMM_32;
        in.imm = fetch(d, p++); term = 1;
    } else if (op == 0xe0 || op == 0xe1 || op == 0xe2) {
        in.op = op == 0xe0 ? IR_OP_LOOPNE : op == 0xe1 ? IR_OP_LOOPE : IR_OP_LOOP;
        in.immk = IR_IMM_S8; in.imm = (int8_t)fetch(d, p++); term = 1;
    } else if (op == 0xe3) {
        in.op = IR_OP_JECXZ;
        in.immk = IR_IMM_S8; in.imm = (int8_t)fetch(d, p++); term = 1;
    } else if (op == 0xe8 || op == 0xe9) {
        in.op = op == 0xe8 ? IR_OP_CALL : IR_OP_JMP;
        in.immk = IR_IMM_32;
        in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                 ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
        p += 4; term = 1;
    } else if (op == 0xeb) {
        in.op = IR_OP_JMP; in.immk = IR_IMM_S8; in.imm = (int8_t)fetch(d, p++); term = 1;
    } else if (op == 0xf5) {
        in.op = IR_OP_CMC;
    } else if (op == 0xf6 || op == 0xf7) {
        ModRM m = read_modrm(d, &p);
        in.size = op == 0xf6 ? IR_SIZE_8 : SZ(opsz);
        in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        if (m.reg <= 1) {
            in.op = IR_OP_TEST;
            if (op == 0xf6) { in.immk = IR_IMM_U8; in.imm = fetch(d, p++); }
            else if (opsz == 16) {
                in.immk = IR_IMM_32;
                in.imm = fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8); p += 2;
            } else {
                in.immk = IR_IMM_32;
                in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                         ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
                p += 4;
            }
        } else {
            switch (m.reg) {
            case 2: in.op = IR_OP_NOT; break;
            case 3: in.op = IR_OP_NEG; break;
            case 4: in.op = IR_OP_MUL1; in.dstm = 0; in.dst = 0; break;
            case 5: in.op = IR_OP_IMUL1; in.dstm = 0; in.dst = 0; break;
            case 6: in.op = IR_OP_DIV; in.dstm = 0; in.dst = 0; break;
            default: in.op = IR_OP_IDIV; in.dstm = 0; in.dst = 0; break;
            }
            if (in.dstm == 0 && (m.reg >= 4)) {
                /* one-operand implicit form: the ModRM operand is the source */
                in.src = m.rm; in.srcm = m.mem;
            }
        }
    } else if (op == 0xf8) {
        in.op = IR_OP_CLC;
    } else if (op == 0xf9) {
        in.op = IR_OP_STC;
    } else if (op == 0xfc) {
        in.op = IR_OP_CLD;
    } else if (op == 0xfd) {
        in.op = IR_OP_STD;
    } else if (op == 0xfe || op == 0xff) {
        ModRM m = read_modrm(d, &p);
        in.size = op == 0xfe ? IR_SIZE_8 : SZ(opsz);
        if (op == 0xfe) {
            if (m.reg == 0) in.op = IR_OP_INC;
            else if (m.reg == 1) in.op = IR_OP_DEC;
            else REJECT("group4");
            in.dst = m.rm; in.dstm = m.mem; if (m.mem) ins_mem(&in, &m);
        } else {
            switch (m.reg) {
            case 0: in.op = IR_OP_INC; in.dst = m.rm; in.dstm = m.mem; break;
            case 1: in.op = IR_OP_DEC; in.dst = m.rm; in.dstm = m.mem; break;
            case 2: in.op = IR_OP_CALL_IND; in.size = SZ(opsz); in.src = m.rm;
                    in.srcm = m.mem; term = 1; break;
            case 4: in.op = IR_OP_JMP_IND; in.size = SZ(opsz); in.src = m.rm;
                    in.srcm = m.mem; term = 1; break;
            case 6: in.op = IR_OP_PUSH; in.size = SZ(opsz); in.src = m.rm;
                    in.srcm = m.mem; break;
            default: REJECT("group5");
            }
            if (m.mem) ins_mem(&in, &m);
        }
    } else if (op == 0x0f) {
        uint8_t op2 = fetch(d, p++);
        if (sse_supported(op2, pfx)) {
            ModRM m = read_modrm(d, &p);
            if (m.mem && (op2 == 0x50 || op2 == 0xc5 || op2 == 0xd7))
                dc_fail_name(d, d->pos, "sse");
            unsigned imm8 = sse_has_imm(op2) ? fetch(d, p++) : 0;
            in.op = IR_OP_SSE;
            in.immk = IR_IMM_32;
            in.size = IR_SIZE_32;
            in.imm = ((uint32_t)op2 << 24) | ((uint32_t)pfx << 22) |
                     ((uint32_t)m.raw << 8) | imm8;
            if (m.mem) { in.srcm = 1; ins_mem(&in, &m); }
            emit(d, &in);
            d->pos = p;
            d->count++;
            return 0;
        }
        unsigned opsz2 = opsz;
        if (op2 >= 0x40 && op2 <= 0x4f) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = IR_OP_CMOVCC_BASE + (op2 & 0xf);
            in.size = SZ(opsz2); in.dst = m.reg;
            in.src = m.rm; in.srcm = m.mem;
        } else if (op2 >= 0x80 && op2 <= 0x8f) {
            in.op = IR_OP_JCC_BASE + (op2 & 0xf);
            in.immk = IR_IMM_32;
            in.imm = (uint32_t)fetch(d, p) | ((uint32_t)fetch(d, p + 1) << 8) |
                     ((uint32_t)fetch(d, p + 2) << 16) | ((uint32_t)fetch(d, p + 3) << 24);
            p += 4; term = 1;
        } else if (op2 >= 0x90 && op2 <= 0x9f) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = IR_OP_SETCC_BASE + (op2 & 0xf);
            in.size = IR_SIZE_8; in.dst = m.rm; in.dstm = m.mem;
        } else if (op2 == 0x1f || op2 == 0x18 || op2 == 0x0d || op2 == 0x77) {
            (void)read_modrm(d, &p);
            in.op = IR_OP_NOP;
        } else if (op2 == 0x31) {
            in.op = IR_OP_RDTSC;
        } else if (op2 == 0xa2) {
            in.op = IR_OP_CPUID;
        } else if (op2 == 0xa3 || op2 == 0xab || op2 == 0xb3 || op2 == 0xbb) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = op2 == 0xa3 ? IR_OP_BT : op2 == 0xab ? IR_OP_BTS :
                    op2 == 0xb3 ? IR_OP_BTR : IR_OP_BTC;
            in.size = SZ(opsz2);
            in.dst = m.rm; in.dstm = m.mem;
            in.src = m.reg;
        } else if (op2 == 0xa4 || op2 == 0xa5 || op2 == 0xac || op2 == 0xad) {
            ModRM m = read_modrm(d, &p);
            if (opsz2 != 32) dc_fail(d, d->pos, "IR: unsupported opcode shld16");
            if (m.mem) ins_mem(&in, &m);
            in.op = (op2 == 0xa4 || op2 == 0xa5) ? IR_OP_SHLD : IR_OP_SHRD;
            in.size = IR_SIZE_32;
            in.dst = m.rm; in.dstm = m.mem; in.src = m.reg;
            if (op2 == 0xa4 || op2 == 0xac) { in.immk = IR_IMM_U8; in.imm = fetch(d, p++); }
            else in.aux = IR_AUX_SHIFT_CL;
        } else if (op2 == 0xaf) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = IR_OP_IMUL2; in.size = SZ(opsz2);
            in.dst = m.reg; in.src = m.rm; in.srcm = m.mem;
        } else if (op2 == 0xb0 || op2 == 0xb1) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = IR_OP_CMPXCHG; in.size = op2 == 0xb0 ? IR_SIZE_8 : SZ(opsz2);
            in.dst = m.rm; in.dstm = m.mem; in.src = m.reg;
        } else if (op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = op2 == 0xb6 ? IR_OP_MOVZX8 : op2 == 0xb7 ? IR_OP_MOVZX16 :
                    op2 == 0xbe ? IR_OP_MOVSX8 : IR_OP_MOVSX16;
            in.size = SZ(opsz2); in.dst = m.reg;
            in.src = m.rm; in.srcm = m.mem;
        } else if (op2 == 0xba) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = m.reg == 4 ? IR_OP_BT : m.reg == 5 ? IR_OP_BTS :
                    m.reg == 6 ? IR_OP_BTR : m.reg == 7 ? IR_OP_BTC :
                    (dc_fail(d, d->pos, "IR: unsupported opcode group8"), IR_OP_NOP);
            in.size = SZ(opsz2);
            in.dst = m.rm; in.dstm = m.mem;
            in.immk = IR_IMM_U8; in.imm = fetch(d, p++);
        } else if (op2 == 0xbc || op2 == 0xbd) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = op2 == 0xbc ? IR_OP_BSF : IR_OP_BSR;
            in.size = SZ(opsz2);
            in.dst = m.reg; in.src = m.rm; in.srcm = m.mem;
        } else if (op2 == 0xc0 || op2 == 0xc1) {
            ModRM m = read_modrm(d, &p);
            if (m.mem) ins_mem(&in, &m);
            in.op = IR_OP_XADD; in.size = op2 == 0xc0 ? IR_SIZE_8 : SZ(opsz2);
            in.dst = m.rm; in.dstm = m.mem; in.src = m.reg;
        } else if (op2 == 0xc7) {
            ModRM m = read_modrm(d, &p);
            if (m.reg != 1 || !m.mem) dc_fail(d, d->pos, "IR: unsupported opcode group9");
            ins_mem(&in, &m);
            in.op = IR_OP_CMPXCHG8B; in.size = IR_SIZE_32;
            in.dstm = 1;
        } else if (op2 >= 0xc8 && op2 <= 0xcf) {
            in.op = IR_OP_BSWAP; in.size = IR_SIZE_32; in.dst = op2 & 7;
        } else {
            BAD2(op2);
        }
    } else {
        BAD();
    }
    emit(d, &in);
    d->pos = p;
    d->count++;
    if (term) d->term_op = in.op;
    return term;
#undef SZ
#undef REJECT
#undef BAD
#undef BAD2
}

/* ---------------------------------------------------------------- cache */

/* Small L1/L2-resident mirror of the decoded-block index. Blocks are looked up
 * once per execution, so this is the hottest table in the interpreter. */
#define IR_BCACHE 4096u
static struct { uint32_t start, off, bytes; } g_bcache[IR_BCACHE];

typedef struct {
    uint32_t start;
    uint16_t x86_len;
    uint8_t count;
    uint8_t term;
} IrBlockHeader;

typedef struct {
    uint32_t start;
    uint32_t off;      /* offset into arena + 1; 0 = empty */
    uint32_t bytes;
} IrSlot;

static uint8_t *g_arena;
static size_t g_cap, g_used;
static IrSlot *g_slots;
static unsigned g_nslots;
static IrCacheStats g_stats;

static uint32_t hash32(uint32_t v) {
    v *= 2654435761u;
    return v ^ (v >> 16);
}

static void cache_slot_clear(void) {
    if (g_slots) memset(g_slots, 0, (size_t)g_nslots * sizeof *g_slots);
    memset(g_bcache, 0, sizeof g_bcache);
    g_used = 0;
}

int n3ds_ir_cache_init(size_t bytes) {
    if (!bytes) bytes = N3DS_IR_DEFAULT_CACHE;
    size_t cap = 4096;
    while (cap < bytes && cap < (SIZE_MAX / 2)) cap <<= 1;
    unsigned nslots = 1024;
    while ((size_t)nslots * 2 < cap / 32 && nslots < (1u << 20)) nslots <<= 1;
    uint8_t *a = (uint8_t *)calloc(1, cap);
    IrSlot *s = (IrSlot *)calloc(nslots, sizeof *s);
    if (!a || !s) { free(a); free(s); return -1; }
    free(g_arena); free(g_slots);
    g_arena = a; g_cap = cap; g_slots = s; g_nslots = nslots;
    cache_slot_clear();
    memset(&g_stats, 0, sizeof g_stats);
    return 0;
}

void n3ds_ir_cache_reset(void) { cache_slot_clear(); }

void n3ds_ir_cache_stats(IrCacheStats *out) { if (out) *out = g_stats; }

static IrSlot *slot_find(uint32_t start) {
    unsigned h = hash32(start) & (g_nslots - 1);
    for (unsigned k = 0; k < g_nslots; ++k) {
        IrSlot *s = &g_slots[(h + k) & (g_nslots - 1)];
        if (!s->off) return NULL;
        if (s->start == start) return s;
    }
    return NULL;
}

static IrSlot *slot_insert(uint32_t start, uint32_t off, uint32_t bytes) {
    unsigned h = hash32(start) & (g_nslots - 1);
    for (unsigned k = 0; k < g_nslots; ++k) {
        IrSlot *s = &g_slots[(h + k) & (g_nslots - 1)];
        if (!s->off || s->start == start) {
            s->start = start; s->off = off; s->bytes = bytes;
            return s;
        }
    }
    return NULL;
}

/* Decode `start` into the arena. Returns the cached block or NULL after a
 * flush-triggered retry would still not fit. */
#define IR_CACHE_RESERVE 8192u /* a basic block larger than this is rejected */

static const IrBlockHeader *cache_decode(CPU *c, uint32_t start, uint32_t func_end, uint32_t *stream_words) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        size_t off = (g_used + 3u) & ~(size_t)3u;
        if (off + sizeof(IrBlockHeader) + IR_CACHE_RESERVE > g_cap && off) {
            /* Not enough room for a normal block: flush before decoding so a
             * capacity shortfall never turns into a spurious decode fault. */
            g_stats.flushes++;
            g_stats.evicted += g_used;
            cache_slot_clear();
            off = 0;
        }
        size_t cap = g_cap > off + sizeof(IrBlockHeader) ? (g_cap - off - sizeof(IrBlockHeader)) / 4 : 0;
        if (cap) {
            IrBlockInfo info;
            memset(&info, 0, sizeof info);
            size_t words = n3ds_ir_encode_block(c, start, func_end,
                                                (uint32_t *)(g_arena + off + sizeof(IrBlockHeader)),
                                                cap, &info, 0);
            if (words) {
                IrBlockHeader *h = (IrBlockHeader *)(g_arena + off);
                h->start = start;
                h->x86_len = (uint16_t)(info.end_ip - start);
                h->count = (uint8_t)info.insn_count;
                h->term = 0;
                uint32_t total = (uint32_t)(sizeof(IrBlockHeader) + words * 4);
                if (slot_insert(start, (uint32_t)off + 1u, total)) {
                    g_used = off + total;
                    *stream_words = (uint32_t)words;
                    g_stats.blocks++;
                    g_stats.bytes += total;
                    return h;
                }
                /* index full: treat like an arena flush */
            }
        }
        /* Whole-arena flush: deterministic and bounded; decoded code is a pure
         * function of the guest bytes, so dropping it cannot change behaviour. */
        g_stats.flushes++;
        g_stats.evicted += g_used;
        cache_slot_clear();
    }
    return NULL;
}

int n3ds_ir_fetch_block(CPU *c, uint32_t start, uint32_t func_end, IrExecBlock *out) {
    if (!g_arena) {
        if (n3ds_ir_cache_init(0)) return -1;
    }
    uint32_t words = 0;
    const IrBlockHeader *h = NULL;
    uint32_t ci = (start >> 2) & (IR_BCACHE - 1);
    uint32_t off, bytes;
    if (g_bcache[ci].start == start && g_bcache[ci].off) {
        /* Small direct-mapped mirror of the decoded-block index: the hot loop
         * revisits the same blocks, and this avoids a random probe in the much
         * larger open-addressed slot table. */
        g_stats.hits++;
        off = g_bcache[ci].off;
        bytes = g_bcache[ci].bytes;
    } else {
        IrSlot *s = slot_find(start);
        if (s) {
            g_stats.hits++;
            off = s->off;
            bytes = s->bytes;
            g_bcache[ci].start = start;
            g_bcache[ci].off = off;
            g_bcache[ci].bytes = bytes;
        } else {
            g_stats.misses++;
            if (!func_end) return -1;
            h = cache_decode(c, start, func_end, &words);
            if (!h) return -1;
            off = (uint32_t)((const uint8_t *)h - g_arena) + 1u;
            bytes = (uint32_t)(sizeof(IrBlockHeader) + words * 4u);
            g_bcache[ci].start = start;
            g_bcache[ci].off = off;
            g_bcache[ci].bytes = bytes;
        }
    }
    if (!h) {
        words = (bytes - (uint32_t)sizeof(IrBlockHeader)) / 4u;
        h = (const IrBlockHeader *)(g_arena + (off - 1u));
    }
    out->start = h->start;
    out->x86_len = h->x86_len;
    out->insn_count = h->count;
    out->stream_words = words;
    out->stream = (const uint32_t *)(h + 1);
    return 0;
}

/* ---------------------------------------------------------------- sizes */

size_t n3ds_ir_encode_block(CPU *c, uint32_t start, uint32_t func_end,
                            uint32_t *buf, size_t words, IrBlockInfo *info,
                            int soft) {
    if (info) memset(info, 0, sizeof *info);
    Dec d;
    memset(&d, 0, sizeof d);
    d.c = c; d.start = start; d.func_end = func_end; d.pos = start;
    d.out = buf; d.words = words; d.soft = soft;
    if (soft && setjmp(d.jb)) {
        if (info) {
            info->start = start;
            info->end_ip = d.pos;
            info->insn_count = (uint16_t)d.count;
            info->ir_bytes = 0;
            snprintf(info->error, sizeof info->error, "%s", d.err);
        }
        return 0;
    }
    int terminated = 0;
    while (d.count < IR_MAX_BLOCK_INS) {
        int term = decode_one(&d);
        if (term) { terminated = 1; break; }
        if (d.pos - start > IR_MAX_BLOCK_X86)
            dc_fail(&d, d.pos, "IR: block too long");
    }
    if (d.count == 0) dc_fail(&d, start, "IR: empty block");
    if (!terminated) dc_fail(&d, start, "IR: block has no terminator");
    if (d.pos - start > IR_MAX_BLOCK_X86)
        dc_fail(&d, d.pos, "IR: block too long");
    if (sizeof(IrBlockHeader) + d.used * 4 > 0xffffu)
        dc_fail(&d, start, "IR: block larger than 64 KiB");
    if (info) {
        info->start = start;
        info->end_ip = d.pos;
        info->insn_count = (uint16_t)d.count;
        info->term_op = d.term_op;
        info->ir_bytes = (uint16_t)(sizeof(IrBlockHeader) + d.used * 4);
    }
    return d.used * 4;
}

/* ---------------------------------------------------------------- table */
/* n3ds_ir_bind/has/end live in ir_core.c. */

/* The cache arena is memory the interpreter must own; keep allocation in one
 * place so a device build can raise N3DS_IR_DEFAULT_CACHE without touching the
 * runtime. */

#ifdef N3DS_HOST_PROF
#include "hprof.h"
#if defined(_WIN32)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include <stdio.h>
uint64_t hprof_cycles[HP_COUNT];
uint64_t hprof_calls[HP_COUNT];
uint64_t hprof_rdtsc(void) {
#if defined(_WIN32)
    return (uint64_t)__rdtsc();
#else
    return __rdtsc();
#endif
}
void hprof_reset(void) {
    for (unsigned i = 0; i < HP_COUNT; ++i) { hprof_cycles[i] = 0; hprof_calls[i] = 0; }
}
static const char *hprof_name(unsigned i) {
    static const char *names[HP_COUNT] = {
        "exec_block", "block_fetch", "dispatch", "irc_lookup",
        "ir_end", "guest_read32", "sync_wait"
    };
    return i < HP_COUNT ? names[i] : "?";
}
void hprof_dump(void) {
    fprintf(stderr, "[hprof] cycles calls slot\n");
    for (unsigned i = 0; i < HP_COUNT; ++i)
        fprintf(stderr, "[hprof] %llu %llu %s\n",
                (unsigned long long)hprof_cycles[i],
                (unsigned long long)hprof_calls[i], hprof_name(i));
}
#endif

