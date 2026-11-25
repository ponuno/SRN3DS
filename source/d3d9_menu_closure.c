/* First-main-menu shader closure certification.
 *
 * See d3d9_menu_closure.h for the contract.  This file is deliberately
 * self-contained: SHA-256 plus a bounded D3D9 token walker, no backend and no
 * D3D9 caps dependency, so the same rules can run on the host and on the 3DS.
 */
#include "d3d9_menu_closure.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define D9_MENU_MAX_DECLS 32u
#define D9_MENU_MAX_OPS 128u

enum {
    D3DSIO_DCL = 0x1F,
    D3DSIO_TEXLD = 0x42,
    D3DSIO_COMMENT = 0xFFFE,
    D3DSIO_PHASE = 0xFFFD,
    D3DSIO_END = 0xFFFF,
    D9_MENU_REG_INPUT = 1,
    D9_MENU_REG_OUTPUT = 6
};

/* --------------------------------------------------------------------- */
/* SHA-256                                                               */
/* --------------------------------------------------------------------- */

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static const uint32_t d9_sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

typedef struct {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    size_t fill;
} D9Sha256;

static void sha_init(D9Sha256 *s) {
    static const uint32_t init[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(s->h, init, sizeof init);
    s->length = 0;
    s->fill = 0;
}

static void sha_block(D9Sha256 *s, const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], hh = s->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + s1 + ch + d9_sha_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += hh;
}

static void sha_update(D9Sha256 *s, const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    s->length += (uint64_t)size;
    while (size) {
        size_t take = 64 - s->fill;
        if (take > size) take = size;
        memcpy(s->block + s->fill, p, take);
        s->fill += take;
        p += take;
        size -= take;
        if (s->fill == 64) {
            sha_block(s, s->block);
            s->fill = 0;
        }
    }
}

