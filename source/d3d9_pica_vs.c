#include "d3d9_pica_vs.h"
#ifdef __3DS__
#include "d3d9_pica_vs_device.h"
#endif

#include <stdio.h>
#include <string.h>

#include "d3d9_pica_vs_shbin.inc"
#include "d3d9_pica_vs_menu_shbin.inc"
#include "d3d9_pica_vs_ui_shbin.inc"
#include "d3d9_pica_vs_contract_shbin.inc"

/* Verified emitted programs (exact vs_3_0 SHA-256 keys). */
#include "d3d9_pica_vs_table.inc"
/* GFx menu VS: same emitter contract, no bone palette (mode == MENU). */
#include "d3d9_pica_vs_menu_table.inc"
/* BSA ui/interface VS: menu transform/texgen/tint, identity uniform bank. */
#include "d3d9_pica_vs_ui_table.inc"
/* Non-skinned lighting contracts: identity/compact uniform bank. */
#include "d3d9_pica_vs_contract_table.inc"

/* ------------------------------------------------------------------------- */
/* SHA-256 (FIPS 180-4), used to pin the embedded .shbin bytes. */

static const uint32_t d9_sha_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t d9_rotr(uint32_t v, unsigned n) {
    return (v >> n) | (v << (32u - n));
}

static void d9_sha_block(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
               (uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = d9_rotr(w[i - 15], 7) ^ d9_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = d9_rotr(w[i - 2], 17) ^ d9_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], k = h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = d9_rotr(e, 6) ^ d9_rotr(e, 11) ^ d9_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = k + s1 + ch + d9_sha_k[i] + w[i];
        uint32_t s0 = d9_rotr(a, 2) ^ d9_rotr(a, 13) ^ d9_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void d9_sha256(const uint8_t *data, size_t size, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                     0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    size_t full = size / 64;
    for (size_t i = 0; i < full; ++i) d9_sha_block(h, data + i * 64);
    uint8_t tail[128];
    size_t rest = size - full * 64;
    memset(tail, 0, sizeof tail);
    if (rest) memcpy(tail, data + full * 64, rest);
    tail[rest] = 0x80;
    size_t tail_len = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)size * 8u;
    for (unsigned i = 0; i < 8; ++i)
        tail[tail_len - 1 - i] = (uint8_t)(bits >> (i * 8));
    d9_sha_block(h, tail);
    if (tail_len == 128) d9_sha_block(h, tail + 64);
    for (unsigned i = 0; i < 8; ++i) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

void d9_pica_vs_sha256_hex(const uint8_t *data, size_t size, char hex[65]);
void d9_pica_vs_sha256_hex(const uint8_t *data, size_t size, char hex[65]) {
    uint8_t digest[32];
    d9_sha256(data, size, digest);
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
}

/* ------------------------------------------------------------------------- */

static char d9_lower(char c) {
    return (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
}

int d9_pica_vs_sha256_equal(const char *a, const char *b) {
    if (!a || !b || strlen(a) != 64 || strlen(b) != 64) return 0;
    for (unsigned i = 0; i < 64; ++i)
        if (d9_lower(a[i]) != d9_lower(b[i])) return 0;
    return 1;
}

unsigned d9_pica_vs_count(void) {
    return (unsigned)(D9_PICA_VS_ENTRY_COUNT + D9_PICA_VS_MENU_ENTRY_COUNT +
                      D9_PICA_VS_UI_ENTRY_COUNT +
                      D9_PICA_VS_CONTRACT_ENTRY_COUNT);
}

const D9PicaVsProgram *d9_pica_vs_at(unsigned index) {
    if (index < D9_PICA_VS_ENTRY_COUNT) return &d9_pica_vs_entries[index];
    index -= D9_PICA_VS_ENTRY_COUNT;
    if (index < D9_PICA_VS_MENU_ENTRY_COUNT)
        return &d9_pica_vs_menu_entries[index];
    index -= D9_PICA_VS_MENU_ENTRY_COUNT;
    if (index < D9_PICA_VS_UI_ENTRY_COUNT)
        return &d9_pica_vs_ui_entries[index];
    index -= D9_PICA_VS_UI_ENTRY_COUNT;
    if (index >= D9_PICA_VS_CONTRACT_ENTRY_COUNT) return NULL;
    return &d9_pica_vs_contract_entries[index];
}

const D9PicaVsProgram *d9_pica_vs_lookup(const char *sha256_hex) {
    if (!sha256_hex || strlen(sha256_hex) != 64) return NULL;
    for (unsigned i = 0; i < D9_PICA_VS_ENTRY_COUNT; ++i)
        if (d9_pica_vs_sha256_equal(d9_pica_vs_entries[i].sha256, sha256_hex))
            return &d9_pica_vs_entries[i];
    for (unsigned i = 0; i < D9_PICA_VS_MENU_ENTRY_COUNT; ++i)
        if (d9_pica_vs_sha256_equal(d9_pica_vs_menu_entries[i].sha256, sha256_hex))
            return &d9_pica_vs_menu_entries[i];
    for (unsigned i = 0; i < D9_PICA_VS_UI_ENTRY_COUNT; ++i)
        if (d9_pica_vs_sha256_equal(d9_pica_vs_ui_entries[i].sha256, sha256_hex))
            return &d9_pica_vs_ui_entries[i];
    for (unsigned i = 0; i < D9_PICA_VS_CONTRACT_ENTRY_COUNT; ++i)
        if (d9_pica_vs_sha256_equal(d9_pica_vs_contract_entries[i].sha256,
                                    sha256_hex))
            return &d9_pica_vs_contract_entries[i];
    return NULL;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

int d9_pica_vs_validate(const D9PicaVsProgram *program,
                        char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!program) {
        if (why && why_size) snprintf(why, why_size, "no PICA VS program");
        return -1;
    }
    const uint8_t *b = program->shbin;
    unsigned size = program->shbin_size;
    if (!b || size < 0x30u || memcmp(b, "DVLB", 4) != 0) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: .shbin is not a DVLB image", program->id);
        return -1;
    }
    if (le32(b + 4) != 1u) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: .shbin has %u DVLEs, expected 1",
                     program->id, (unsigned)le32(b + 4));
        return -1;
    }
    uint32_t dvle = le32(b + 8);
    if (size < 0x30u || dvle > size - 0x30u || dvle < 0x10u ||
        memcmp(b + dvle, "DVLE", 4) != 0) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: .shbin DVLE offset 0x%x is invalid",
                     program->id, (unsigned)dvle);
        return -1;
    }
    /* DVLE sub-tables are offsets relative to the DVLE start.  Check the
     * constant (12-byte), output (8-byte) and uniform (8-byte) tables before
     * letting DVLB_ParseFile or the GPU walk them. */
    {
        static const unsigned table_off[3] = {0x18u, 0x20u, 0x28u};
        static const unsigned table_size[3] = {12u, 8u, 8u};
        for (unsigned t = 0; t < 3; ++t) {
            uint32_t off = le32(b + dvle + table_off[t]);
            uint32_t count = le32(b + dvle + table_off[t] + 4u);
            uint32_t avail = size - dvle;
            if (off > avail || count > (avail - off) / table_size[t]) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: .shbin DVLE table %u is out of bounds",
                             program->id, t);
                return -1;
            }
        }
    }
    /* DVLP is inline at 0x0c in picasso output; its main word count lives at
     * DVLP+0x0c.  Both reads must stay inside the image. */
    if (size < 0x1cu || memcmp(b + 0x0c, "DVLP", 4) != 0) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: .shbin has no complete DVLP program block",
                     program->id);
        return -1;
    }
    /* picasso writes the main-function word count at DVLP+0x0c. */
    uint32_t words = le32(b + 0x18);
    if (words != program->instructions) {
        if (why && why_size)
            snprintf(why, why_size,
                     "plan %s: .shbin main is %u words, emitter proved %u",
                     program->id, (unsigned)words, program->instructions);
        return -1;
    }
    char hex[65];
    d9_pica_vs_sha256_hex(b, size, hex);
    if (!d9_pica_vs_sha256_equal(hex, program->shbin_sha256)) {
        if (why && why_size)
            snprintf(why, why_size,
                     "plan %s: .shbin SHA-256 %.12s... != pinned %.12s...",
                     program->id, hex, program->shbin_sha256);
        return -1;
    }
    if (program->bone_rows == 0) {
        /* GFx menu programs use the plain D3D c0..cN bank: no bone window
         * remap, no c180 WVP indirection. */
        if (program->uniform_rows == 0 ||
            program->uniform_rows > D9_PICA_VS_MAX_ROWS ||
            program->bone_rows != 0 || program->wvp_first_row != 0 ||
            program->wvp_rows != program->uniform_rows ||
            program->attr_count == 0 ||
            program->attr_count > D9_PICA_VS_MAX_ATTRS) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: menu uniform/attribute contract is inconsistent",
                         program->id);
            return -1;
        }
    } else if (program->bone_rows != D9_PICA_VS_BONE_ROWS ||
               program->uniform_rows < D9_PICA_VS_BONE_ROWS + 4u ||
               program->uniform_rows > D9_PICA_VS_MAX_ROWS ||
               program->bone_rows != D9_PICA_VS_BONE_ROWS ||
               program->wvp_first_row != D9_PICA_VS_BONE_ROWS ||
               program->wvp_rows != 4u || program->attr_count == 0 ||
               program->attr_count > D9_PICA_VS_MAX_ATTRS) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: uniform/attribute contract is inconsistent",
                     program->id);
        return -1;
    }
    uint32_t seen = 0;
    for (unsigned a = 0; a < program->attr_count; ++a) {
        const D9PicaVsAttr *attr = &program->attrs[a];
        if (attr->reg >= 16u || !attr->components || attr->components > 4u ||
            attr->decl_type == 0 || attr->format > D9_PICA_VS_FMT_UBYTE_NORM ||
            attr->attr > D9_PICA_VS_ATTR_COLOR || (seen & (1u << attr->reg))) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: attribute %u has an invalid register/format",
                         program->id, a);
            return -1;
        }
        seen |= 1u << attr->reg;
    }
    return 0;
}

