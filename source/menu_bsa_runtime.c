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

const char *menu_swf_format_name(MenuSwfFormat format) {
    switch (format) {
    case MENU_SWF_FWS: return "FWS";
    case MENU_SWF_CWS: return "CWS";
    case MENU_SWF_ZWS: return "ZWS";
    default: return "unknown";
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

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

BsaStatus menu_swf_header_parse(const uint8_t *data, size_t available,
                                uint32_t file_bytes, MenuSwfHeader *out,
                                char *reason, size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!data || !out) {
        set_reason(reason, reason_cap,
                   "swf: header parse needs data and an output header");
        return BSA_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    if (available < 8 || file_bytes < 8) {
        set_reason(reason, reason_cap, "swf: header is shorter than 8 bytes");
        return BSA_ERR_TRUNCATED;
    }
    memcpy(out->signature, data, 3);
    out->file_bytes = file_bytes;
    out->version = data[3];
    out->declared_bytes = get_u32(data + 4);
    if (!memcmp(data, "FWS", 3)) {
        out->format = MENU_SWF_FWS;
    } else if (!memcmp(data, "CWS", 3)) {
        out->format = MENU_SWF_CWS;
    } else if (!memcmp(data, "ZWS", 3)) {
        out->format = MENU_SWF_ZWS;
        set_reason(reason, reason_cap,
                   "swf: ZWS (LZMA) compression is unsupported");
        return BSA_ERR_UNSUPPORTED;
    } else {
        out->format = MENU_SWF_UNKNOWN;
        set_reason(reason, reason_cap, "swf: signature %.3s is not FWS/CWS/ZWS",
                   (const char *)data);
        return BSA_ERR_UNSUPPORTED;
    }
    if (out->version == 0 || out->version > 32) {
        set_reason(reason, reason_cap, "swf: version %u is unsupported",
                   out->version);
        return BSA_ERR_UNSUPPORTED;
    }
    if (out->declared_bytes < 8) {
        set_reason(reason, reason_cap,
                   "swf: declared size %u is smaller than the header",
                   out->declared_bytes);
        return BSA_ERR_FORMAT;
    }
    if (out->format == MENU_SWF_FWS) {
        if (out->declared_bytes != file_bytes) {
            set_reason(reason, reason_cap,
                       "swf: FWS declares %u bytes but stores %u",
                       out->declared_bytes, file_bytes);
            return BSA_ERR_FORMAT;
        }
        return BSA_OK;
    }
    /* CWS: the body is zlib.  Only the two-byte zlib prefix is inspected; the
     * GFx body is deliberately not decompressed here. */
    if (available < 10 || file_bytes < 10) {
        set_reason(reason, reason_cap, "swf: CWS body lacks a zlib prefix");
        return BSA_ERR_TRUNCATED;
    }
    unsigned cmf = data[8], flg = data[9];
    if ((cmf & 0x0f) != 8 || ((cmf << 8) | flg) % 31 != 0) {
        set_reason(reason, reason_cap, "swf: CWS zlib header is invalid");
        return BSA_ERR_FORMAT;
    }
    if (flg & 0x20) {
        set_reason(reason, reason_cap,
                   "swf: CWS preset dictionary is unsupported");
        return BSA_ERR_UNSUPPORTED;
    }
    return BSA_OK;
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

MenuBsaStatus menu_bsa_resolve(GuestHeap *heap,
                               const MenuBsaSource *sources, size_t source_count,
                               const char *virtual_path, uint32_t budget_bytes,
                               const BsaOptions *options, MenuBsaResult *out) {
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
            continue; /* absent optional archive, e.g. Update.bsa */
        }
        if (st != BSA_OK) {
            result_fail(out, map_archive_status(st, MENU_BSA_CORRUPT),
                        "archive '%s' (%s): %s: %s", name, src->path,
                        bsa_status_name(st), archive_reason);
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

MenuBsaStatus menu_bsa_resolve_startmenu(GuestHeap *heap,
                                         const MenuBsaSource *sources,
                                         size_t source_count,
                                         uint32_t budget_bytes,
                                         MenuBsaResult *out) {
    return menu_bsa_resolve(heap, sources, source_count,
                            "Interface\\StartMenu.swf", budget_bytes, NULL, out);
}
