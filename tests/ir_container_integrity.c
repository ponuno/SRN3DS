/* Container API, LRU and deterministic failure tests.
 *
 * Driven by tests/ir_container_test.py. It never executes guest code; the
 * stubs exist only because ir_container.c reuses ir_exec.c's body.
 *
 * Modes:
 *   open PATH                     success, count, lookup, stats
 *   expect-open PATH CODE         open must fail with CODE
 *   expect-load PATH ADDR CODE    pin must fail with CODE, twice (deterministic)
 *   short PATH ADDR               open, truncate the record, pin must SHORT
 *   cache PATH                    bounded LRU, pin/unpin, eviction stats
 */
#include "ir_container.h"
#include "guest.h"
#include "rt_table.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

static char g_fault[256];

void *n3ds_guest_ptr(uint32_t addr, size_t bytes) {
    static uint8_t dummy[65536];
    (void)addr;
    return bytes <= sizeof dummy ? dummy : NULL;
}

void rt_unimpl(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    snprintf(g_fault, sizeof g_fault, "fault at %08lx: %s", (unsigned long)addr,
             what ? what : "unknown");
    fprintf(stderr, "%s\n", g_fault);
    abort();
}

void rt_int3(CPU *c, uint32_t addr) { rt_unimpl(c, addr, "int3"); }
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d) {
    (void)c; (void)a; (void)b; (void)cc; (void)d;
}
uint64_t rt_rdtsc(void) { return 0; }
void n3ds_lift_set_ir_ip(uint32_t ip) { (void)ip; }
void n3ds_a43f_trace_pre(CPU *c, uint32_t block_ip, uint32_t func_addr,
                         int is_irc) {
    (void)c; (void)block_ip; (void)func_addr; (void)is_irc;
}
void n3ds_a43f_trace_block(CPU *c, uint32_t block_ip, uint32_t func_addr,
                           uint32_t next, int is_irc) {
    (void)c; (void)block_ip; (void)func_addr; (void)next; (void)is_irc;
}
void n3ds_a43f_trace_dispatch(CPU *c, uint32_t addr) { (void)c; (void)addr; }
void n3ds_a43f_trace_aot_pre(CPU *c) { (void)c; }
void n3ds_a43f_trace_aot_post(CPU *c) { (void)c; }
void n3ds_a43f_trace_periodic(CPU *c) { (void)c; }
int n3ds_rt_dispatch(CPU *c, uint32_t addr) { (void)c; (void)addr; return -1; }
void rt_call(CPU *c, uint32_t addr) { rt_unimpl(c, addr, "unexpected rt_call"); }
void rt_call_ic(CPU *c, uint32_t addr, const FnEntry **cache) {
    (void)cache;
    rt_unimpl(c, addr, "unexpected rt_call_ic");
}

static int find_first_decoded(N3dsIrc *irc, unsigned skip, N3dsIrcEntry *out) {
    unsigned n = n3ds_irc_function_count(irc);
    for (unsigned i = 0; i < n; ++i) {
        N3dsIrcEntry e;
        if (n3ds_irc_entry_at(irc, i, &e)) continue;
        if (!(e.flags & N3DS_IRC_FN_DECODED)) continue;
        if (!skip) { *out = e; return 0; }
        skip--;
    }
    return -1;
}

static int mode_open(const char *path) {
    int st = 0;
    N3dsIrc *irc = n3ds_irc_open_ex(path, &st);
    if (!irc) {
        printf("open failed: %s (%d)\n", n3ds_irc_strstatus(st), st);
        return 1;
    }
    unsigned n = n3ds_irc_function_count(irc);
    printf("open ok: functions %u, decoded %u, file %llu, index %llu\n",
           n, n3ds_irc_decoded_count(irc),
           (unsigned long long)n3ds_irc_file_size(irc),
           (unsigned long long)n3ds_irc_index_size(irc));
    if (!n) return 1;
    N3dsIrcEntry e;
    if (n3ds_irc_entry_at(irc, 0, &e)) return 1;
    N3dsIrcEntry looked;
    if (n3ds_irc_lookup(irc, e.addr, &looked) != N3DS_IRC_OK) return 1;
    if (looked.addr != e.addr || looked.end != e.end || looked.record_size != e.record_size)
        return 1;
    if (n3ds_irc_lookup(irc, 0xfffffff0u, &looked) != N3DS_IRC_ERR_NOT_FOUND) return 1;
    N3dsIrcStats stats;
    n3ds_irc_stats(irc, &stats);
    printf("stats: lookups %llu hits %llu misses %llu\n",
           (unsigned long long)stats.lookups, (unsigned long long)stats.lookup_hits,
           (unsigned long long)stats.lookup_misses);
    n3ds_irc_close(irc);
    return 0;
}

static int mode_expect_open(const char *path, int want) {
    int st = 999;
    N3dsIrc *irc = n3ds_irc_open_ex(path, &st);
    if (irc) {
        printf("expected open failure %d, got success\n", want);
        n3ds_irc_close(irc);
        return 1;
    }
    if (st != want) {
        printf("expected open failure %d, got %d (%s)\n", want, st,
               n3ds_irc_strstatus(st));
        return 1;
    }
    printf("open failure as expected: %s (%d)\n", n3ds_irc_strstatus(st), st);
    return 0;
}

