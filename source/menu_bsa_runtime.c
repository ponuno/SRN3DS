#include "menu_bsa_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const menu_bsa_archive_names[MENU_BSA_ARCHIVE_COUNT] = {
    "Skyrim - Interface.bsa",
    "Update.bsa"
};

const char *menu_bsa_status_name(MenuBsaStatus status) {
    switch (status) {
    case MENU_BSA_OK: return "ok";
    case MENU_BSA_MISSING: return "missing";
    case MENU_BSA_ACCESS: return "archive access error";
    case MENU_BSA_CORRUPT: return "corrupt";
    case MENU_BSA_UNSUPPORTED: return "unsupported";
    case MENU_BSA_BUDGET: return "over budget";
    case MENU_BSA_GUEST_MEMORY: return "guest memory exhausted";
    case MENU_BSA_BAD_ARGUMENT: return "bad argument";
    default: return "unknown menu BSA status";
    }
}

static void set_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static MenuBsaStatus map_archive_status(BsaStatus st, MenuBsaStatus fallback) {
    if (st == BSA_ERR_BUDGET) return MENU_BSA_BUDGET;
    if (st == BSA_ERR_ACCESS || st == BSA_ERR_IO) return MENU_BSA_ACCESS;
    return fallback;
}

int menu_bsa_sources_init(MenuBsaSources *out, const char *data_dir,
                          char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out || !data_dir || !data_dir[0]) {
        set_reason(reason, reason_cap, "data directory is empty");
        return 0;
    }
    out->count = MENU_BSA_ARCHIVE_COUNT;
    out->total_index_budget = MENU_BSA_DEFAULT_TOTAL_INDEX_BYTES;
    out->skip_unusable = 0;
    size_t len = strlen(data_dir);
    while (len > 1 && (data_dir[len - 1] == '/' || data_dir[len - 1] == '\\')) --len;
    for (unsigned i = 0; i < MENU_BSA_ARCHIVE_COUNT; ++i) {
        int n = snprintf(out->storage[i], sizeof out->storage[i], "%.*s/%s",
                         (int)len, data_dir, menu_bsa_archive_names[i]);
        if (n < 0 || (size_t)n >= sizeof out->storage[i]) {
            set_reason(reason, reason_cap, "archive path is too long");
            return 0;
        }
        out->source[i].path = out->storage[i];
        out->source[i].name = menu_bsa_archive_names[i];
    }
    return 1;
}

/* ---- Skyrim.ini [Archive] list ------------------------------------------- */

#define MENU_BSA_INI_MAX_BYTES (1024u * 1024u)

static int ascii_ci_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

static int ascii_ci_eq(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    return na == nb && ascii_ci_eq_n(a, b, na);
}

static char *span_trim(char *s, size_t *len) {
    while (*len && (*s == ' ' || *s == '\t' || *s == '\r')) { ++s; --*len; }
    while (*len && (s[*len - 1] == ' ' || s[*len - 1] == '\t' ||
                    s[*len - 1] == '\r')) --*len;
    s[*len] = 0;
    return s;
}

static int ini_archive_section(const char *line) {
    if (line[0] != '[') return 0;
    const char *end = strchr(line + 1, ']');
    if (!end) return 0;
    size_t n = (size_t)(end - (line + 1));
    return n == 7 && ascii_ci_eq_n(line + 1, "Archive", n);
}

static int ini_append_name(MenuBsaSources *out, const char *data_dir,
                           size_t dir_len, char *token,
                           char *reason, size_t reason_cap) {
    size_t len = strlen(token);
    token = span_trim(token, &len);
    if (len >= 2 && token[0] == '"' && token[len - 1] == '"') {
        ++token;
        len -= 2;
        token = span_trim(token, &len);
    }
    if (!len) return 1;
    if (len < 4 || !ascii_ci_eq_n(token + len - 4, ".bsa", 4)) {
        fprintf(stderr, "[menu-bsa] ignoring non-.bsa entry '%s' in Skyrim.ini\n",
                token);
        return 1;
    }
    if (out->count >= MENU_BSA_MAX_SOURCES) {
        set_reason(reason, reason_cap,
                   "Skyrim.ini lists more than %u archives (at '%s'); refusing "
                   "instead of truncating the mount list",
                   MENU_BSA_MAX_SOURCES, token);
        return 0;
    }
    char *storage = out->storage[out->count];
    int n = snprintf(storage, MENU_BSA_DATA_PATH_MAX, "%.*s/%.*s",
                     (int)dir_len, data_dir, (int)len, token);
    if (n < 0 || (size_t)n >= MENU_BSA_DATA_PATH_MAX) {
        set_reason(reason, reason_cap,
                   "archive path for '%s' exceeds %u bytes",
                   token, MENU_BSA_DATA_PATH_MAX);
        return 0;
    }
    out->source[out->count].path = storage;
    out->source[out->count].name = storage + dir_len + 1;
    ++out->count;
    return 1;
}

