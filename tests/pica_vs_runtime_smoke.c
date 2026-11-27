/* Host test: verified emitted PICA VS catalog, declaration and uniform bank.
 *
 * The `.shbin` bytes are embedded by `pack_plans.py emit-runtime` from
 * source/shader_packs/skinning/generated.  This test never opens a
 * device: it validates the DVLB image and its SHA-256 pin, maps a matching D3D9
 * declaration to the three PICA loaders, and lays out the 4-bone palette /
 * WVP uniform bank.  ARM compile is covered by the 3DS build of the same
 * translation unit.
 */

#include "d3d9_pica_vs.h"

#include <stdio.h>
#include <string.h>

#define SKIN_F8FFAECD_SHA \
    "f8ffaecd431d4416a4503b2e8ca80b6dad57a128dc1782c941f97db346072efa"
#define SKIN_60DBD11A_SHA \
    "60dbd11a23ffbf74517f1256edaadadb62441fe1ed120d413540b220b9833407"

static int checks, failures;
#define CHECK(condition, text) do { ++checks; if (!(condition)) { \
    ++failures; printf("FAIL: %s (%s:%d)\n", text, __FILE__, __LINE__); \
} } while (0)

static D9VertexElement entry(unsigned stream, unsigned offset, unsigned type,
                             unsigned usage, unsigned usage_index) {
    D9VertexElement e;
    e.stream = (uint16_t)stream;
    e.offset = (uint16_t)offset;
    e.type = (uint8_t)type;
    e.method = 0;
    e.usage = (uint8_t)usage;
    e.usage_index = (uint8_t)usage_index;
    return e;
}

static void skin_decl(D9Decl *decl, unsigned index_type) {
    D9VertexElement elements[4] = {{0}};
    elements[0] = entry(0, 0, D9_DECL_FLOAT3, D9_USAGE_POSITION, 0);
    elements[1] = entry(0, 12, D9_DECL_FLOAT4, 1 /* blendweight */, 0);
    elements[2] = entry(0, 28, index_type, 2 /* blendindices */, 0);
    memset(decl, 0, sizeof *decl);
    /* d9_decl_init is not linked here: the loader mapping only reads the
     * element table/count, which this test fills directly. */
    decl->count = 3;
    memcpy(decl->elements, elements, sizeof elements);
    decl->required[0] = 32;
    decl->stream_mask = 1u;
}