static void sha_final(D9Sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->length * 8u;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->fill != 56) sha_update(s, &zero, 1);
    uint8_t length_bytes[8];
    for (unsigned i = 0; i < 8; ++i)
        length_bytes[i] = (uint8_t)(bits >> (56u - i * 8u));
    sha_update(s, length_bytes, 8);
    for (unsigned i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void d9_menu_closure_sha256(const void *data, size_t size,
                            uint8_t out[D9_MENU_CLOSURE_SHA256_BYTES]) {
    D9Sha256 s;
    if (!out) return;
    sha_init(&s);
    if (data && size) sha_update(&s, data, size);
    sha_final(&s, out);
}

/* --------------------------------------------------------------------- */
/* Bounded D3D9 token walk                                               */
/* --------------------------------------------------------------------- */

typedef struct {
    uint32_t version;
    uint32_t used_words;
    uint32_t instructions;
    uint32_t texld;
    uint32_t branches;
    uint32_t decl_count;
    uint32_t opcode_count;
    uint32_t output_mask;
    uint32_t output_overflow;
    D9MenuClosureDecl decls[D9_MENU_MAX_DECLS];
    uint16_t opcodes[D9_MENU_MAX_OPS];
} D9MenuWalk;

static int is_branch_opcode(unsigned op) {
    switch (op) {
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
    case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A:
    case 0x2B: case 0x2C: case 0x2D: case 0x60:
        return 1;
    default:
        return 0;
    }
}

/* Returns 0 on a valid stream (reached END inside `words`), -1 otherwise.
 * On failure `why` is a bounded explanation. */
static int walk_program(const uint32_t *code, size_t words, D9MenuWalk *w,
                        char *why, size_t why_cap) {
    uint32_t i;
    int end = 0;
    memset(w, 0, sizeof *w);
    if (!code || words < 2) {
        snprintf(why, why_cap, "token stream is shorter than a version token plus END");
        return -1;
    }
    w->version = code[0];
    i = 1;
    while (i < words) {
        uint32_t token = code[i];
        unsigned op = (unsigned)(token & 0xFFFFu);
        if (op == D3DSIO_END) {
            ++i;
            end = 1;
            break;
        }
        if (op == D3DSIO_COMMENT) {
            size_t length = (size_t)((token >> 16) & 0x7FFFu);
            if (i + 1 + length > words) {
                snprintf(why, why_cap, "comment at word %u runs past the stream", (unsigned)i);
                return -1;
            }
            i += 1 + length;
            continue;
        }
        if (op == D3DSIO_PHASE) {
            ++i;
            continue;
        }
        if (op > 96) {
            snprintf(why, why_cap, "unsupported opcode 0x%02x at word %u", op, (unsigned)i);
            return -1;
        }
        unsigned extra = (token >> 24) & 0xFu;
        if (i + 1 + extra > words) {
            snprintf(why, why_cap, "instruction at word %u runs past the stream", (unsigned)i);
            return -1;
        }
        ++w->instructions;
        if (w->opcode_count < D9_MENU_MAX_OPS)
            w->opcodes[w->opcode_count] = (uint16_t)op;
        ++w->opcode_count;
        if (op == D3DSIO_TEXLD) ++w->texld;
        if (is_branch_opcode(op)) ++w->branches;
        if (op == D3DSIO_DCL && extra >= 2) {
            uint32_t usage_token = code[i + 1];
            uint32_t dst = code[i + 2];
            if (w->decl_count < D9_MENU_MAX_DECLS) {
                D9MenuClosureDecl *d = &w->decls[w->decl_count];
                d->usage = usage_token & 0x1Fu;
                d->usage_index = (usage_token >> 16) & 0xFu;
                d->texture_type = (int32_t)((usage_token >> 27) & 0xFu);
                d->reg_type = (dst >> 28) & 0x7u;
                d->reg_index = dst & 0x7FFu;
            }
            ++w->decl_count;
        } else if (extra >= 1) {
            uint32_t dst = code[i + 1];
            unsigned reg_type = (dst >> 28) & 0x7u;
            unsigned reg_index = dst & 0x7FFu;
            if (reg_type == D9_MENU_REG_OUTPUT) {
                if (reg_index < 32) w->output_mask |= 1u << reg_index;
                else ++w->output_overflow;
            }
        }
        i += 1 + extra;
    }
    if (!end) {
        snprintf(why, why_cap, "no END token inside %u words", (unsigned)words);
        return -1;
    }
    w->used_words = i;
    return 0;
}

static void format_decl(char *buf, size_t cap, const D9MenuClosureDecl *d) {
    static const char *usage_names[] = {
        "position", "blendweight", "blendindices", "normal", "psize",
        "texcoord", "tangent", "binormal", "tessfactor", "positiont",
        "color", "fog", "depth", "sample"
    };
    const char *usage = d->usage < 14 ? usage_names[d->usage] : "usage?";
    const char *prefix = d->reg_type == D9_MENU_REG_INPUT ? "v" :
                         d->reg_type == D9_MENU_REG_OUTPUT ? "o" :
                         d->reg_type == 2 ? "s" : d->reg_type == 3 ? "r" :
                         d->reg_type == 0 ? "r" : "?";
    if (d->reg_type == 255)
        snprintf(buf, cap, "%s%u", usage, d->usage_index);
    else
        snprintf(buf, cap, "%s%u@%s%u%s", usage, d->usage_index, prefix,
                 d->reg_index, d->texture_type >= 0 ? "/tex" : "");
}

static int decl_matches(const D9MenuClosureDecl *want, const D9MenuWalk *w) {
    for (uint32_t i = 0; i < w->decl_count && i < D9_MENU_MAX_DECLS; ++i) {
        const D9MenuClosureDecl *have = &w->decls[i];
        if (have->usage != want->usage) continue;
        if (have->usage_index != want->usage_index) continue;
        if (want->reg_type != 255u &&
            (have->reg_type != want->reg_type || have->reg_index != want->reg_index))
            continue;
        if (want->texture_type >= 0 &&
            (int32_t)have->texture_type != want->texture_type)
            continue;
        return 1;
    }
    return 0;
}

static void verdict_reason(D9MenuClosureVerdict *v, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->reason, sizeof v->reason, fmt, ap);
    va_end(ap);
}

static void fill_verdict(D9MenuClosureVerdict *v, uint32_t match, int entry,
                         const D9MenuWalk *w) {
    memset(v, 0, sizeof *v);
    v->match = match;
    v->entry = entry;
    if (w) {
        v->version = w->version;
        v->used_words = w->used_words;
        v->instructions = w->instructions;
    }
}