static int decl_type_components(unsigned type) {
    switch (type) {
    case D9_DECL_FLOAT1: return 1;
    case D9_DECL_FLOAT2: return 2;
    case D9_DECL_FLOAT3: return 3;
    case D9_DECL_FLOAT4: return 4;
    case D9_DECL_COLOR:
    case D9_DECL_UBYTE4:
    case D9_DECL_UBYTE4N: return 4;
    case D9_DECL_SHORT2:
    case D9_DECL_SHORT2N:
    case D9_DECL_USHORT2N: return 2;
    case D9_DECL_SHORT4:
    case D9_DECL_SHORT4N:
    case D9_DECL_USHORT4N: return 4;
    case D9_DECL_UDEC3: return 3;
    case D9_DECL_DEC3N: return 4;
    case D9_DECL_FLOAT16_2: return 2;
    case D9_DECL_FLOAT16_4: return 4;
    default: return 0;
    }
}

enum { D9_USAGE_BLENDWEIGHT_ = 1, D9_USAGE_BLENDINDICES_ = 2 };

static unsigned attr_usage(const D9PicaVsAttr *attr) {
    switch (attr->attr) {
    case D9_PICA_VS_ATTR_POSITION: return D9_USAGE_POSITION;
    case D9_PICA_VS_ATTR_BLENDWEIGHT: return D9_USAGE_BLENDWEIGHT_;
    case D9_PICA_VS_ATTR_BLENDINDICES: return D9_USAGE_BLENDINDICES_;
    case D9_PICA_VS_ATTR_TEXCOORD: return D9_USAGE_TEXCOORD;
    case D9_PICA_VS_ATTR_COLOR: return D9_USAGE_COLOR;
    default: return 0xffffu;
    }
}