int main(void) {
    CHECK(d9_pica_vs_count() == 3, "three verified programs");

    for (unsigned i = 0; i < d9_pica_vs_count(); ++i) {
        const D9PicaVsProgram *p = d9_pica_vs_at(i);
        char why[192] = "";
        CHECK(p != NULL, "entry is readable");
        if (!p) continue;
        CHECK(d9_pica_vs_validate(p, why, sizeof why) == 0, why);
        CHECK(p->shbin_size == 500u || p->shbin_size == 532u ||
              p->shbin_size == 548u, "shbin size is the emitter's");
        CHECK(p->shbin[0] == 'D' && p->shbin[1] == 'V', "DVLB magic");
        CHECK(strlen(p->shbin_sha256) == 64, "pinned shbin SHA-256 present");
    }

    const D9PicaVsProgram *p = d9_pica_vs_lookup(SKIN_F8FFAECD_SHA);
    CHECK(p != NULL, "exact-hash lookup hits f8ffaecd");
    CHECK(p && p->instructions == 53u, "f8ffaecd has 53 emitted instructions");
    CHECK(p && p->uniform_rows == 16u, "f8ffaecd uses c0..c15");
    CHECK(p && p->attr_count == 3u, "f8ffaecd declares position/weight/index");
    CHECK(d9_pica_vs_lookup(SKIN_60DBD11A_SHA) != NULL, "60dbd11a is present");
    CHECK(d9_pica_vs_lookup(
              "0000000000000000000000000000000000000000000000000000000000000000")
          == NULL, "foreign hash is a miss, not a fallback");

    /* Full declaration contract. */
    D9Decl decl;
    D9PicaVsDecl loaders;
    char why[192] = "";
    skin_decl(&decl, D9_DECL_UBYTE4N);
    CHECK(d9_pica_vs_declare(p, &decl, &loaders, why, sizeof why) == 0, why);
    CHECK(loaders.count == 3u, "three PICA loaders");
    if (loaders.count == 3u) {
        CHECK(loaders.loader[0].attr == D9_PICA_VS_ATTR_POSITION, "loader 0 is position");
        CHECK(loaders.loader[0].components == 3u, "position loads 3 components");
        CHECK(loaders.loader[1].attr == D9_PICA_VS_ATTR_BLENDWEIGHT, "loader 1 is weight");
        CHECK(loaders.loader[2].attr == D9_PICA_VS_ATTR_BLENDINDICES, "loader 2 is index");
        CHECK(loaders.loader[2].format == D9_PICA_VS_FMT_UBYTE_NORM,
              "indices load normalised bytes");
        CHECK(loaders.loader[2].offset == 28u, "index offset comes from the declaration");
    }

    /* Raw UBYTE4 would feed 0..255 indices to a x765 floor: named refusal. */
    skin_decl(&decl, D9_DECL_UBYTE4);
    why[0] = 0;
    CHECK(d9_pica_vs_declare(p, &decl, &loaders, why, sizeof why) != 0,
          "raw UBYTE4 indices are refused");
    CHECK(strstr(why, "blendindices") != NULL, why);

    /* A declaration element the shader never reads is refused, not dropped. */
    skin_decl(&decl, D9_DECL_UBYTE4N);
    decl.elements[3] = entry(0, 32, D9_DECL_FLOAT2, D9_USAGE_TEXCOORD, 0);
    decl.count = 4;
    why[0] = 0;
    CHECK(d9_pica_vs_declare(p, &decl, &loaders, why, sizeof why) != 0,
          "an extra texcoord is refused for the position/weight/index program");
    CHECK(strstr(why, "texcoord") != NULL || strstr(why, "usage 5") != NULL, why);

    /* Uniform bank: c0..c11 palette, c12..c15 WVP. */
    D9PicaVsBank bank;
    why[0] = 0;
    CHECK(d9_pica_vs_bank(p, &bank, why, sizeof why) == 0, why);
    CHECK(bank.rows == 16u, "bank has 16 rows");
    CHECK(bank.d3d_reg[0] == 0u && bank.d3d_reg[11] == 11u, "palette rows map c0..c11");
    CHECK(bank.d3d_reg[12] == 180u && bank.d3d_reg[15] == 183u, "WVP maps c180..c183");
    uint32_t mask[8];
    memset(mask, 0, sizeof mask);
    for (unsigned reg = 0; reg < 12u; ++reg) mask[reg >> 5] |= 1u << (reg & 31u);
    for (unsigned reg = 180u; reg < 184u; ++reg) mask[reg >> 5] |= 1u << (reg & 31u);
    why[0] = 0;
    CHECK(d9_pica_vs_bank_ready(&bank, mask, 8, why, sizeof why) == 0, why);
    mask[181u >> 5] &= ~(1u << (181u & 31u));
    why[0] = 0;
    CHECK(d9_pica_vs_bank_ready(&bank, mask, 8, why, sizeof why) != 0,
          "missing c181 is a named refusal");
    CHECK(strstr(why, "c181") != NULL, why);

    /* The 17-row program adds D3D c184 for the UV transform. */
    const D9PicaVsProgram *q = d9_pica_vs_lookup(SKIN_60DBD11A_SHA);
    why[0] = 0;
    CHECK(q && d9_pica_vs_bank(q, &bank, why, sizeof why) == 0, why);
    CHECK(bank.rows == 17u && bank.d3d_reg[16] == 184u, "row 16 maps D3D c184");
    mask[181u >> 5] |= 1u << (181u & 31u);
    mask[184u >> 5] |= 1u << (184u & 31u);
    why[0] = 0;
    CHECK(d9_pica_vs_bank_ready(&bank, mask, 8, why, sizeof why) == 0, why);
    /* c184 carries the UV transform; it must be a real row, not an implicit
     * zero.  Dropping it is a named refusal. */
    mask[184u >> 5] &= ~(1u << (184u & 31u));
    why[0] = 0;
    CHECK(d9_pica_vs_bank_ready(&bank, mask, 8, why, sizeof why) != 0,
          "missing c184 is a named refusal");
    CHECK(strstr(why, "c184") != NULL, why);
    mask[184u >> 5] |= 1u << (184u & 31u);

    /* A program's attr_count/registers are part of the validated contract. */
    {
        D9PicaVsProgram bad = *p;
        bad.attrs[0].reg = 16u;
        why[0] = 0;
        CHECK(d9_pica_vs_validate(&bad, why, sizeof why) != 0,
              "attribute register 16 is rejected");
        CHECK(strstr(why, "attribute") != NULL, why);
        bad = *p;
        bad.attr_count = 0;
        why[0] = 0;
        CHECK(d9_pica_vs_validate(&bad, why, sizeof why) != 0,
              "attr_count=0 is rejected");
    }
    /* A DVLE offset that would overflow the old dvle+4 check is rejected. */
    {
        D9PicaVsProgram bad = *p;
        uint8_t blob[512];
        memcpy(blob, p->shbin, p->shbin_size);
        blob[8] = 0xff; blob[9] = 0xff; blob[10] = 0xff; blob[11] = 0x7f;
        bad.shbin = blob;
        why[0] = 0;
        CHECK(d9_pica_vs_validate(&bad, why, sizeof why) != 0,
              "huge DVLE offset is rejected without overflow");
    }

    if (failures) {
        printf("%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("pica_vs_runtime: %d checks passed\n", checks);
    return 0;
}