static int structure_against(const D9MenuClosureEntry *e, const D9MenuWalk *w,
                             D9MenuClosureVerdict *v) {
    if (w->version != e->version) {
        verdict_reason(v, "%s: version 0x%08x != 0x%08x", e->id,
                       (unsigned)w->version, (unsigned)e->version);
        return D9_MENU_STRUCTURE_MISMATCH;
    }
    if (e->chunk_bytes && w->used_words * 4u != e->chunk_bytes) {
        verdict_reason(v, "%s: length %u bytes != %u", e->id,
                       (unsigned)(w->used_words * 4u), (unsigned)e->chunk_bytes);
        return D9_MENU_STRUCTURE_MISMATCH;
    }
    if (e->instruction_count && w->instructions != e->instruction_count) {
        verdict_reason(v, "%s: %u instructions != %u", e->id,
                       (unsigned)w->instructions, (unsigned)e->instruction_count);
        return D9_MENU_STRUCTURE_MISMATCH;
    }
    if (e->texld_count >= 0 && w->texld != (uint32_t)e->texld_count) {
        verdict_reason(v, "%s: %u texld != %d", e->id, (unsigned)w->texld,
                       (int)e->texld_count);
        return D9_MENU_STRUCTURE_MISMATCH;
    }
    if (e->branch_count >= 0 && w->branches != (uint32_t)e->branch_count) {
        verdict_reason(v, "%s: %u branches != %d", e->id, (unsigned)w->branches,
                       (int)e->branch_count);
        return D9_MENU_STRUCTURE_MISMATCH;
    }
    if (e->opcode_count) {
        if (w->opcode_count != e->opcode_count) {
            verdict_reason(v, "%s: %u opcodes != %u", e->id,
                           (unsigned)w->opcode_count, (unsigned)e->opcode_count);
            return D9_MENU_STRUCTURE_MISMATCH;
        }
        for (uint32_t i = 0; i < e->opcode_count; ++i) {
            if (w->opcodes[i] != e->opcodes[i]) {
                verdict_reason(v, "%s: opcode %u is 0x%02x, expected 0x%02x", e->id,
                               (unsigned)i, (unsigned)w->opcodes[i],
                               (unsigned)e->opcodes[i]);
                return D9_MENU_STRUCTURE_MISMATCH;
            }
        }
    }
    for (uint32_t i = 0; i < e->declaration_count; ++i) {
        if (!decl_matches(&e->declarations[i], w)) {
            char want[64];
            format_decl(want, sizeof want, &e->declarations[i]);
            verdict_reason(v, "%s: missing declaration %s", e->id, want);
            return D9_MENU_STRUCTURE_MISMATCH;
        }
    }
    if (e->required_output_mask) {
        if (w->output_overflow ||
            (w->output_mask & e->required_output_mask) != e->required_output_mask) {
            verdict_reason(v, "%s: outputs 0x%08x do not cover 0x%08x", e->id,
                           (unsigned)w->output_mask,
                           (unsigned)e->required_output_mask);
            return D9_MENU_STRUCTURE_MISMATCH;
        }
    }
    return D9_MENU_MATCH;
}

int d9_menu_closure_check_structure(int entry, const uint32_t *code,
                                    size_t available_words,
                                    D9MenuClosureVerdict *out) {
    D9MenuClosureVerdict local;
    D9MenuWalk walk;
    char why[96];
    int result;
    if (!out) out = &local;
    fill_verdict(out, D9_MENU_BAD_ARGUMENT, D9_MENU_ENTRY_NONE, 0);
    if (entry < 0 || entry >= D9_MENU_ENTRY_COUNT) {
        verdict_reason(out, "entry %d is not in the closure", entry);
        return D9_MENU_BAD_ARGUMENT;
    }
    const D9MenuClosureEntry *e = &d9_menu_closure_entries[entry];
    if (e->stage == D9_MENU_STAGE_FIXED) {
        verdict_reason(out, "%s is a fixed-pipeline entry, not bytecode", e->id);
        return D9_MENU_BAD_ARGUMENT;
    }
    if (walk_program(code, available_words, &walk, why, sizeof why)) {
        fill_verdict(out, D9_MENU_NOT_MEMBER, D9_MENU_ENTRY_NONE, 0);
        verdict_reason(out, "%s: %s", e->id, why);
        return D9_MENU_NOT_MEMBER;
    }
    fill_verdict(out, D9_MENU_MATCH, entry, &walk);
    out->stage = e->stage;
    out->key = e->key;
    if (e->sha256)
        memcpy(out->sha256, e->sha256, D9_MENU_CLOSURE_SHA256_BYTES);
    result = structure_against(e, &walk, out);
    out->match = (uint32_t)result;
    return result;
}

