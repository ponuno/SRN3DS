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

int main(void) {
    D9MenuShaderMatch m;
    char why[256];
    uint32_t code[256];

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

    if (failures) {
        printf("menu_vs_family: %d failure(s)\n", failures);
        return 1;
    }
    puts("menu_vs_family: normal/color1/abs, texgen and zyxw permutations PASS");
    return 0;
}
