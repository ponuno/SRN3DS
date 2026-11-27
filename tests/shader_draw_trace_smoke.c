/* Host test: N3DS_SHADER_DRAW_TRACE formatting and catalog resolution.
 *
 * The trace itself is observation-only (d3d9_shader.c).  This test exercises
 * the pure formatter for every PICA VS catalog row (including the five GFx
 * menu programs), so a draw that resolves to the catalog always prints a
 * concrete `<index>:<pinned .shbin SHA>` and `refusal=null`.
 */

#include "d3d9_shader.h"
#include "d3d9_pica_vs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition, text) do { ++checks; if (!(condition)) { \
    ++failures; printf("FAIL: %s (%s:%d)\n", text, __FILE__, __LINE__); \
} } while (0)

int main(void) {
    static char env[] = "N3DS_SHADER_DRAW_TRACE=1";
    putenv(env);
    CHECK(d9_shader_draw_trace_enabled() == 1, "trace env enables the hook");

    unsigned menu = 0;
    for (unsigned i = 0; i < d9_pica_vs_count(); ++i) {
        const D9PicaVsProgram *program = d9_pica_vs_at(i);
        char line[1024];
        char vs_program[192];
        CHECK(program != NULL, "catalog entry is readable");
        if (!program) continue;
        snprintf(vs_program, sizeof vs_program, "%u:%s", i,
                 program->shbin_sha256);
        d9_shader_draw_trace_format(line, sizeof line, "DrawPrimitive",
                                    program->sha256, program->id, vs_program,
                                    "none", "none", "none", 1, NULL);
        CHECK(strstr(line, "vs_program=") != NULL, "trace has vs_program");
        CHECK(strstr(line, "vs_program=none") == NULL,
              "resolved trace is not none");
        CHECK(strstr(line, program->shbin_sha256) != NULL,
              "trace carries the pinned shbin SHA");
        CHECK(strstr(line, "refusal=null") != NULL,
              "successful draw has refusal=null");
        d9_shader_draw_trace("DrawPrimitive", program->sha256, program->id,
                             vs_program, "none", "none", "none", 1, NULL);
        if (strncmp(program->id, "menu.gfx.", 9) == 0) ++menu;
    }
    CHECK(menu == 5, "five GFx menu programs resolve in the catalog");

    char refused[1024];
    d9_shader_draw_trace_format(refused, sizeof refused, "DrawPrimitive",
                                "none", "none", "none", "none", "none",
                                "none", 1, "plan is not draw-enabled");
    CHECK(strstr(refused, "refusal=plan is not draw-enabled") != NULL,
          "refusal reason is recorded verbatim");

    char multi[1024];
    d9_shader_draw_trace_format(multi, sizeof multi, "DrawPrimitive",
                                "hash", "plan", "plan:plan", "hash2",
                                "ps-plan", "tev:ps-plan", 3, NULL);
    CHECK(strstr(multi, "passes=3") != NULL, "pass count is recorded");

    if (failures) {
        printf("shader_draw_trace: %d checks, %d failures\n", checks, failures);
        return 1;
    }
    printf("shader_draw_trace: %d checks passed (%u menu programs)\n", checks,
           menu);
    return 0;
}
