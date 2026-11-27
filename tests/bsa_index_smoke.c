/* BSA directory index: memory footprint + lookup equivalence + timing.
 *
 * This test is read-only and only runs against a real install when one is
 * present (arg 1, TESV_DATA_ROOT / SKYRIM_DATA, or E:\harness_skyrim\Data).
 * It owns its own tiny directory parser so the query corpus does not depend on
 * the reader's private index.
 *
 * Reference protocol: the first run with no reference file dumps every
 * bsa_find result twice, to %TEMP%\bsa_ref.txt and %TEMP%\bsa_ref_<pid>.txt.
 * Later runs compare the generated stream byte-for-byte with bsa_ref.txt (or
 * BSA_INDEX_REF).  This is exactly the "before vs after" check: the old index
 * produced the reference, the new index must reproduce every hit/miss, path,
 * offset, stored size, hash and record id.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "bsa_reader.h"

#define MAX_ARCHIVES 64
#define MAX_PATH_BYTES 512
#define CORPUS_REAL 24000
#define CORPUS_MISS 2500
#define BENCH_FINDS 100000

typedef struct {
    char *name;
    char *full;
} ArchiveInfo;

typedef struct {
    char *text;
    int owner; /* -1 for a synthetic miss */
} Query;

static ArchiveInfo g_arch[MAX_ARCHIVES];
static int g_narch;

static Query *g_queries;
static size_t g_nqueries, g_capqueries;