static const char *attr_name(const D9PicaVsAttr *attr) {
    switch (attr->attr) {
    case D9_PICA_VS_ATTR_POSITION: return "position";
    case D9_PICA_VS_ATTR_BLENDWEIGHT: return "blendweight";
    case D9_PICA_VS_ATTR_BLENDINDICES: return "blendindices";
    case D9_PICA_VS_ATTR_TEXCOORD: return "texcoord";
    case D9_PICA_VS_ATTR_COLOR: return "color";
    default: return "attribute";
    }
}

int d9_pica_vs_declare(const D9PicaVsProgram *program, const D9Decl *decl,
                       D9PicaVsDecl *out, char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (out) memset(out, 0, sizeof *out);
    if (!program || !decl || !out) {
        if (why && why_size) snprintf(why, why_size, "invalid argument");
        return -1;
    }
    if (decl->count == 0) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: no active vertex declaration", program->id);
        return -1;
    }
    /* One stream only: the PICA loader set is bound to one buffer. */
    unsigned stream = UINT32_MAX;
    for (unsigned i = 0; i < decl->count; ++i) {
        const D9VertexElement *e = &decl->elements[i];
        if (e->stream == 0xff) continue;
        if (stream == UINT32_MAX) stream = e->stream;
        else if (stream != e->stream) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: declaration spans streams %u and %u; the PICA VS contract is one stream",
                         program->id, stream, e->stream);
            return -1;
        }
    }
    for (unsigned a = 0; a < program->attr_count; ++a) {
        const D9PicaVsAttr *attr = &program->attrs[a];
        const D9VertexElement *found = NULL;
        for (unsigned i = 0; i < decl->count; ++i) {
            const D9VertexElement *e = &decl->elements[i];
            if (e->stream == 0xff) continue;
            if (e->usage != attr_usage(attr) || e->usage_index != 0) continue;
            if (found) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: duplicate usage %u in the declaration",
                             program->id, e->usage);
                return -1;
            }
            found = e;
        }
        if (!found) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: declaration has no %s element (required as v%u)",
                         program->id, attr_name(attr), attr->reg);
            return -1;
        }
        if (found->type != attr->decl_type) {
            /* COLOR0 is D3DDECLTYPE_D3DCOLOR in retail declarations; it is
             * the same normalised 4-byte colour as UBYTE4N for this path. */
            int color_alias = attr->attr == D9_PICA_VS_ATTR_COLOR &&
                              attr->decl_type == D9_DECL_UBYTE4N &&
                              found->type == D9_DECL_COLOR;
            /* The GFx menu declarations are not the skin contract: position
             * and texcoord may be declared FLOAT1..4 as long as they carry
             * the components the emitted program reads.  Integers other than
             * a colour stay a named refusal (PICA loaders do not normalise). */
            int menu_float = program->bone_rows == 0 &&
                             (attr->attr == D9_PICA_VS_ATTR_POSITION ||
                              attr->attr == D9_PICA_VS_ATTR_TEXCOORD) &&
                             decl_type_components(found->type) >= attr->components;
            if (!color_alias && !menu_float) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: %s element type %u != required %u",
                             program->id, attr_name(attr), found->type,
                             attr->decl_type);
                return -1;
            }
        }
        if (found->offset & 3u) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: %s offset %u is not 4-byte aligned",
                         program->id, attr_name(attr), found->offset);
            return -1;
        }
        if (decl_type_components(found->type) < attr->components) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: %s supplies %u component(s), shader loads %u",
                         program->id, attr_name(attr),
                         decl_type_components(found->type), attr->components);
            return -1;
        }
        D9PicaVsLoader *loader = &out->loader[out->count++];
        loader->attr = attr->attr;
        loader->stream = found->stream;
        loader->offset = found->offset;
        loader->components = attr->components;
        loader->format = attr->format;
        loader->reg = attr->reg;
        out->mask |= 1u << attr->reg;
    }
    /* No silent drops: every declared usage must belong to the emitted
     * program.  A declaration element the shader never reads is a contract
     * mismatch, not something to ignore.  The GFx menu family is the
     * exception: Scaleform declarations are shared between programs and may
     * carry elements a particular program does not read; only the emitted
     * inputs are loaded. */
    if (program->bone_rows == 0) return 0;
    for (unsigned i = 0; i < decl->count; ++i) {
        const D9VertexElement *e = &decl->elements[i];
        if (e->stream == 0xff) continue;
        int known = 0;
        for (unsigned a = 0; a < program->attr_count; ++a)
            if (e->usage == attr_usage(&program->attrs[a]) && e->usage_index == 0)
                known = 1;
        if (!known) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: declaration usage %u is not an input of the emitted shader",
                         program->id, e->usage);
            return -1;
        }
    }
    return 0;
}