int d9_menu_closure_certify(const D9MenuClosureRequest *request,
                            D9MenuClosureVerdict *out) {
    D9MenuClosureVerdict local;
    D9MenuWalk walk;
    char why[96];
    int by_key = -1;
    int by_hash = -1;
    int result;
    if (!out) out = &local;
    fill_verdict(out, D9_MENU_BAD_ARGUMENT, D9_MENU_ENTRY_NONE, 0);
    if (!request || !request->code ||
        (request->stage != D9_MENU_STAGE_PS && request->stage != D9_MENU_STAGE_VS) ||
        request->available_words < 2) {
        verdict_reason(out, "request needs a ps/vs stage and a readable token stream");
        return D9_MENU_BAD_ARGUMENT;
    }
    if (walk_program(request->code, request->available_words, &walk, why, sizeof why)) {
        fill_verdict(out, D9_MENU_NOT_MEMBER, D9_MENU_ENTRY_NONE, 0);
        verdict_reason(out, "token stream is not a shader program: %s", why);
        return D9_MENU_NOT_MEMBER;
    }
    fill_verdict(out, D9_MENU_MATCH, D9_MENU_ENTRY_NONE, &walk);
    out->stage = request->stage;
    out->key = request->key;
    d9_menu_closure_sha256(request->code, (size_t)walk.used_words * 4u, out->sha256);

    if ((walk.version >> 16) != (request->stage == D9_MENU_STAGE_PS ? 0xFFFFu : 0xFFFEu)) {
        out->match = D9_MENU_NOT_MEMBER;
        verdict_reason(out, "version 0x%08x is not a %s program",
                       (unsigned)walk.version,
                       request->stage == D9_MENU_STAGE_PS ? "ps" : "vs");
        return D9_MENU_NOT_MEMBER;
    }

    /* Key-directed lookup first: this is where a wrong key is named even when
     * the bytes are a valid closure member. */
    if (request->key_known) {
        for (int i = 0; i < D9_MENU_ENTRY_COUNT; ++i) {
            const D9MenuClosureEntry *e = &d9_menu_closure_entries[i];
            if (e->stage != request->stage || !e->key_known) continue;
            if (e->key == request->key) { by_key = i; break; }
        }
        if (by_key < 0) {
            /* The VS side is a bounded family with no fixed key, so an engine
             * VS key cannot be checked against a table.  PS entries do have a
             * single key and a wrong one is named. */
            if (request->stage != D9_MENU_STAGE_VS) {
                out->match = D9_MENU_KEY_MISMATCH;
                verdict_reason(out, "key 0x%08x is not a certified %s entry",
                               (unsigned)request->key, "ps");
                return D9_MENU_KEY_MISMATCH;
            }
        } else {
            const D9MenuClosureEntry *e = &d9_menu_closure_entries[by_key];
            if (e->sha256 &&
                memcmp(out->sha256, e->sha256, D9_MENU_CLOSURE_SHA256_BYTES) != 0) {
                out->match = D9_MENU_HASH_MISMATCH;
                verdict_reason(out, "%s: SHA-256 is not the certified program", e->id);
                return D9_MENU_HASH_MISMATCH;
            }
            result = structure_against(e, &walk, out);
            out->match = (uint32_t)result;
            if (result == D9_MENU_MATCH) {
                out->entry = by_key;
                out->key = e->key;
            }
            return result;
        }
    }

    for (int i = 0; i < D9_MENU_ENTRY_COUNT; ++i) {
        const D9MenuClosureEntry *e = &d9_menu_closure_entries[i];
        if (e->stage != request->stage || !e->sha256) continue;
        if (memcmp(out->sha256, e->sha256, D9_MENU_CLOSURE_SHA256_BYTES) == 0) {
            by_hash = i;
            break;
        }
    }
    if (by_hash >= 0) {
        const D9MenuClosureEntry *e = &d9_menu_closure_entries[by_hash];
        result = structure_against(e, &walk, out);
        out->match = (uint32_t)result;
        if (result == D9_MENU_MATCH) {
            out->entry = by_hash;
            out->key = e->key;
        }
        return result;
    }

    /* The vertex family has no single hash.  Declarations and outputs are the
     * contract; the exact runtime key is chosen by the engine's vertex
     * declaration path. */
    if (request->stage == D9_MENU_STAGE_VS) {
        char family_reason[D9_MENU_CLOSURE_REASON_MAX];
        int have_family_reason = 0;
        family_reason[0] = 0;
        for (int i = 0; i < D9_MENU_ENTRY_COUNT; ++i) {
            const D9MenuClosureEntry *e = &d9_menu_closure_entries[i];
            if (e->stage != D9_MENU_STAGE_VS || e->sha256) continue;
            result = structure_against(e, &walk, out);
            if (result == D9_MENU_MATCH) {
                out->match = D9_MENU_MATCH;
                out->entry = i;
                return D9_MENU_MATCH;
            }
            if (!have_family_reason && out->reason[0]) {
                snprintf(family_reason, sizeof family_reason, "%s", out->reason);
                have_family_reason = 1;
            }
        }
        out->match = D9_MENU_NOT_MEMBER;
        verdict_reason(out, "%s", have_family_reason ? family_reason :
                       "vs token stream is not in the closed menu set");
        return D9_MENU_NOT_MEMBER;
    }

    out->match = D9_MENU_NOT_MEMBER;
    verdict_reason(out, "%s token stream is not in the closed menu set",
                   request->stage == D9_MENU_STAGE_PS ? "ps" : "vs");
    return D9_MENU_NOT_MEMBER;
}