static int g_failures;

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "bsa_index: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    ++g_failures;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "bsa_index: out of memory (%llu bytes)\n",
                (unsigned long long)n);
        exit(2);
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static int rd_at(FILE *f, uint64_t off, void *dst, size_t n) {
#ifdef _WIN32
    if (_fseeki64(f, (int64_t)off, SEEK_SET) != 0) return 0;
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return 0;
#endif
    return fread(dst, 1, n, f) == n;
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void add_query(const char *text, int owner) {
    if (g_nqueries == g_capqueries) {
        g_capqueries = g_capqueries ? g_capqueries * 2 : 1024;
        g_queries = (Query *)realloc(g_queries, g_capqueries * sizeof *g_queries);
        if (!g_queries) { fprintf(stderr, "bsa_index: oom\n"); exit(2); }
    }
    g_queries[g_nqueries].text = xstrdup(text);
    g_queries[g_nqueries].owner = owner;
    ++g_nqueries;
}

/* ------------------------------------------------------------------ */
/* Archive directory parser (query corpus only).                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char **paths;
    uint32_t files;
} ParsedArchive;

static char *join_path(const char *folder, const char *base) {
    size_t fl = strlen(folder), bl = strlen(base);
    char *out = (char *)xmalloc(fl + (fl ? 1u : 0u) + bl + 1u);
    if (fl) {
        memcpy(out, folder, fl);
        out[fl] = '\\';
        memcpy(out + fl + 1, base, bl + 1);
    } else {
        memcpy(out, base, bl + 1);
    }
    return out;
}

static int parse_archive(const ArchiveInfo *info, ParsedArchive *out) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(info->full, "rb");
    if (!f) return 0;
    uint8_t head[36];
    if (!rd_at(f, 0, head, sizeof head) || memcmp(head, "BSA\0", 4) != 0) {
        fclose(f);
        return 0;
    }
    uint32_t version = rd_u32(head + 4);
    uint32_t hoff = rd_u32(head + 8);
    uint32_t flags = rd_u32(head + 12);
    uint32_t folders = rd_u32(head + 16);
    uint32_t files = rd_u32(head + 20);
    uint32_t tfln = rd_u32(head + 28);
    if (version < 103 || version > 105 || folders > 65536u || files > 1000000u) {
        fclose(f);
        return 0;
    }
    int embedded = version >= 104 && (flags & 0x100u) != 0;
    uint8_t *recs = (uint8_t *)xmalloc((size_t)folders * 16u + 1u);
    if (folders && !rd_at(f, hoff, recs, (size_t)folders * 16u)) {
        free(recs); fclose(f); return 0;
    }
    uint32_t *counts = (uint32_t *)xmalloc((size_t)folders * sizeof(uint32_t) + 1u);
    uint64_t *rec_at = (uint64_t *)xmalloc((size_t)folders * sizeof(uint64_t) + 1u);
    char **fnames = (char **)xmalloc((size_t)folders * sizeof(char *) + 1u);
    uint64_t cursor = (uint64_t)hoff + (uint64_t)folders * 16u;
    int ok = 1;
    for (uint32_t i = 0; i < folders && ok; ++i) {
        counts[i] = rd_u32(recs + (size_t)i * 16u + 8u);
        uint8_t len;
        if (!rd_at(f, cursor, &len, 1) || !len) {
            ok = 0; break;
        }
        ++cursor;
        char raw[MAX_PATH_BYTES];
        if (!rd_at(f, cursor, raw, len)) { ok = 0; break; }
        cursor += len;
        uint32_t n = len;
        if (raw[n - 1] == 0) --n;
        raw[n] = 0;
        for (uint32_t k = 0; k < n; ++k)
            if (raw[k] == '/') raw[k] = '\\';
        fnames[i] = xstrdup(raw);
        rec_at[i] = cursor;
        cursor += (uint64_t)counts[i] * 16u;
    }
    char **paths = NULL;
    if (ok) {
        paths = (char **)xmalloc((size_t)files * sizeof(char *) + 1u);
        uint32_t fi = 0;
        if (tfln) {
            uint8_t *block = (uint8_t *)xmalloc(tfln);
            if (!rd_at(f, cursor, block, tfln)) {
                ok = 0;
            } else {
                uint32_t pos = 0;
                for (uint32_t i = 0; i < folders && ok; ++i) {
                    for (uint32_t j = 0; j < counts[i]; ++j, ++fi) {
                        uint32_t begin = pos;
                        while (pos < tfln && block[pos]) ++pos;
                        if (pos >= tfln || pos == begin) { ok = 0; break; }
                        block[pos] = 0;
                        paths[fi] = join_path(fnames[i],
                                              (const char *)block + begin);
                        ++pos;
                    }
                }
            }
            free(block);
        } else if (embedded) {
            for (uint32_t i = 0; i < folders && ok; ++i) {
                for (uint32_t j = 0; j < counts[i]; ++j, ++fi) {
                    uint8_t rec[16];
                    if (!rd_at(f, rec_at[i] + (uint64_t)j * 16u, rec, 16)) {
                        ok = 0; break;
                    }
                    uint32_t stored = rd_u32(rec + 8);
                    uint32_t off = rd_u32(rec + 12);
                    uint8_t nlen;
                    if (!stored || !rd_at(f, off, &nlen, 1) || !nlen ||
                        1u + nlen > stored) {
                        ok = 0; break;
                    }
                    char raw[MAX_PATH_BYTES];
                    if (!rd_at(f, (uint64_t)off + 1u, raw, nlen)) { ok = 0; break; }
                    uint32_t n = nlen;
                    if (raw[n - 1] == 0) --n;
                    raw[n] = 0;
                    char *p = xstrdup(raw);
                    for (uint32_t k = 0; k < n; ++k)
                        if (p[k] == '/') p[k] = '\\';
                    paths[fi] = p;
                }
            }
        } else {
            ok = 0;
        }
        if (ok && fi != files) ok = 0;
    }
    for (uint32_t i = 0; i < folders; ++i) free(fnames[i]);
    free(fnames);
    free(rec_at);
    free(counts);
    free(recs);
    fclose(f);
    if (!ok) {
        if (paths) {
            for (uint32_t i = 0; i < files; ++i) free(paths[i]);
            free(paths);
        }
        return 0;
    }
    out->paths = paths;
    out->files = files;
    return 1;
}

static int cmp_nocase(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
}

typedef struct {
    const char *p;
    int owner;
} PtrOwner;

static int cmp_ptr_owner(const void *pa, const void *pb) {
    const PtrOwner *a = (const PtrOwner *)pa;
    const PtrOwner *b = (const PtrOwner *)pb;
    return cmp_nocase(a->p, b->p);
}

/* ------------------------------------------------------------------ */
/* Reference stream.                                                   */
/* ------------------------------------------------------------------ */

static void result_line(char *buf, size_t cap, const char *arch_name,
                        const char *query, BsaStatus st, const BsaMatch *m) {
    if (st == BSA_OK) {
        snprintf(buf, cap, "%s\t%s\t%d\t%llu\t%u\t%u\t%u\t%u\t%s\n",
                 arch_name, query, (int)st,
                 (unsigned long long)m->entry.hash, m->entry.stored_size,
                 m->entry.offset, m->entry.folder_index,
                 m->entry.record_index, m->path);
    } else {
        snprintf(buf, cap, "%s\t%s\t%d\t0\t0\t0\t0\t0\t\n",
                 arch_name, query, (int)st);
    }
}

/* ------------------------------------------------------------------ */
/* Timing.                                                             */
/* ------------------------------------------------------------------ */

static uint64_t now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ------------------------------------------------------------------ */
/* Archive discovery.                                                  */
/* ------------------------------------------------------------------ */

static int cmp_archive_names(const void *pa, const void *pb) {
    const ArchiveInfo *a = (const ArchiveInfo *)pa;
    const ArchiveInfo *b = (const ArchiveInfo *)pb;
    return strcmp(a->name, b->name);
}

static int list_archives(const char *dir) {
#ifdef _WIN32
    char pattern[MAX_PATH_BYTES * 2];
    snprintf(pattern, sizeof pattern, "%s\\*.bsa", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            g_narch < MAX_ARCHIVES) {
            size_t n = strlen(fd.cFileName);
            if (n < 4 || cmp_nocase(fd.cFileName + n - 4, ".bsa") != 0) continue;
            g_arch[g_narch].name = xstrdup(fd.cFileName);
            size_t need = strlen(dir) + n + 2;
            g_arch[g_narch].full = (char *)xmalloc(need);
            snprintf(g_arch[g_narch].full, need, "%s/%s", dir, fd.cFileName);
            ++g_narch;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_narch < MAX_ARCHIVES) {
        size_t n = strlen(e->d_name);
        if (n < 4 || cmp_nocase(e->d_name + n - 4, ".bsa") != 0) continue;
        g_arch[g_narch].name = xstrdup(e->d_name);
        size_t need = strlen(dir) + n + 2;
        g_arch[g_narch].full = (char *)xmalloc(need);
        snprintf(g_arch[g_narch].full, need, "%s/%s", dir, e->d_name);
        ++g_narch;
    }
    closedir(d);
#endif
    qsort(g_arch, (size_t)g_narch, sizeof g_arch[0], cmp_archive_names);
    return g_narch;
}

/* ------------------------------------------------------------------ */
/* Corpus.                                                             */
/* ------------------------------------------------------------------ */

static void build_corpus(ParsedArchive *parsed) {
    size_t total = 0;
    for (int i = 0; i < g_narch; ++i) total += parsed[i].files;
    if (!total) return;
    size_t step = total / CORPUS_REAL;
    if (!step) step = 1;
    size_t sampled = 0, flat = 0;
    for (int i = 0; i < g_narch && sampled < CORPUS_REAL; ++i) {
        for (uint32_t j = 0; j < parsed[i].files && sampled < CORPUS_REAL;
             ++j, ++flat) {
            if (flat % step) continue;
            const char *p = parsed[i].paths[j];
            char variant[MAX_PATH_BYTES];
            if (flat % 7 == 0) {
                size_t n = strlen(p);
                if (n >= sizeof variant) n = sizeof variant - 1;
                for (size_t k = 0; k < n; ++k) {
                    char c = p[k];
                    variant[k] = (c >= 'a' && c <= 'z')
                                     ? (char)(c - 32) : c;
                }
                variant[n] = 0;
                add_query(variant, i);
            } else if (flat % 11 == 0) {
                size_t n = strlen(p);
                if (n >= sizeof variant) n = sizeof variant - 1;
                for (size_t k = 0; k < n; ++k)
                    variant[k] = p[k] == '\\' ? '/' : p[k];
                variant[n] = 0;
                add_query(variant, i);
            } else {
                add_query(p, i);
            }
            ++sampled;
        }
    }

    /* Cross-archive duplicates: same canonical path in more than one file. */
    PtrOwner *all = (PtrOwner *)xmalloc(total * sizeof *all);
    size_t at = 0;
    for (int i = 0; i < g_narch; ++i)
        for (uint32_t j = 0; j < parsed[i].files; ++j) {
            all[at].p = parsed[i].paths[j];
            all[at].owner = i;
            ++at;
        }
    PtrOwner *sorted = (PtrOwner *)xmalloc(total * sizeof *sorted);
    memcpy(sorted, all, total * sizeof *sorted);
    qsort(sorted, total, sizeof *sorted, cmp_ptr_owner);
    size_t dups = 0;
    for (size_t i = 1; i < total && dups < 3000; ++i) {
        if (cmp_nocase(sorted[i].p, sorted[i - 1].p) == 0) {
            add_query(sorted[i].p, sorted[i].owner);
            ++dups;
        }
    }
    free(sorted);
    free(all);

    for (int i = 0; i < CORPUS_MISS; ++i) {
        char miss[MAX_PATH_BYTES];
        snprintf(miss, sizeof miss, "__missing__\\bsa_index_%05d.nif", i);
        add_query(miss, -1);
    }
    add_query("", -1);
    add_query("\\", -1);
    add_query("/", -1);
    add_query(".\\Interface\\", -1);
    add_query("interface/startmenu.swf", -1);
    add_query("INTERFACE\\STARTMENU.SWF", -1);
}

/* ------------------------------------------------------------------ */
/* Main.                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    const char *data = NULL;
    if (argc > 1) data = argv[1];
    if (!data) data = getenv("TESV_DATA_ROOT");
    if (!data) data = getenv("SKYRIM_DATA");
    if (!data) data = "E:/harness_skyrim/Data";

    if (!list_archives(data)) {
        printf("bsa_index: skip (no archives under %s)\n", data);
        return 0;
    }

    ParsedArchive *parsed =
        (ParsedArchive *)xmalloc((size_t)g_narch * sizeof *parsed);
    for (int i = 0; i < g_narch; ++i) {
        if (!parse_archive(&g_arch[i], &parsed[i])) {
            printf("bsa_index: skip (cannot parse %s)\n", g_arch[i].full);
            return 0;
        }
    }
    build_corpus(parsed);

    BsaArchive *arch[MAX_ARCHIVES];
    memset(arch, 0, sizeof arch);
    size_t total_index = 0, total_files = 0, total_folders = 0;
    printf("%-28s %8s %8s %10s\n", "archive", "files", "folders", "index KiB");
    for (int i = 0; i < g_narch; ++i) {
        char reason[BSA_REASON_MAX];
        reason[0] = 0;
        BsaStatus st = bsa_open_path(g_arch[i].full, NULL, &arch[i], reason,
                                     sizeof reason);
        if (st != BSA_OK) {
            fail("open %s: %s (%s)", g_arch[i].name, bsa_status_name(st), reason);
            continue;
        }
        const BsaHeader *h = bsa_header(arch[i]);
        size_t idx = bsa_index_bytes(arch[i]);
        total_index += idx;
        total_files += h->file_count;
        total_folders += h->folder_count;
        printf("%-28s %8u %8u %10.1f\n", g_arch[i].name, h->file_count,
               h->folder_count, idx / 1024.0);
    }
    printf("%-28s %8llu %8llu %10.1f  (%.2f MiB index, %.2f MiB page cache)\n",
           "TOTAL", (unsigned long long)total_files,
           (unsigned long long)total_folders, total_index / 1024.0,
           total_index / (double)(1u << 20),
           (double)g_narch * 32.0 * 1024.0 / (double)(1u << 20));

    /* Reference dump/compare. */
    char ref_path[1024], pid_path[1024], tmpdir[1024];
    const char *env_ref = getenv("BSA_INDEX_REF");
    /* run_host_tests.ps1 redirects TEMP into the per-run directory; keep the
     * before/after reference in the user's real temp so a later run can find
     * the stream the old index produced.  BSA_INDEX_TMP overrides. */
    const char *tmp = getenv("BSA_INDEX_TMP");
    if (tmp && *tmp) {
        snprintf(tmpdir, sizeof tmpdir, "%s", tmp);
    } else {
#ifdef _WIN32
        const char *local = getenv("LOCALAPPDATA");
        if (local && *local)
            snprintf(tmpdir, sizeof tmpdir, "%s\\Temp", local);
        else
            snprintf(tmpdir, sizeof tmpdir, "%s", getenv("TEMP") ? getenv("TEMP") : ".");
#else
        snprintf(tmpdir, sizeof tmpdir, "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
#endif
    }
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    if (env_ref && *env_ref) {
        snprintf(ref_path, sizeof ref_path, "%s", env_ref);
    } else {
        snprintf(ref_path, sizeof ref_path, "%s/bsa_ref.txt", tmpdir);
    }
    snprintf(pid_path, sizeof pid_path, "%s/bsa_ref_%lu.txt", tmpdir, pid);

    FILE *ref = fopen(ref_path, "rb");
    int dumping = (ref == NULL);
    FILE *dump_pid = NULL;
    if (!dumping) {
        printf("bsa_index: comparing against %s\n", ref_path);
    } else {
        printf("bsa_index: no reference at %s, dumping one\n", ref_path);
        ref = fopen(ref_path, "wb");
        dump_pid = fopen(pid_path, "wb");
        if (!ref) {
            fail("cannot create %s", ref_path);
            return 1;
        }
    }

    size_t lines = 0, mismatches = 0;
    char expect[4096], got[4096];
    for (size_t q = 0; q < g_nqueries && (ref || dumping); ++q) {
        int owner = g_queries[q].owner;
        int targets[3];
        if (owner >= 0) {
            targets[0] = owner;
            targets[1] = (owner + 1) % g_narch;
            targets[2] = (owner + 3) % g_narch;
        } else {
            targets[0] = 0;
            targets[1] = g_narch / 2;
            targets[2] = g_narch - 1;
        }
        for (int t = 0; t < 3; ++t) {
            int ai = targets[t];
            BsaMatch m;
            BsaStatus st = BSA_ERR_ARGUMENT;
            if (arch[ai])
                st = bsa_find(arch[ai], g_queries[q].text, &m);
            result_line(got, sizeof got, g_arch[ai].name, g_queries[q].text,
                        st, &m);
            if (dumping) {
                fputs(got, ref);
                if (dump_pid) fputs(got, dump_pid);
            } else {
                if (!fgets(expect, sizeof expect, ref)) {
                    fail("reference ends early at line %llu",
                         (unsigned long long)(lines + 1));
                    mismatches++;
                    break;
                }
                if (strcmp(expect, got) != 0) {
                    if (mismatches < 8) {
                        fprintf(stderr,
                                "bsa_index: mismatch line %llu\n  ref: %s  got: %s",
                                (unsigned long long)(lines + 1), expect, got);
                    }
                    mismatches++;
                }
            }
            ++lines;
        }
    }
    if (ref) fclose(ref);
    if (dump_pid) fclose(dump_pid);
    if (dumping) {
        printf("bsa_index: dumped %llu result lines to %s and %s\n",
               (unsigned long long)lines, ref_path, pid_path);
        if (dump_pid) printf("bsa_index: %s\n", pid_path);
    } else {
        if (mismatches) {
            fail("%llu of %llu result lines differ from %s",
                 (unsigned long long)mismatches, (unsigned long long)lines,
                 ref_path);
        } else {
            printf("bsa_index: %llu result lines match %s\n",
                   (unsigned long long)lines, ref_path);
        }
    }

    const char *max_bytes = getenv("BSA_INDEX_MAX_BYTES");
    if (max_bytes && *max_bytes) {
        unsigned long long limit = strtoull(max_bytes, NULL, 0);
        if (total_index > limit) {
            fail("resident index %llu bytes exceeds BSA_INDEX_MAX_BYTES=%llu",
                 (unsigned long long)total_index, limit);
        } else {
            printf("bsa_index: resident index %llu <= %llu bytes\n",
                   (unsigned long long)total_index, limit);
        }
    }

    for (int i = 0; i < g_narch; ++i) {
        if (arch[i]) bsa_close(arch[i]);
        arch[i] = NULL;
    }

    /* Fresh open so the page cache starts cold for the timing run. */
    for (int i = 0; i < g_narch; ++i) {
        char reason[BSA_REASON_MAX];
        if (bsa_open_path(g_arch[i].full, NULL, &arch[i], reason,
                          sizeof reason) != BSA_OK)
            arch[i] = NULL;
    }
    unsigned long long hits = 0;
    uint64_t t0 = now_ns();
    for (int n = 0; n < BENCH_FINDS; ++n) {
        int ai = n % g_narch;
        if (!arch[ai]) continue;
        size_t qi = ((size_t)n * 2654435761u) % g_nqueries;
        BsaMatch m;
        if (bsa_find(arch[ai], g_queries[qi].text, &m) == BSA_OK) ++hits;
    }
    uint64_t dt = now_ns() - t0;
    printf("bsa_index: %d random-archive bsa_find: %.1f ms, %.0f ns/find, "
           "%llu hits\n", BENCH_FINDS, (double)dt / 1e6,
           (double)dt / BENCH_FINDS, hits);

    /* Hit-heavy worst case: same archive as the path, random order, so each
     * hit pays the exact-name verification read with no locality. */
    unsigned long long hits2 = 0;
    size_t nq_pos = 0;
    t0 = now_ns();
    for (int n = 0; n < BENCH_FINDS; ++n) {
        while (nq_pos < g_nqueries && g_queries[nq_pos].owner < 0) ++nq_pos;
        if (nq_pos >= g_nqueries) nq_pos = 0;
        int ai = g_queries[nq_pos].owner >= 0 ? g_queries[nq_pos].owner : 0;
        if (arch[ai] &&
            bsa_find(arch[ai], g_queries[nq_pos].text, &(BsaMatch){0}) == BSA_OK)
            ++hits2;
        ++nq_pos;
        while (nq_pos < g_nqueries && g_queries[nq_pos].owner < 0) ++nq_pos;
        if (nq_pos >= g_nqueries) nq_pos = 0;
    }
    dt = now_ns() - t0;
    printf("bsa_index: %d same-archive bsa_find: %.1f ms, %.0f ns/find, "
           "%llu hits\n", BENCH_FINDS, (double)dt / 1e6,
           (double)dt / BENCH_FINDS, hits2);

    for (int i = 0; i < g_narch; ++i) {
        if (arch[i]) bsa_close(arch[i]);
        free(parsed[i].paths ? parsed[i].paths : NULL);
        free(g_arch[i].name);
        free(g_arch[i].full);
    }
    for (size_t i = 0; i < g_nqueries; ++i) free(g_queries[i].text);
    free(g_queries);
    free(parsed);

    if (g_failures) {
        fprintf(stderr, "bsa_index: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("bsa_index: ok\n");
    return 0;
}