int d9_pica_vs_bank(const D9PicaVsProgram *program, D9PicaVsBank *out,
                    char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (out) memset(out, 0, sizeof *out);
    if (!program || !out) {
        if (why && why_size) snprintf(why, why_size, "invalid argument");
        return -1;
    }
    if (program->bone_rows == 0) {
        if (program->uniform_rows == 0 ||
            program->uniform_rows > D9_PICA_VS_MAX_ROWS) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: %u uniform rows outside the menu bank",
                         program->id, program->uniform_rows);
            return -1;
        }
        out->rows = program->uniform_rows;
        for (unsigned row = 0; row < out->rows; ++row)
            out->d3d_reg[row] = row;
        return 0;
    }
    if (program->uniform_rows < D9_PICA_VS_BONE_ROWS + 4u ||
        program->uniform_rows > D9_PICA_VS_MAX_ROWS) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %u uniform rows outside the 4-bone window",
                     program->id, program->uniform_rows);
        return -1;
    }
    out->rows = program->uniform_rows;
    for (unsigned row = 0; row < out->rows; ++row) {
        if (row < D9_PICA_VS_BONE_ROWS) out->d3d_reg[row] = row;
        else if (row < D9_PICA_VS_BONE_ROWS + 4u)
            out->d3d_reg[row] = 180u + (row - D9_PICA_VS_BONE_ROWS);
        else out->d3d_reg[row] = 184u + (row - (D9_PICA_VS_BONE_ROWS + 4u));
    }
    return 0;
}