/* --------------------------------------------------------------------- */
/* Runtime state                                                         */
/* --------------------------------------------------------------------- */

typedef struct {
    int armed;
    int invalidated;
    uint32_t seen[D9_MENU_ENTRY_COUNT];
    uint32_t attempted[D9_MENU_ENTRY_COUNT];
    char reason[D9_MENU_CLOSURE_REASON_MAX];
} D9MenuClosureStateData;

static D9MenuClosureStateData g_closure;

static int entry_required(int entry) {
    return entry >= 0 && entry < D9_MENU_ENTRY_COUNT &&
           d9_menu_closure_entries[entry].required;
}

static int coverage_complete(void) {
    for (int i = 0; i < D9_MENU_ENTRY_COUNT; ++i) {
        if (entry_required(i) && !g_closure.seen[i]) return 0;
    }
    return 1;
}

static int shader_set_coverage_complete(void) {
    for (int i = 0; i < D9_MENU_ENTRY_COUNT; ++i) {
        const D9MenuClosureEntry *e = &d9_menu_closure_entries[i];
        if (e->required && e->runtime_gated && !g_closure.seen[i]) return 0;
    }
    return 1;
}

static void invalidate(const char *fmt, ...) {
    va_list ap;
    if (g_closure.invalidated) return;
    g_closure.invalidated = 1;
    va_start(ap, fmt);
    vsnprintf(g_closure.reason, sizeof g_closure.reason, fmt, ap);
    va_end(ap);
}

void d9_menu_closure_reset(void) {
    memset(&g_closure, 0, sizeof g_closure);
}

void d9_menu_closure_arm(void) {
    if (g_closure.invalidated) return;
    if (g_closure.armed) return;
    memset(g_closure.seen, 0, sizeof g_closure.seen);
    memset(g_closure.attempted, 0, sizeof g_closure.attempted);
    g_closure.armed = 1;
}

void d9_menu_closure_disarm(void) {
    g_closure.armed = 0;
}

int d9_menu_closure_armed(void) {
    return g_closure.armed;
}

int d9_menu_closure_precreate(uint32_t stage, const uint32_t *code,
                              size_t available_words,
                              D9MenuClosureVerdict *verdict) {
    D9MenuClosureRequest request;
    D9MenuClosureVerdict local;
    int result;
    if (!verdict) verdict = &local;
    memset(&request, 0, sizeof request);
    request.stage = stage;
    request.code = code;
    request.available_words = available_words;
    result = d9_menu_closure_certify(&request, verdict);
    if (!g_closure.armed) return 0;
    if (result == D9_MENU_MATCH && verdict->entry >= 0 &&
        verdict->entry < D9_MENU_ENTRY_COUNT) {
        g_closure.attempted[verdict->entry] = 1;
        return 0;
    }
    invalidate("%s", verdict->reason[0] ? verdict->reason :
                                       "shader outside the certified closure");
    return -1;
}

void d9_menu_closure_commit(const D9MenuClosureVerdict *verdict) {
    if (!verdict || verdict->match != D9_MENU_MATCH) return;
    if (verdict->entry < 0 || verdict->entry >= D9_MENU_ENTRY_COUNT) return;
    g_closure.seen[verdict->entry] = 1;
}

void d9_menu_closure_commit_failed(const D9MenuClosureVerdict *verdict,
                                   const char *why) {
    if (!verdict || verdict->entry < 0 || verdict->entry >= D9_MENU_ENTRY_COUNT)
        return;
    if (!g_closure.armed) return;
    invalidate("%s creation failed: %s", d9_menu_closure_entries[verdict->entry].id,
               why && why[0] ? why : "backend rejected the shader");
}

