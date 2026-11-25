/* Execution profile of lifted functions. See rt_prof.h for the contract. */
#include "rt_prof.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROF_SLOTS (1u << 16) /* 65,536 slots: 512 KiB of .bss, ~24k functions at 40% load */

typedef struct { uint32_t addr, hits; } ProfSlot;
static ProfSlot prof_slots[PROF_SLOTS];
static uint64_t prof_total;
static size_t prof_used;
int rt_prof_on_hint;

void rt_prof_enable(int on) { rt_prof_on_hint = on ? 1 : 0; }
int rt_prof_enabled(void) { return rt_prof_on_hint; }
void rt_prof_reset(void) {
    memset(prof_slots, 0, sizeof prof_slots);
    prof_total = 0;
    prof_used = 0;
}
static ProfSlot *slot_find(uint32_t addr) {
    uint32_t h = ((addr >> 2) * 2654435761u) & (PROF_SLOTS - 1);
    for (unsigned probe = 0; probe < 64; ++probe) {
        ProfSlot *s = &prof_slots[(h + probe) & (PROF_SLOTS - 1)];
        if (!s->hits) return NULL;
        if (s->addr == addr) return s;
    }
    return NULL;
}
static ProfSlot *slot_insert(uint32_t addr) {
    if (prof_used + 1 > PROF_SLOTS / 2) return NULL;
    uint32_t h = ((addr >> 2) * 2654435761u) & (PROF_SLOTS - 1);
    for (unsigned probe = 0; probe < 64; ++probe) {
        ProfSlot *s = &prof_slots[(h + probe) & (PROF_SLOTS - 1)];
        if (!s->hits) { s->addr = addr; s->hits = 0; ++prof_used; return s; }
    }
    return NULL;
}
void rt_prof_hit(uint32_t addr) {
    if (!addr) return;
    ProfSlot *s = slot_find(addr);
    if (!s) s = slot_insert(addr);
    if (s) ++s->hits;
    ++prof_total;
}
size_t rt_prof_distinct(void) { return prof_used; }
uint64_t rt_prof_total(void) { return prof_total; }
int rt_prof_slot(size_t index, uint32_t *addr, uint32_t *hits) {
    if (index >= PROF_SLOTS) return 0;
    ProfSlot *s = &prof_slots[index];
    if (!s->hits) return 0;
    if (addr) *addr = s->addr;
    if (hits) *hits = s->hits;
    return 1;
}
uint32_t rt_prof_hits(uint32_t addr) {
    ProfSlot *s = slot_find(addr);
    return s ? s->hits : 0;
}

typedef struct { uint32_t addr, hits; } Row;
static int row_cmp(const void *a, const void *b) {
    const Row *x = a, *y = b;
    if (x->hits != y->hits) return x->hits < y->hits ? 1 : -1;
    return x->addr < y->addr ? -1 : 1;
}
static Row *sorted_rows(size_t *out_count) {
    Row *rows = malloc(sizeof(Row) * (prof_used ? prof_used : 1));
    if (!rows) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < PROF_SLOTS; ++i)
        if (prof_slots[i].hits) { rows[n].addr = prof_slots[i].addr; rows[n].hits = prof_slots[i].hits; ++n; }
    qsort(rows, n, sizeof *rows, row_cmp);
    *out_count = n;
    return rows;
}
int rt_prof_write_csv(const char *path) {
    if (!path) return -1;
    size_t n = 0;
    Row *rows = sorted_rows(&n);
    if (!rows) return -1;
    FILE *f = fopen(path, "w");
    if (!f) { free(rows); return -1; }
    fprintf(f, "addr,hits\n");
    for (size_t i = 0; i < n; ++i) fprintf(f, "%08lx,%lu\n",
        (unsigned long)rows[i].addr, (unsigned long)rows[i].hits);
    int bad = ferror(f);
    fclose(f);
    free(rows);
    return bad ? -1 : (int)n;
}
void rt_prof_print_top(unsigned n) {
    size_t count = 0;
    Row *rows = sorted_rows(&count);
    if (!rows) return;
    size_t limit = count < n ? count : n;
    printf("profile: %lu distinct functions, %lu calls\n",
           (unsigned long)count, (unsigned long)prof_total);
    for (size_t i = 0; i < limit; ++i)
        printf("  %08lx  %lu\n", (unsigned long)rows[i].addr, (unsigned long)rows[i].hits);
    free(rows);
}