static int mode_expect_load(const char *path, uint32_t addr, int want) {
    int st = 0;
    N3dsIrc *irc = n3ds_irc_open_ex(path, &st);
    if (!irc) { printf("open failed %d\n", st); return 1; }
    if (n3ds_irc_cache_init(irc, 4u << 20, 64) != N3DS_IRC_OK) return 1;
    int r1 = n3ds_irc_pin(irc, addr);
    int r2 = n3ds_irc_pin(irc, addr);
    N3dsIrcStats s;
    n3ds_irc_stats(irc, &s);
    printf("load rc %d/%d, short %llu crc %llu io %llu attempts %llu loads %llu\n",
           r1, r2, (unsigned long long)s.short_reads,
           (unsigned long long)s.crc_errors, (unsigned long long)s.io_errors,
           (unsigned long long)s.load_attempts, (unsigned long long)s.loads);
    int ok = r1 == want && r2 == want;
    if (want == N3DS_IRC_ERR_RECORD_SHORT && s.short_reads != 2) ok = 0;
    if (want == N3DS_IRC_ERR_RECORD_CRC && s.crc_errors != 2) ok = 0;
    if (want == N3DS_IRC_ERR_UNDECODED && s.invalid_entries < 1) ok = 0;
    n3ds_irc_close(irc);
    return ok ? 0 : 1;
}

static int mode_short(const char *path, uint32_t addr) {
    int st = 0;
    N3dsIrc *irc = n3ds_irc_open_ex(path, &st);
    if (!irc) { printf("open failed %d\n", st); return 1; }
    N3dsIrcEntry e;
    if (n3ds_irc_lookup(irc, addr, &e) != N3DS_IRC_OK) {
        printf("lookup failed\n");
        return 1;
    }
    /* stdio may already hold the first 4 KiB in its read buffer. Pick a record
     * beyond that so truncation is observed by the next fread. */
    if (e.record_offset < 8192) {
        unsigned n = n3ds_irc_function_count(irc);
        N3dsIrcEntry best = e;
        for (unsigned i = 0; i < n; ++i) {
            N3dsIrcEntry c;
            if (n3ds_irc_entry_at(irc, i, &c)) continue;
            if (!(c.flags & N3DS_IRC_FN_DECODED)) continue;
            if (c.record_offset > best.record_offset) best = c;
        }
        e = best;
    }
    if (n3ds_irc_cache_init(irc, 4u << 20, 64) != N3DS_IRC_OK) return 1;
#ifdef _WIN32
    FILE *t = fopen(path, "rb+");
    if (!t) { perror(path); return 1; }
    long new_size = (long)(e.record_offset + e.record_size / 2);
    printf("truncate %s to %ld (record off %u size %u)\n", path, new_size,
           e.record_offset, e.record_size);
    if (_chsize_s(_fileno(t), new_size)) { perror("_chsize_s"); fclose(t); return 1; }
    fclose(t);
    FILE *chk = fopen(path, "rb");
    if (chk) {
        fseek(chk, 0, SEEK_END);
        printf("new file size %ld\n", ftell(chk));
        fclose(chk);
    }
#else
    if (truncate(path, (off_t)(e.record_offset + e.record_size / 2))) return 1;
#endif
    int r1 = n3ds_irc_pin(irc, e.addr);
    int r2 = n3ds_irc_pin(irc, e.addr);
    N3dsIrcStats s;
    n3ds_irc_stats(irc, &s);
    printf("short rc %d/%d, short_reads %llu\n", r1, r2,
           (unsigned long long)s.short_reads);
    int ok = r1 == N3DS_IRC_ERR_RECORD_SHORT && r2 == N3DS_IRC_ERR_RECORD_SHORT &&
             s.short_reads == 2 && s.loads == 0;
    n3ds_irc_close(irc);
    return ok ? 0 : 1;
}