/* Scan one key across the whole file, preserving file order within that key. */
static int ini_collect_list(MenuBsaSources *out, const char *data_dir,
                            size_t dir_len, char *buf, const char *wanted,
                            char *reason, size_t reason_cap) {
    int in_archive = 0;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        size_t line_len = strlen(p);
        char *line = span_trim(p, &line_len);
        if (*line == ';' || *line == '#' || !*line) {
            /* comment or blank */
        } else if (*line == '[') {
            in_archive = ini_archive_section(line);
        } else if (in_archive && strchr(line, '=')) {
            char *eq = strchr(line, '=');
            *eq = 0;
            size_t key_len = strlen(line);
            char *key = span_trim(line, &key_len);
            if (ascii_ci_eq(key, wanted)) {
                char *value = eq + 1;
                size_t value_len = strlen(value);
                value = span_trim(value, &value_len);
                if (value_len >= 2 && value[0] == '"' &&
                    value[value_len - 1] == '"') {
                    ++value;
                    value_len -= 2;
                    value = span_trim(value, &value_len);
                }
                char *seg = value;
                for (;;) {
                    char *comma = strchr(seg, ',');
                    if (comma) *comma = 0;
                    if (!ini_append_name(out, data_dir, dir_len, seg,
                                         reason, reason_cap))
                        return 0;
                    if (!comma) break;
                    seg = comma + 1;
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 1;
}

int menu_bsa_sources_from_ini(MenuBsaSources *out, const char *data_dir,
                              const char *ini_path,
                              char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out || !data_dir || !data_dir[0]) {
        set_reason(reason, reason_cap, "data directory is empty");
        return 0;
    }
    out->count = 0;
    out->total_index_budget = MENU_BSA_DEFAULT_TOTAL_INDEX_BYTES;
    out->skip_unusable = 1;

    char *buf = NULL;
    FILE *f = ini_path && ini_path[0] ? fopen(ini_path, "rb") : NULL;
    if (f) {
        buf = malloc(MENU_BSA_INI_MAX_BYTES);
        if (buf) {
            size_t n = fread(buf, 1, MENU_BSA_INI_MAX_BYTES - 1, f);
            buf[n] = 0;
            /* Skip a UTF-8 BOM if the file was edited on a desktop. */
            if (n >= 3 && (unsigned char)buf[0] == 0xef &&
                (unsigned char)buf[1] == 0xbb && (unsigned char)buf[2] == 0xbf)
                memmove(buf, buf + 3, n - 2);
        }
        fclose(f);
    }

    size_t dir_len = strlen(data_dir);
    while (dir_len > 1 && (data_dir[dir_len - 1] == '/' ||
                           data_dir[dir_len - 1] == '\\'))
        --dir_len;

    int ok = 1;
    if (buf) {
        size_t buf_len = strlen(buf);
        char *work = malloc(buf_len + 1);
        if (!work) {
            free(buf);
            set_reason(reason, reason_cap, "out of memory while reading the INI");
            return 0;
        }
        /* Each pass mutates its own copy: List is collected fully before List2
         * even if the two keys appear in the opposite order in the file. */
        memcpy(work, buf, buf_len + 1);
        ok = ini_collect_list(out, data_dir, dir_len, work,
                              "sResourceArchiveList", reason, reason_cap);
        if (ok) {
            memcpy(work, buf, buf_len + 1);
            ok = ini_collect_list(out, data_dir, dir_len, work,
                                  "sResourceArchiveList2", reason, reason_cap);
        }
        free(work);
        free(buf);
    }
    if (!ok) return 0;

    if (!out->count) {
        /* No INI, no readable list or only empty elements: keep the measured
         * Interface/Update default so existing boots do not regress. */
        return menu_bsa_sources_init(out, data_dir, reason, reason_cap);
    }
    return 1;
}

int menu_bsa_sources_from_boot(MenuBsaSources *out, const char *root,
                               char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out || !root || !root[0]) {
        set_reason(reason, reason_cap, "game root is empty");
        return 0;
    }
    size_t root_len = strlen(root);
    while (root_len > 1 && (root[root_len - 1] == '/' ||
                            root[root_len - 1] == '\\'))
        --root_len;

    char data_dir[MENU_BSA_DATA_PATH_MAX];
    int n = snprintf(data_dir, sizeof data_dir, "%.*s/Data",
                     (int)root_len, root);
    if (n < 0 || (size_t)n >= sizeof data_dir) {
        set_reason(reason, reason_cap, "game root path is too long");
        return 0;
    }

    const char *suffixes[] = {"/Skyrim.ini", "/Data/Skyrim.ini"};
    for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; ++i) {
        char ini_path[MENU_BSA_DATA_PATH_MAX];
        n = snprintf(ini_path, sizeof ini_path, "%.*s%s", (int)root_len, root,
                     suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof ini_path) continue;
        FILE *probe = fopen(ini_path, "rb");
        if (!probe) continue;
        fclose(probe);
        if (!menu_bsa_sources_from_ini(out, data_dir, ini_path,
                                       reason, reason_cap))
            return 0;
        if (out->skip_unusable) return 1; /* non-empty configured list */
        /* The file exists but has no archive names; prefer the cooked INI in
         * Data over silently falling back. */
    }
    return menu_bsa_sources_init(out, data_dir, reason, reason_cap);
}

