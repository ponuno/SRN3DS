/* Host test for source/mem_stats.c: profile arithmetic, env-limit parsing,
 * guest peak, linear/VRAM deltas on a fake allocator, line format and counter
 * leak checks.  No libctru, no GPU. */
#include "mem_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

typedef struct { uint64_t linear, vram; } FakeHeap;

static uint64_t fake_linear(void *ctx) { return ((FakeHeap *)ctx)->linear; }
static uint64_t fake_vram(void *ctx) { return ((FakeHeap *)ctx)->vram; }

static void test_profile_table(void)
{
    struct { unsigned heap, linear; int valid; unsigned budget; } rows[] = {
        {96, 8, 1, 92},
        {104, 16, 1, 100},
        {120, 24, 1, 116},
        {128, 26, 0, 124},
    };
    printf("profile  heap  linear  nonheap  sum  region  valid  guest_budget\n");
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        unsigned h = rows[i].heap, l = rows[i].linear;
        printf("%4ux%-3u %5u %7u %8u %4u %7u %6d %12u MiB\n", h, l, h, l,
               MEM_STATS_NON_HEAP_MIB, h + l + MEM_STATS_NON_HEAP_MIB,
               MEM_STATS_REGION_MIB, mem_stats_profile_valid(h, l),
               (unsigned)(mem_stats_guest_budget(h, 4) >> 20));
        CHECK(mem_stats_profile_valid(h, l) == rows[i].valid);
        CHECK(mem_stats_guest_budget(h, 4) == ((size_t)rows[i].budget << 20));
    }
    CHECK(120 + 24 + MEM_STATS_NON_HEAP_MIB == MEM_STATS_REGION_MIB);
}

static void test_parse(void)
{
    struct { const char *text; int ok; size_t value; } cases[] = {
        {"0", 1, 0},
        {"4194304", 1, 4194304u},
        {"512K", 1, 512u << 10},
        {"4M", 1, 4u << 20},
        {"2m", 1, 2u << 20},
        {" 8 M ", 1, 8u << 20},
        {"1G", 1, 1u << 30},
        {"  ", 0, 0},
        {"", 0, 0},
        {"-1", 0, 0},
        {"+1", 0, 0},
        {"12MB", 0, 0},
        {"1.5M", 0, 0},
        {"abc", 0, 0},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        int ok = 0;
        size_t v = mem_stats_parse_limit(cases[i].text, &ok);
        if (ok != cases[i].ok || (ok && v != cases[i].value)) {
            fprintf(stderr, "FAIL parse '%s': ok=%d value=%zu (want ok=%d value=%zu)\n",
                    cases[i].text, ok, v, cases[i].ok, cases[i].value);
            ++g_failures;
        }
    }
}

static void test_guest_and_format(void)
{
    mem_stats_configure(120, 24, 4);
    mem_stats_set_guest(1000, 10000, 3);
    mem_stats_set_guest(9000, 10000, 4);
    mem_stats_set_guest(4000, 10000, 5);
    mem_stats_set_newlib(2000, 3000);
    mem_stats_set_guest(4000, 10000, 5);
    mem_stats_set_newlib(2500, 2500);
    mem_stats_set_irc(4u << 20, 64);
    mem_stats_set_irdecode(1234, 9);
    mem_stats_set_bsa(0, 13);
    mem_stats_set_texcache(100, 200, 300, 400, 2, 1, 7);
    mem_stats_set_tex_limits(4u << 20, 2u << 20);
    mem_stats_set_os_region(178u << 20, 60u << 20, 118u << 20);
    mem_stats_note_arena("vertex", 16384u * 20u);
    mem_stats_note_arena("colour", 65536u * 40u);
    mem_stats_note_arena("cube", 16384u * 40u);
    mem_stats_note_arena("index", 262144u * 2u);
    mem_stats_note_arena("skin", 4096u * 80u);
    mem_stats_note_arena("srquad", 6u * 36u);

    char line[2048];
    size_t n = mem_stats_format(line, sizeof line, "test", "extra_key=42");
    CHECK(n > 0);
    CHECK(line[n - 1] == '\n');
    CHECK(strstr(line, "mem tag=test ") == line);
    CHECK(strstr(line, "heap_mib=120") != NULL);
    CHECK(strstr(line, "linear_mib=24") != NULL);
    CHECK(strstr(line, "guest_budget=121634816") != NULL);
    CHECK(strstr(line, "guest_peak=9000") != NULL);
    CHECK(strstr(line, "guest_regions=5") != NULL);
    CHECK(strstr(line, "newlib_peak=2500") != NULL);
    CHECK(strstr(line, "irc_bytes=4194304") != NULL);
    CHECK(strstr(line, "irdec_bytes=1234") != NULL);
    CHECK(strstr(line, "irdec_blocks=9") != NULL);
    CHECK(strstr(line, "bsa_archives=13") != NULL);
    CHECK(strstr(line, "have_bsa=0") != NULL);
    CHECK(strstr(line, "tex_guest_limit=4194304") != NULL);
    CHECK(strstr(line, "tex_gpu_limit=2097152") != NULL);
    CHECK(strstr(line, "tex_trim_evictions=1") != NULL);
    CHECK(strstr(line, "tex_evictions_gpu=7") != NULL);
    CHECK(strstr(line, "os_region_size=186646528") != NULL);
    CHECK(strstr(line, "have_os_region=1") != NULL);
    CHECK(strstr(line, "arena_vertex=327680") != NULL);
    CHECK(strstr(line, "arena_colour=2621440") != NULL);
    CHECK(strstr(line, "arena_cube=655360") != NULL);
    CHECK(strstr(line, "arena_index=524288") != NULL);
    CHECK(strstr(line, "arena_skin=327680") != NULL);
    CHECK(strstr(line, "arenas_bytes=4456664") != NULL);
    CHECK(strstr(line, "extra_key=42") != NULL);
    CHECK(strstr(line, "\nextra_key") == NULL);

    /* One key=value per token; every token contains '='. */
    char copy[2048];
    snprintf(copy, sizeof copy, "%s", line);
    unsigned tokens = 0;
    for (char *tok = strtok(copy, " \n"); tok; tok = strtok(NULL, " \n")) {
        if (!strcmp(tok, "mem")) continue;
        ++tokens;
        CHECK(strchr(tok, '=') != NULL);
    }
    CHECK(tokens > 30);
}

