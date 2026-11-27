/* Host test: measured menu.generic.vs permutations.
 *
 * The retail StartMenu family contains members with normal0/color1 inputs and
 * the abs opcode, and members that omit color1 or color0 outputs.  The
 * selector must accept them without widening the D3D9 caps, and it must mark
 * only the fixed CPU transform (position dp4 c0..c3, texcoord mov or
 * dp4 c4/c5, colour mov) as backend-executable.
 */

#include "d3d9_menu_shader_runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s\n", #cond); } \
} while (0)

#define VER 0xFFFE0300u
#define END 0x0000FFFFu
static uint32_t DI(unsigned op, unsigned operands) {
    return op | (operands << 24);
}
static uint32_t dst(unsigned type, unsigned reg, unsigned mask) {
    return 0x80000000u | ((type & 7u) << 28) | (reg & 0x7FFu) |
           ((mask & 0xFu) << 16);
}
static uint32_t src(unsigned type, unsigned reg, unsigned swz) {
    return 0x80000000u | ((type & 7u) << 28) | (reg & 0x7FFu) |
           ((swz & 0xFFu) << 16);
}
static uint32_t dcl_in(unsigned usage, unsigned index) {
    return usage | (index << 16);
}

/* position v0, texcoord v1, color v2, normal v3, color1 v4 */
static size_t build_menu_vs(uint32_t *c, int texgen, int color_zyxw,
                            int with_color, int with_color1) {
    size_t n = 0;
    c[n++] = VER;
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(0, 0);  c[n++] = dst(1, 0, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(5, 0);  c[n++] = dst(1, 1, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(10, 0); c[n++] = dst(1, 2, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(3, 0);  c[n++] = dst(1, 3, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(10, 1); c[n++] = dst(1, 4, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(0, 0);  c[n++] = dst(6, 0, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(5, 0);  c[n++] = dst(6, 1, 0xF);
    if (with_color) {
        c[n++] = DI(0x1F, 2); c[n++] = dcl_in(10, 0); c[n++] = dst(6, 2, 0xF);
    }
    if (with_color1) {
        c[n++] = DI(0x1F, 2); c[n++] = dcl_in(10, 1); c[n++] = dst(6, 3, 0xF);
    }
    /* position = dp4(c0..c3, v0) */
    for (unsigned row = 0; row < 4u; ++row) {
        c[n++] = DI(0x09, 3); c[n++] = dst(6, 0, 1u << row);
        c[n++] = src(1, 0, 0xE4); c[n++] = src(2, row, 0xE4);
    }
    if (texgen) {
        c[n++] = DI(0x09, 3); c[n++] = dst(6, 1, 1u);
        c[n++] = src(1, 0, 0xE4); c[n++] = src(2, 4, 0xE4);
        c[n++] = DI(0x09, 3); c[n++] = dst(6, 1, 2u);
        c[n++] = src(1, 0, 0xE4); c[n++] = src(2, 5, 0xE4);
    } else {
        c[n++] = DI(0x01, 2); c[n++] = dst(6, 1, 3u); c[n++] = src(1, 1, 0xE4);
    }
    if (with_color) {
        c[n++] = DI(0x01, 2); c[n++] = dst(6, 2, 0xF);
        c[n++] = src(1, 2, color_zyxw ? 0xC6u : 0xE4u);
    }
    if (with_color1) {
        c[n++] = DI(0x01, 2); c[n++] = dst(6, 3, 0xF); c[n++] = src(1, 4, 0xE4);
    }
    /* measured abs opcode; result is not on the visible path */
    c[n++] = DI(0x23, 2); c[n++] = dst(0, 0, 0xF); c[n++] = src(1, 3, 0xE4);
    c[n++] = END;
    return n;
}

/* position v0, texcoord v1, tangent v2, binormal v3.  The tangent/binormal
 * declaration is measured; whether it is safe depends only on dataflow into
 * the visible position/texcoord/color0 outputs. */
static size_t build_tangent_vs(uint32_t *c, int feed_position) {
    size_t n = 0;
    c[n++] = VER;
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(0, 0); c[n++] = dst(1, 0, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(5, 0); c[n++] = dst(1, 1, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(6, 0); c[n++] = dst(1, 2, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(7, 0); c[n++] = dst(1, 3, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(0, 0); c[n++] = dst(6, 0, 0xF);
    c[n++] = DI(0x1F, 2); c[n++] = dcl_in(5, 0); c[n++] = dst(6, 1, 0xF);
    for (unsigned row = 0; row < 4u; ++row) {
        c[n++] = DI(0x09, 3); c[n++] = dst(6, 0, 1u << row);
        c[n++] = src(2, row, 0xE4);
        c[n++] = src(1, feed_position ? 2u : 0u, 0xE4);
    }
    c[n++] = DI(0x01, 2); c[n++] = dst(6, 1, 3u); c[n++] = src(1, 1, 0xE4);
    /* tangent/binormal math, result only in a temp: not on the visible path. */
    c[n++] = DI(0x08, 3); c[n++] = dst(0, 0, 0xF);
    c[n++] = src(1, 2, 0xE4); c[n++] = src(1, 3, 0xE4);
    c[n++] = END;
    return n;
}

/* Read one dumped token stream: <dir>\vs_<sha>.bin (little-endian words). */
static int load_dump(const char *sha, uint32_t *code, size_t cap, size_t *words) {
    const char *dir = getenv("TESV_MENU_VS_DUMPS");
    char path[512];
    FILE *file;
    long size;
    if (!dir || !*dir) dir = "E:\\menu_draw_dump";
    snprintf(path, sizeof path, "%s\\vs_%s.bin", dir, sha);
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    size = ftell(file);
    if (size <= 0 || (size_t)size / 4u > cap || (size % 4) != 0) {
        fclose(file);
        return 0;
    }
    rewind(file);
    if (fread(code, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    *words = (size_t)size / 4u;
    return 1;
}

int main(void) {
    D9MenuShaderMatch m;
    char why[256];
    uint32_t code[512];

    size_t n = build_menu_vs(code, 0, 0, 1, 1);
    int r = d9_menu_shader_match_vs(code, n, &m, why, sizeof why);
    if (r != 1 || m.backend_cpu_safe != 1u)
        printf("case1 r=%d safe=%u uvgen=%u swap=%u why=%s\n", r,
               m.backend_cpu_safe, m.backend_uvgen, m.backend_color_swap, why);
    CHECK(r == 1);
    CHECK(m.kind == D9_MENU_SHADER_GENERIC_VS);
    CHECK(m.backend_cpu_safe == 1u);
    CHECK(m.backend_uvgen == 0u && m.backend_color_swap == 0u);

    n = build_menu_vs(code, 1, 1, 1, 1);
    r = d9_menu_shader_match_vs(code, n, &m, why, sizeof why);
    if (r != 1 || m.backend_cpu_safe != 1u)
        printf("case2 r=%d safe=%u uvgen=%u swap=%u why=%s\n", r,
               m.backend_cpu_safe, m.backend_uvgen, m.backend_color_swap, why);
    CHECK(r == 1);
    CHECK(m.backend_cpu_safe == 1u);
    CHECK(m.backend_uvgen == 1u);
    CHECK(m.backend_color_swap == 1u);

    /* No color0/color1 outputs: the GFx position+texgen member. */
    n = build_menu_vs(code, 1, 0, 0, 0);
    r = d9_menu_shader_match_vs(code, n, &m, why, sizeof why);
    if (r != 1 || m.backend_cpu_safe != 1u)
        printf("case3 r=%d safe=%u why=%s\n", r, m.backend_cpu_safe, why);
    CHECK(r == 1);
    CHECK(m.backend_cpu_safe == 1u);

    /* A different position constant row is a family member but not the fixed
     * CPU transform (which is exactly c0..c3). */
    n = build_menu_vs(code, 0, 0, 1, 1);
    for (size_t i = 1; i + 3 < n; ++i) {
        if ((code[i] & 0xFFFFu) == 0x09u &&
            ((code[i] >> 24) & 0xFu) == 3u) {
            code[i + 3] = (code[i + 3] & ~0x7FFu) | 6u;
            break;
        }
    }
    r = d9_menu_shader_match_vs(code, n, &m, why, sizeof why);
    CHECK(r == 1);
    CHECK(m.backend_cpu_safe == 0u);

    /* tangent0/binormal0 are measured menu-family declarations.  They are
     * accepted when the bytecode keeps them off position/texcoord/color0. */
    n = build_tangent_vs(code, 0);
    r = d9_menu_shader_match_vs(code, n, &m, why, sizeof why);
    if (r != 1 || m.backend_cpu_safe != 1u)
        printf("case4 r=%d safe=%u why=%s\n", r, m.backend_cpu_safe, why);
    CHECK(r == 1);
    CHECK(m.backend_cpu_safe == 1u);

    /* Same declaration but the tangent feeds a position row: the family match
     * is still structural, the CPU gate must refuse it by dataflow. */
    n = build_tangent_vs(code, 1);
    r = d9_menu_shader_match_vs(code, n, &m, why, sizeof why);
    CHECK(r == 1);
    CHECK(m.backend_cpu_safe == 0u);

    /* Real dumped menu.generic.vs permutations.  The bytes stay outside the
     * repository; the test skips with a notice when the dump is absent. */
    {
        static const char *accepted_texgen =
            "6186c545f488d0f84789cf3e0ca55d0fd87f37c2091f87239f5dccf7afc404a0";
        static const char *tangent_member =
            "10d46b0a59aa18fcd51d907081bd781c323ddfd871c90a756f69ae10bd63c32a";
        static const char *lrp_member =
            "08865907d27f8eaf21be2b1427a20edae7fcce0311487f24117ebce6a2e7d66a";
        size_t words = 0;
        if (!load_dump(accepted_texgen, code, 512, &words)) {
            puts("menu_vs_family: real dump absent, dumped-bytecode cases skipped");
        } else {
            r = d9_menu_shader_match_vs(code, words, &m, why, sizeof why);
            if (r != 1 || m.backend_cpu_safe != 1u || m.backend_uvgen != 1u)
                printf("dump accepted r=%d safe=%u uvgen=%u why=%s\n", r,
                       m.backend_cpu_safe, m.backend_uvgen, why);
            CHECK(r == 1);
            CHECK(m.backend_cpu_safe == 1u);
            CHECK(m.backend_uvgen == 1u);

            words = 0;
            if (load_dump(tangent_member, code, 512, &words)) {
                r = d9_menu_shader_match_vs(code, words, &m, why, sizeof why);
                if (r != 1 || m.backend_cpu_safe != 0u)
                    printf("dump tangent r=%d safe=%u why=%s\n", r,
                           m.backend_cpu_safe, why);
                /* tangent/binormal are family inputs, but the measured member
                 * also builds position from a non-fixed temp. */
                CHECK(r == 1);
                CHECK(m.backend_cpu_safe == 0u);
            }

            words = 0;
            if (load_dump(lrp_member, code, 512, &words)) {
                r = d9_menu_shader_match_vs(code, words, &m, why, sizeof why);
                if (r != -1 || !strstr(why, "lrp") || !strstr(why, "0x12"))
                    printf("dump lrp r=%d why=%s\n", r, why);
                CHECK(r == -1);
                CHECK(strstr(why, "lrp") != NULL);
                CHECK(strstr(why, "0x12") != NULL);
            }
        }
    }

    if (failures) {
        printf("menu_vs_family: %d failure(s)\n", failures);
        return 1;
    }
    puts("menu_vs_family: normal/color1/abs/tangent/binormal, texgen and zyxw permutations PASS");
    return 0;
}