int d9_pica_vs_bank_ready(const D9PicaVsBank *bank, const uint32_t *mask,
                          unsigned words, char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!bank || !bank->rows || bank->rows > D9_PICA_VS_MAX_ROWS) {
        if (why && why_size) snprintf(why, why_size, "no uniform bank");
        return -1;
    }
    if (!mask) {
        if (why && why_size) snprintf(why, why_size, "no constant mask");
        return -1;
    }
    for (unsigned row = 0; row < bank->rows; ++row) {
        unsigned reg = bank->d3d_reg[row];
        unsigned word = reg >> 5;
        int set = word < words && ((mask[word] >> (reg & 31u)) & 1u);
        if (!set) {
            if (why && why_size)
                snprintf(why, why_size,
                         "PICA VS uniform row c%u needs D3D c%u, which is not set",
                         row, reg);
            return -1;
        }
    }
    return 0;
}

int d9_pica_vs_gate(const D9PicaVsProgram *program, const D9Decl *decl,
                    int backend_available, const uint32_t *mask, unsigned words,
                    D9PicaVsDecl *out_decl, D9PicaVsBank *out_bank,
                    char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (out_decl) memset(out_decl, 0, sizeof *out_decl);
    if (out_bank) memset(out_bank, 0, sizeof *out_bank);
    if (!program) {
        if (why && why_size)
            snprintf(why, why_size,
                     "no verified PICA VS program (exact vs_3_0 SHA-256 miss)");
        return -1;
    }
    char step[192];
    if (d9_pica_vs_validate(program, step, sizeof step)) {
        if (why && why_size) snprintf(why, why_size, "%s", step);
        return -1;
    }
    if (!decl || decl->count == 0) {
        if (why && why_size)
            snprintf(why, why_size,
                     "PICA VS %s: draw has no active vertex declaration",
                     program->id);
        return -1;
    }
    if (!out_decl || !out_bank) {
        if (why && why_size) snprintf(why, why_size, "invalid gate output");
        return -1;
    }
    if (d9_pica_vs_declare(program, decl, out_decl, step, sizeof step)) {
        if (why && why_size) snprintf(why, why_size, "%s", step);
        return -1;
    }
    if (d9_pica_vs_bank(program, out_bank, step, sizeof step)) {
        if (why && why_size) snprintf(why, why_size, "%s", step);
        return -1;
    }
    if (d9_pica_vs_bank_ready(out_bank, mask, words, step, sizeof step)) {
        if (why && why_size) snprintf(why, why_size, "%s", step);
        return -1;
    }
    if (!backend_available) {
        if (why && why_size)
            snprintf(why, why_size,
                     "PICA VS %s: backend has no draw_pica_vs callback; "
                     "the skinned stream cannot be submitted", program->id);
        return -1;
    }
    return 0;
}

/* --- packed skinned vertex ------------------------------------------------ */

static unsigned attr_component_size(uint8_t format, unsigned components) {
    /* Both loader formats are submitted as GPU_FLOAT: PICA vertex loaders do
     * not normalise integer attributes, and the emitted shader consumes the
     * D3D UBYTE4N contract (indices in [0,1], colour in [0,1]). */
    if (format == D9_PICA_VS_FMT_FLOAT || format == D9_PICA_VS_FMT_UBYTE_NORM)
        return components * 4u;
    return 0;
}