static void install_fake(FakeHeap *f)
{
    f->linear = 1000;
    f->vram = 4096;
    MemStatsSpaceProbes p = {fake_linear, fake_vram, f};
    mem_stats_set_space_probes(&p);
}

static void test_space_accounting(void)
{
    mem_stats_configure(96, 8, 4);
    FakeHeap f;
    install_fake(&f);

    uint64_t before = mem_stats_space_begin(MEM_STATS_LINEAR);
    CHECK(before == 1000);
    f.linear -= 100;
    mem_stats_space_end(MEM_STATS_LINEAR, before, 100, 1);
    CHECK(mem_stats_live_bytes(MEM_STATS_LINEAR) == 100);

    before = mem_stats_space_begin(MEM_STATS_LINEAR);
    CHECK(before == 900);
    /* failed request: free unchanged, requested size recorded */
    mem_stats_space_end(MEM_STATS_LINEAR, before, 250, 0);
    CHECK(mem_stats_live_bytes(MEM_STATS_LINEAR) == 100);

    before = mem_stats_space_begin(MEM_STATS_LINEAR);
    f.linear += 100;
    mem_stats_space_released(MEM_STATS_LINEAR, before);
    CHECK(mem_stats_live_bytes(MEM_STATS_LINEAR) == 0);

    /* VRAM is independent. */
    before = mem_stats_space_begin(MEM_STATS_VRAM);
    CHECK(before == 4096);
    f.vram -= 512;
    mem_stats_space_end(MEM_STATS_VRAM, before, 512, 1);
    CHECK(mem_stats_live_bytes(MEM_STATS_VRAM) == 512);
    CHECK(mem_stats_live_bytes(MEM_STATS_LINEAR) == 0);

    char line[2048];
    mem_stats_format(line, sizeof line, "space", NULL);
    CHECK(strstr(line, "linear_live=0") != NULL);
    CHECK(strstr(line, "linear_peak=100") != NULL);
    CHECK(strstr(line, "linear_min_free=900") != NULL);
    CHECK(strstr(line, "linear_fail_n=1") != NULL);
    CHECK(strstr(line, "linear_fail_bytes=250") != NULL);
    CHECK(strstr(line, "vram_live=512") != NULL);
    CHECK(strstr(line, "vram_peak=512") != NULL);
    CHECK(strstr(line, "vram_min_free=3584") != NULL);
    CHECK(strstr(line, "alloc_failures=1") != NULL);

    /* Repeated snapshots must not mutate counters. */
    char again[2048];
    mem_stats_format(again, sizeof again, "space", NULL);
    CHECK(strstr(again, "linear_fail_n=1") != NULL);
    CHECK(strstr(again, "vram_peak=512") != NULL);
    CHECK(strstr(again, "alloc_failures=1") != NULL);
}

int main(void)
{
    mem_stats_init();
    test_profile_table();
    test_parse();
    test_guest_and_format();
    test_space_accounting();
    if (g_failures) {
        fprintf(stderr, "mem_stats: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("mem_stats: all checks passed\n");
    return 0;
}