void d9_menu_closure_note_fixed(int entry) {
    if (entry < 0 || entry >= D9_MENU_ENTRY_COUNT) return;
    if (d9_menu_closure_entries[entry].stage != D9_MENU_STAGE_FIXED) return;
    g_closure.seen[entry] = 1;
}

D9MenuClosureState d9_menu_closure_state(void) {
    if (g_closure.invalidated) return D9_MENU_STATE_INVALIDATED;
    if (!g_closure.armed) return D9_MENU_STATE_UNARMED;
    return coverage_complete() ? D9_MENU_STATE_READY : D9_MENU_STATE_ARMED;
}

const char *d9_menu_closure_state_name(D9MenuClosureState state) {
    switch (state) {
    case D9_MENU_STATE_UNARMED: return "unarmed";
    case D9_MENU_STATE_ARMED: return "armed";
    case D9_MENU_STATE_READY: return "ready";
    case D9_MENU_STATE_INVALIDATED: return "invalidated";
    default: return "unknown";
    }
}

int d9_menu_closure_ready(void) {
    return d9_menu_closure_state() == D9_MENU_STATE_READY;
}

int d9_menu_closure_invalidated(void) {
    return g_closure.invalidated;
}

const char *d9_menu_closure_invalidation_reason(void) {
    return g_closure.reason;
}

D9MenuClosurePredicate d9_menu_closure_predicate(void) {
    switch (d9_menu_closure_state()) {
    case D9_MENU_STATE_READY: return D9_MENU_PREDICATE_CERTIFIED;
    case D9_MENU_STATE_INVALIDATED: return D9_MENU_PREDICATE_INVALIDATED;
    default: return D9_MENU_PREDICATE_NOT_CERTIFIED;
    }
}

int d9_menu_closure_shader_set_ready(void) {
    return g_closure.armed && !g_closure.invalidated &&
           shader_set_coverage_complete();
}

D9MenuClosurePredicate d9_menu_closure_shader_set_predicate(void) {
    if (g_closure.invalidated) return D9_MENU_PREDICATE_INVALIDATED;
    return d9_menu_closure_shader_set_ready() ? D9_MENU_PREDICATE_CERTIFIED
                                              : D9_MENU_PREDICATE_NOT_CERTIFIED;
}

int d9_menu_closure_entry_count(void) {
    return D9_MENU_ENTRY_COUNT;
}

const char *d9_menu_closure_entry_id(int entry) {
    if (entry < 0 || entry >= D9_MENU_ENTRY_COUNT) return 0;
    return d9_menu_closure_entries[entry].id;
}

int d9_menu_closure_entry_status(int entry, uint32_t *seen,
                                 uint32_t *attempted) {
    if (entry < 0 || entry >= D9_MENU_ENTRY_COUNT) return -1;
    if (seen) *seen = g_closure.seen[entry];
    if (attempted) *attempted = g_closure.attempted[entry];
    return 0;
}

void d9_menu_closure_caps_claim(D9MenuClosureCapsClaim *out) {
    if (!out) return;
    out->vertex_shader_version = 0xFFFE0200u;
    out->pixel_shader_version = 0xFFFF0200u;
    out->sm3_instruction_slots = 0;
}

size_t d9_menu_closure_describe(char *buf, size_t cap) {
    unsigned shader_seen = 0, shader_required = 0;
    unsigned fixed_seen = 0, fixed_required = 0;
    for (int i = 0; i < D9_MENU_ENTRY_COUNT; ++i) {
        const D9MenuClosureEntry *e = &d9_menu_closure_entries[i];
        if (!e->required) continue;
        if (e->runtime_gated) {
            ++shader_required;
            if (g_closure.seen[i]) ++shader_seen;
        } else {
            ++fixed_required;
            if (g_closure.seen[i]) ++fixed_seen;
        }
    }
    D9MenuClosureState state = d9_menu_closure_state();
    int n = snprintf(buf, cap,
                     "menu_shader_closure=%s shaders=%u/%u fixed=%u/%u "
                     "caps=vs2.0/ps2.0 sm3_slots=0%s%s",
                     d9_menu_closure_state_name(state), shader_seen, shader_required,
                     fixed_seen, fixed_required,
                     g_closure.invalidated ? " reason=" : "",
                     g_closure.invalidated ? g_closure.reason : "");
    return n > 0 ? (size_t)n : 0;
}