static int mode_cache(const char *path) {
    int st = 0;
    N3dsIrc *irc = n3ds_irc_open_ex(path, &st);
    if (!irc) { printf("open failed %d\n", st); return 1; }
    N3dsIrcEntry e[3];
    for (unsigned i = 0; i < 3; ++i)
        if (find_first_decoded(irc, i, &e[i])) { printf("need 3 decoded entries\n"); return 1; }
    if (n3ds_irc_cache_init(irc, 1u << 20, 2) != N3DS_IRC_OK) return 1;

    int r0 = n3ds_irc_pin(irc, e[0].addr);
    int r1 = n3ds_irc_pin(irc, e[1].addr);
    int r2 = n3ds_irc_pin(irc, e[2].addr);
    if (r0 || r1 || r2 != N3DS_IRC_ERR_CACHE_PINNED) {
        printf("pinned-full rc %d/%d/%d\n", r0, r1, r2);
        return 1;
    }
    if (n3ds_irc_is_loaded(irc, e[2].addr)) { printf("loaded while pinned-full\n"); return 1; }

    if (n3ds_irc_unpin(irc, e[0].addr) != N3DS_IRC_OK) return 1;
    if (n3ds_irc_pin(irc, e[2].addr) != N3DS_IRC_OK) {
        printf("pin after unpin failed\n");
        return 1;
    }
    if (n3ds_irc_is_loaded(irc, e[0].addr) || !n3ds_irc_is_loaded(irc, e[1].addr) ||
        !n3ds_irc_is_loaded(irc, e[2].addr)) {
        printf("LRU evicted the wrong record\n");
        return 1;
    }
    N3dsIrcStats s;
    n3ds_irc_stats(irc, &s);
    if (s.evictions != 1 || s.evicted_bytes != e[0].record_size || s.loads != 3) {
        printf("eviction stats: evictions %llu bytes %llu loads %llu\n",
               (unsigned long long)s.evictions, (unsigned long long)s.evicted_bytes,
               (unsigned long long)s.loads);
        return 1;
    }

    /* Re-pin an already loaded record: cache hit, no second load. */
    if (n3ds_irc_pin(irc, e[1].addr) != N3DS_IRC_OK) return 1;
    n3ds_irc_stats(irc, &s);
    if (s.loads != 3 || s.cache_hits != 1) {
        printf("hit stats: loads %llu hits %llu\n",
               (unsigned long long)s.loads, (unsigned long long)s.cache_hits);
        return 1;
    }
    if (s.cache_records != 2 ||
        s.cache_bytes != (uint64_t)e[1].record_size + e[2].record_size) {
        printf("resident stats: records %llu bytes %llu\n",
               (unsigned long long)s.cache_records,
               (unsigned long long)s.cache_bytes);
        return 1;
    }
    if (n3ds_irc_unpin(irc, e[1].addr) || n3ds_irc_unpin(irc, e[1].addr) ||
        n3ds_irc_unpin(irc, e[2].addr)) return 1;

    /* Clear drops loaded records but not the index. */
    n3ds_irc_cache_clear(irc);
    if (n3ds_irc_is_loaded(irc, e[1].addr)) { printf("clear kept a record\n"); return 1; }
    if (n3ds_irc_lookup(irc, e[1].addr, &e[1]) != N3DS_IRC_OK) return 1;

    printf("cache ok: loads %llu hits %llu misses %llu evictions %llu load_bytes %llu\n",
           (unsigned long long)s.loads, (unsigned long long)s.cache_hits,
           (unsigned long long)s.cache_misses, (unsigned long long)s.evictions,
           (unsigned long long)s.load_bytes);
    n3ds_irc_close(irc);
    return 0;
}

static int mode_verify_all(const char *path) {
    int st = 0;
    N3dsIrc *irc = n3ds_irc_open_ex(path, &st);
    if (!irc) { printf("open failed %d\n", st); return 1; }
    unsigned n = n3ds_irc_function_count(irc);
    unsigned decoded = 0, loaded = 0, bad = 0;
    if (n3ds_irc_cache_init(irc, 8u << 20, 512) != N3DS_IRC_OK) return 1;
    for (unsigned i = 0; i < n; ++i) {
        N3dsIrcEntry e;
        if (n3ds_irc_entry_at(irc, i, &e)) { bad++; continue; }
        if (!(e.flags & N3DS_IRC_FN_DECODED)) continue;
        decoded++;
        int rc = n3ds_irc_pin(irc, e.addr);
        if (rc != N3DS_IRC_OK) {
            if (bad < 8)
                printf("load failed %08x: %s (%d)\n", e.addr,
                       n3ds_irc_strstatus(rc), rc);
            bad++;
            continue;
        }
        if (n3ds_irc_unpin(irc, e.addr) != N3DS_IRC_OK) { bad++; continue; }
        loaded++;
    }
    N3dsIrcStats s;
    n3ds_irc_stats(irc, &s);
    printf("verify-all: %u functions, %u decoded, %u loaded, %u bad, "
           "loads %llu bytes %llu load_ns %llu evictions %llu short %llu crc %llu record %llu\n",
           n, decoded, loaded, bad, (unsigned long long)s.loads,
           (unsigned long long)s.load_bytes, (unsigned long long)s.load_ns,
           (unsigned long long)s.evictions,
           (unsigned long long)s.short_reads, (unsigned long long)s.crc_errors,
           (unsigned long long)s.record_errors);
    n3ds_irc_close(irc);
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s MODE PATH [args]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    if (!strcmp(mode, "open")) return mode_open(argv[2]);
    if (!strcmp(mode, "verify-all")) return mode_verify_all(argv[2]);
    if (!strcmp(mode, "expect-open") && argc >= 4)
        return mode_expect_open(argv[2], atoi(argv[3]));
    if (!strcmp(mode, "expect-load") && argc >= 5)
        return mode_expect_load(argv[2], (uint32_t)strtoul(argv[3], NULL, 16), atoi(argv[4]));
    if (!strcmp(mode, "short") && argc >= 4)
        return mode_short(argv[2], (uint32_t)strtoul(argv[3], NULL, 16));
    if (!strcmp(mode, "cache")) return mode_cache(argv[2]);
    fprintf(stderr, "bad mode/args\n");
    return 2;
}