unsigned d9_pica_vs_vertex_stride(const D9PicaVsDecl *decl) {
    unsigned stride = 0;
    if (!decl) return 0;
    for (unsigned i = 0; i < decl->count; ++i) {
        unsigned size = attr_component_size(decl->loader[i].format,
                                            decl->loader[i].components);
        if (!size) return 0;
        stride += size;
    }
    return stride;
}

uint64_t d9_pica_vs_bufinfo_permutation(const D9PicaVsDecl *decl) {
    uint64_t permutation = 0;
    if (!decl || decl->count > 8u) return 0;
    for (unsigned i = 0; i < decl->count; ++i)
        permutation |= (uint64_t)i << (i * 4u);
    return permutation;
}

static const float *attr_source(const D9PicaVsLoader *loader,
                                const D9SkinVertex *v) {
    switch (loader->attr) {
    case D9_PICA_VS_ATTR_POSITION: return &v->x;
    case D9_PICA_VS_ATTR_BLENDWEIGHT: return v->weight;
    case D9_PICA_VS_ATTR_TEXCOORD: return &v->u;
    case D9_PICA_VS_ATTR_COLOR: return &v->r;
    default: return NULL;
    }
}

int d9_pica_vs_pack_vertex(const D9PicaVsDecl *decl, const D9SkinVertex *v,
                           uint8_t *dst) {
    if (!decl || !v || !dst) return -1;
    unsigned offset = 0;
    for (unsigned i = 0; i < decl->count; ++i) {
        const D9PicaVsLoader *loader = &decl->loader[i];
        unsigned n = loader->components;
        if (!n || n > 4u) return -1;
        if (loader->format == D9_PICA_VS_FMT_UBYTE_NORM) {
            /* D3D UBYTE4N feeds the shader a normalised float.  The PICA
             * loader has no normalised integer variant, so decode here and
             * submit GPU_FLOAT components. */
            if (loader->attr == D9_PICA_VS_ATTR_BLENDINDICES) {
                for (unsigned c = 0; c < n; ++c) {
                    float f = (float)v->index[c] / 255.0f;
                    memcpy(dst + offset + c * 4u, &f, sizeof f);
                }
            } else {
                const float *src = attr_source(loader, v);
                if (!src) return -1;
                for (unsigned c = 0; c < n; ++c) {
                    float f = src[c];
                    if (!(f >= 0.0f)) f = 0.0f; /* NaN and negatives clamp */
                    if (f > 1.0f) f = 1.0f;
                    memcpy(dst + offset + c * 4u, &f, sizeof f);
                }
            }
            offset += n * 4u;
        } else if (loader->format == D9_PICA_VS_FMT_FLOAT) {
            const float *src = attr_source(loader, v);
            if (!src) return -1;
            memcpy(dst + offset, src, n * sizeof(float));
            offset += n * 4u;
        } else {
            return -1;
        }
    }
    return 0;
}


#ifdef __3DS__

int d9_pica_vs_device_open(D9PicaVsDevice *device,
                           const D9PicaVsProgram *program,
                           char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!device || !program) {
        if (why && why_size) snprintf(why, why_size, "invalid argument");
        return -1;
    }
    memset(device, 0, sizeof *device);
    if (d9_pica_vs_validate(program, why, why_size)) return -1;
    device->dvlb = DVLB_ParseFile((u32 *)program->shbin, program->shbin_size);
    if (!device->dvlb) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: DVLB_ParseFile rejected the .shbin",
                     program->id);
        return -1;
    }
    if (R_FAILED(shaderProgramInit(&device->program))) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: shaderProgramInit failed", program->id);
        DVLB_Free(device->dvlb);
        device->dvlb = NULL;
        return -1;
    }
    device->program_ready = 1;
    if (R_FAILED(shaderProgramSetVsh(&device->program, &device->dvlb->DVLE[0]))) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: shaderProgramSetVsh failed", program->id);
        d9_pica_vs_device_close(device);
        return -1;
    }
    device->loaded_program = program;
    device->open = 1;
    return 0;
}