void menu_bsa_sources_log(const MenuBsaSources *set, FILE *log) {    if (!set || !log) return;
    if (!set->skip_unusable) {
        fprintf(log, "[menu-bsa] fallback: Interface+Update\n");
        return;
    }
    fprintf(log, "[menu-bsa] %u archive(s) from Skyrim.ini: ",
            (unsigned)set->count);
    for (size_t i = 0; i < set->count; ++i) {
        const char *name = set->source[i].name && set->source[i].name[0]
                               ? set->source[i].name
                               : set->source[i].path;
        fprintf(log, "%s%s", i ? ", " : "", name ? name : "<unnamed>");
    }
    fputc('\n', log);
}

static void result_fail(MenuBsaResult *out, MenuBsaStatus status,
                        const char *fmt, ...) {
    if (!out) return;
    out->status = status;
    out->guest_address = 0;
    out->size = 0;
    out->archive_index = 0;
    out->stored_size = 0;
    out->crc32 = 0;
    out->archive_name[0] = 0;
    memset(&out->swf, 0, sizeof out->swf);
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->reason, sizeof out->reason, fmt, ap);
    va_end(ap);
    out->reason[sizeof out->reason - 1] = 0;
}

MenuBsaStatus menu_bsa_resolve_ex(GuestHeap *heap,
                                  const MenuBsaSource *sources,
                                  size_t source_count,
                                  const char *virtual_path,
                                  uint32_t budget_bytes,
                                  const BsaOptions *options,
                                  uint64_t total_index_budget,
                                  int skip_unusable, MenuBsaResult *out) {
    if (!out) return MENU_BSA_BAD_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->status = MENU_BSA_BAD_ARGUMENT;
    if (!heap || !heap->memory || !sources || !source_count ||
        source_count > MENU_BSA_MAX_SOURCES || !virtual_path || !budget_bytes) {
        result_fail(out, MENU_BSA_BAD_ARGUMENT,
                    "resolver needs a heap, at most %u sources and a non-zero budget",
                    MENU_BSA_MAX_SOURCES);
        return out->status;
    }
    if (strlen(virtual_path) >= BSA_PATH_MAX) {
        result_fail(out, MENU_BSA_BAD_ARGUMENT,
                    "virtual path is longer than %u bytes", BSA_PATH_MAX - 1);
        return out->status;
    }
    snprintf(out->virtual_path, sizeof out->virtual_path, "%s", virtual_path);
    if (!total_index_budget) total_index_budget = MENU_BSA_DEFAULT_TOTAL_INDEX_BYTES;

    BsaOptions opt;
    bsa_options_default(&opt);
    if (options) {
        if (options->max_folders) opt.max_folders = options->max_folders;
        if (options->max_files) opt.max_files = options->max_files;
        if (options->max_name_bytes) opt.max_name_bytes = options->max_name_bytes;
        if (options->max_index_bytes) opt.max_index_bytes = options->max_index_bytes;
        if (options->max_stored_bytes) opt.max_stored_bytes = options->max_stored_bytes;
        if (options->max_unpacked_bytes) opt.max_unpacked_bytes = options->max_unpacked_bytes;
        opt.verify_embedded_names = options->verify_embedded_names;
    }
    if (opt.max_unpacked_bytes > budget_bytes) opt.max_unpacked_bytes = budget_bytes;
    if (opt.max_stored_bytes > budget_bytes) opt.max_stored_bytes = budget_bytes;

    BsaArchive *archives[MENU_BSA_MAX_SOURCES];
    memset(archives, 0, sizeof archives);
    uint64_t index_total = 0;
    int winner = -1;
    BsaMatch match;
    memset(&match, 0, sizeof match);

    for (size_t i = 0; i < source_count; ++i) {
        const MenuBsaSource *src = &sources[i];
        const char *name = (src->name && src->name[0]) ? src->name : src->path;
        char archive_reason[BSA_REASON_MAX] = "";
        if (!src->path || !src->path[0]) {
            result_fail(out, MENU_BSA_MISSING, "source %u has no path", (unsigned)i);
            goto done;
        }
        BsaStatus st = bsa_open_path(src->path, &opt, &archives[i],
                                     archive_reason, sizeof archive_reason);
        if (st == BSA_ERR_NOT_FOUND) {
            fprintf(stderr, "[menu-bsa] skip missing archive '%s' (%s)\n",
                    name ? name : "<unnamed>", src->path);
            continue; /* absent optional archive, e.g. Update.bsa */
        }
        if (st != BSA_OK) {
            if (skip_unusable) {
                fprintf(stderr,
                        "[menu-bsa] skip unusable archive '%s' (%s): %s: %s\n",
                        name ? name : "<unnamed>", src->path,
                        bsa_status_name(st), archive_reason);
                continue;
            }
            result_fail(out, map_archive_status(st, MENU_BSA_CORRUPT),
                        "archive '%s' (%s): %s: %s", name, src->path,
                        bsa_status_name(st), archive_reason);
            goto done;
        }
        index_total += bsa_index_bytes(archives[i]);
        if (index_total > total_index_budget) {
            result_fail(out, MENU_BSA_BUDGET,
                        "archive index budget exceeded: '%s' brings the mounted "
                        "total to %llu bytes, limit is %llu",
                        name ? name : src->path,
                        (unsigned long long)index_total,
                        (unsigned long long)total_index_budget);
            goto done;
        }
        st = bsa_find(archives[i], virtual_path, &match);
        if (st == BSA_ERR_NOT_FOUND) continue;
        if (st != BSA_OK) {
            result_fail(out, map_archive_status(st, MENU_BSA_CORRUPT),
                        "archive '%s': lookup of '%s': %s", name,
                        virtual_path, bsa_status_name(st));
            goto done;
        }
        winner = (int)i;
    }

    if (winner < 0) {
        char searched[MENU_BSA_REASON_MAX];
        size_t at = 0;
        searched[0] = 0;
        for (size_t i = 0; i < source_count && at + 1 < sizeof searched; ++i) {
            const char *name = (sources[i].name && sources[i].name[0])
                                   ? sources[i].name : sources[i].path;
            int n = snprintf(searched + at, sizeof searched - at, "%s%s",
                             i ? ", " : "", name ? name : "<unnamed>");
            if (n < 0) break;
            at += (size_t)n;
            if (at >= sizeof searched) { at = sizeof searched - 1; break; }
        }
        result_fail(out, MENU_BSA_MISSING,
                    "no source contains '%s' (searched: %s)",
                    virtual_path, searched);
        goto done;
    }

    {
        const MenuBsaSource *src = &sources[winner];
        const char *name = (src->name && src->name[0]) ? src->name : src->path;
        char entry_reason[BSA_REASON_MAX] = "";
        uint32_t unpacked = 0;
        BsaStatus st = bsa_unpacked_size(archives[winner], &match.entry,
                                         &unpacked, entry_reason,
                                         sizeof entry_reason);
        if (st == BSA_ERR_BUDGET) {
            result_fail(out, MENU_BSA_BUDGET,
                        "'%s' in %s: %s", virtual_path, name, entry_reason);
            goto done;
        }
        if (st != BSA_OK) {
            result_fail(out, map_archive_status(st, MENU_BSA_CORRUPT),
                        "'%s' in %s: %s: %s", virtual_path, name,
                        bsa_status_name(st), entry_reason);
            goto done;
        }
        if (unpacked > budget_bytes) {
            result_fail(out, MENU_BSA_BUDGET,
                        "'%s' is %u bytes, budget is %u", virtual_path,
                        unpacked, budget_bytes);
            goto done;
        }

        uint8_t prefix[16];
        size_t prefix_len = 0;
        size_t want = unpacked < sizeof prefix ? unpacked : sizeof prefix;
        st = bsa_read_prefix(archives[winner], &match.entry, prefix, want,
                             &prefix_len, entry_reason, sizeof entry_reason);
        if (st != BSA_OK) {
            result_fail(out, map_archive_status(st, MENU_BSA_CORRUPT),
                        "'%s' prefix: %s: %s", virtual_path,
                        bsa_status_name(st), entry_reason);
            goto done;
        }
        st = menu_swf_header_parse(prefix, prefix_len, unpacked, &out->swf,
                                   entry_reason, sizeof entry_reason);
        if (st != BSA_OK) {
            result_fail(out, st == BSA_ERR_UNSUPPORTED ? MENU_BSA_UNSUPPORTED
                                                       : map_archive_status(st, MENU_BSA_CORRUPT),
                        "'%s': %s", virtual_path, entry_reason);
            goto done;
        }

        uint32_t addr = heap_alloc(heap, unpacked);
        if (!addr) {
            result_fail(out, MENU_BSA_GUEST_MEMORY,
                        "guest heap cannot provide %u bytes for '%s'",
                        unpacked, virtual_path);
            goto done;
        }
        uint8_t *dst = (uint8_t *)guest_ptr(heap->memory, addr, unpacked);
        if (!dst) {
            heap_free(heap, addr);
            result_fail(out, MENU_BSA_GUEST_MEMORY,
                        "guest pages for '%s' could not be backed", virtual_path);
            goto done;
        }
        size_t got = 0;
        st = bsa_read(archives[winner], &match.entry, dst, unpacked, &got,
                      entry_reason, sizeof entry_reason);
        if (st != BSA_OK || got != unpacked) {
            heap_free(heap, addr);
            result_fail(out, map_archive_status(st, MENU_BSA_CORRUPT),
                        "'%s' read: %s: %s", virtual_path,
                        bsa_status_name(st), entry_reason);
            goto done;
        }
        out->status = MENU_BSA_OK;
        out->guest_address = addr;
        out->size = (uint32_t)got;
        out->archive_index = (uint32_t)winner;
        out->stored_size = match.entry.stored_size;
        out->crc32 = bsa_crc32(dst, got);
        snprintf(out->archive_name, sizeof out->archive_name, "%s", name);
        out->reason[0] = 0;
    }

done:
    for (size_t i = 0; i < source_count; ++i) {
        if (archives[i]) bsa_close(archives[i]);
    }
    return out->status;
}