void d9_pica_vs_device_close(D9PicaVsDevice *device) {
    if (!device) return;
    if (device->program_ready) shaderProgramFree(&device->program);
    if (device->dvlb) DVLB_Free(device->dvlb);
    memset(device, 0, sizeof *device);
}

static GPU_FORMATS pica_format(uint8_t format) {
    /* PICA integer loaders are not normalised; pack_vertex() has already
     * decoded UBYTE4N into floats. */
    (void)format;
    return GPU_FLOAT;
}

int d9_pica_vs_device_bind(D9PicaVsDevice *device,
                           const D9PicaVsProgram *program,
                           const D9PicaVsDecl *decl,
                           const float (*rows)[4], const uint32_t *row_mask,
                           unsigned row_count,
                           const void *packed, size_t vertex_count,
                           size_t stride, char *why, size_t why_size) {
    if (why && why_size) why[0] = 0;
    if (!device || !device->open || !program || !program->shbin ||
        !decl || !rows || !row_mask || !packed || !vertex_count) {
        if (why && why_size) snprintf(why, why_size, "PICA VS device is not loaded");
        return -1;
    }
    /* A bind may not mix a program from another device/open with this
     * declaration or bank.  All checks happen before any GPU state changes. */
    if (device->loaded_program != program ||
        program->attr_count != decl->count ||
        !decl->count || decl->count > D9_PICA_VS_MAX_ATTRS) {
        if (why && why_size)
            snprintf(why, why_size,
                     "PICA VS program %s does not match the loaded device/declaration",
                     program->id);
        return -1;
    }
    if (row_count < program->uniform_rows) {
        if (why && why_size)
            snprintf(why, why_size, "plan %s: %u uniform row(s), needs %u",
                     program->id, row_count, program->uniform_rows);
        return -1;
    }
    unsigned packed_stride = d9_pica_vs_vertex_stride(decl);
    if (!packed_stride || packed_stride != stride) {
        if (why && why_size)
            snprintf(why, why_size,
                     "plan %s: packed stride %u != declaration stride %u",
                     program->id, (unsigned)stride, packed_stride);
        return -1;
    }
    unsigned seen_offsets[D9_PICA_VS_MAX_ATTRS];
    for (unsigned i = 0; i < decl->count; ++i) {
        const D9PicaVsLoader *loader = &decl->loader[i];
        if (!loader->components || loader->components > 4u ||
            loader->format > D9_PICA_VS_FMT_UBYTE_NORM ||
            loader->reg >= 16u || (loader->offset & 3u)) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: loader %u has an invalid offset/format/register",
                         program->id, i);
            return -1;
        }
        for (unsigned j = 0; j < i; ++j)
            if (seen_offsets[j] == loader->offset) {
                if (why && why_size)
                    snprintf(why, why_size,
                             "plan %s: two loaders share declaration offset %u",
                             program->id, loader->offset);
                return -1;
            }
        seen_offsets[i] = loader->offset;
    }
    C3D_BindProgram(&device->program);
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    for (unsigned i = 0; i < decl->count; ++i) {
        const D9PicaVsLoader *loader = &decl->loader[i];
        if (AttrInfo_AddLoader(attr, (int)loader->reg, pica_format(loader->format),
                               (int)loader->components) < 0) {
            if (why && why_size)
                snprintf(why, why_size,
                         "plan %s: AttrInfo_AddLoader rejected v%u",
                         program->id, loader->reg);
            return -1;
        }
    }
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    if (BufInfo_Add(buf, (void *)packed, (ptrdiff_t)stride, (int)decl->count,
                    d9_pica_vs_bufinfo_permutation(decl)) < 0) {
        if (why && why_size)
            snprintf(why, why_size,
                     "plan %s: BufInfo_Add rejected the packed stream",
                     program->id);
        return -1;
    }
    D9PicaVsBank bank;
    if (d9_pica_vs_bank(program, &bank, why, why_size) ||
        d9_pica_vs_bank_ready(&bank, row_mask, 8u, why, why_size))
        return -1;
    for (unsigned row = 0; row < bank.rows; ++row) {
        unsigned reg = bank.d3d_reg[row];
        if ((row_mask[reg >> 5] >> (reg & 31u) & 1u) == 0) continue;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, (int)row, rows[reg][0], rows[reg][1],
                      rows[reg][2], rows[reg][3]);
    }
    (void)vertex_count;
    return 0;
}

#endif /* __3DS__ */