MenuBsaStatus menu_bsa_resolve(GuestHeap *heap,
                               const MenuBsaSource *sources, size_t source_count,
                               const char *virtual_path, uint32_t budget_bytes,
                               const BsaOptions *options, MenuBsaResult *out) {
    return menu_bsa_resolve_ex(heap, sources, source_count, virtual_path,
                               budget_bytes, options,
                               MENU_BSA_DEFAULT_TOTAL_INDEX_BYTES, 0, out);
}

MenuBsaStatus menu_bsa_resolve_sources(GuestHeap *heap,
                                       const MenuBsaSources *set,
                                       const char *virtual_path,
                                       uint32_t budget_bytes,
                                       const BsaOptions *options,
                                       MenuBsaResult *out) {
    if (!set) return menu_bsa_resolve_ex(heap, NULL, 0, virtual_path,
                                         budget_bytes, options, 0, 0, out);
    return menu_bsa_resolve_ex(heap, set->source, set->count, virtual_path,
                               budget_bytes, options, set->total_index_budget,
                               set->skip_unusable, out);
}

MenuBsaStatus menu_bsa_resolve_startmenu(GuestHeap *heap,
                                         const MenuBsaSource *sources,
                                         size_t source_count,
                                         uint32_t budget_bytes,
                                         MenuBsaResult *out) {
    return menu_bsa_resolve(heap, sources, source_count,
                            "Interface\\StartMenu.swf", budget_bytes, NULL, out);
}

MenuBsaStatus menu_bsa_resolve_startmenu_sources(GuestHeap *heap,
                                                 const MenuBsaSources *set,
                                                 uint32_t budget_bytes,
                                                 MenuBsaResult *out) {
    return menu_bsa_resolve_sources(heap, set, "Interface\\StartMenu.swf",
                                    budget_bytes, NULL, out);
}
