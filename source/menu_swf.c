#include "menu_swf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_reason(char *reason, size_t cap, const char *fmt, ...) {
    if (!reason || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, cap, fmt, ap);
    va_end(ap);
    reason[cap - 1] = 0;
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *menu_swf_format_name(MenuSwfFormat format) {
    switch (format) {
    case MENU_SWF_FWS: return "FWS";
    case MENU_SWF_CWS: return "CWS";
    case MENU_SWF_ZWS: return "ZWS";
    default: return "unknown";
    }
}

const char *menu_swf_status_name(MenuSwfStatus status) {
    switch (status) {
    case MENU_SWF_OK: return "ok";
    case MENU_SWF_ERR_ARGUMENT: return "bad argument";
    case MENU_SWF_ERR_TRUNCATED: return "truncated";
    case MENU_SWF_ERR_FORMAT: return "format error";
    case MENU_SWF_ERR_VERSION: return "unsupported version";
    case MENU_SWF_ERR_ZLIB: return "zlib error";
    case MENU_SWF_ERR_TAG: return "tag stream error";
    case MENU_SWF_ERR_END: return "missing End tag";
    case MENU_SWF_ERR_BUDGET: return "over budget";
    case MENU_SWF_ERR_NOTIMPL: return "not implemented";
    default: return "unknown SWF status";
    }
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
                   "swf: ZWS (LZMA) compression is E_NOTIMPL");
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
    /* CWS: the body is zlib.  Only the two-byte zlib prefix is inspected here;
     * the summary decompresses it with the exact-stream helper. */
    if (available < 10 || file_bytes < 10) {
        set_reason(reason, reason_cap, "swf: CWS body lacks a zlib prefix");
        return BSA_ERR_TRUNCATED;
    }
    unsigned cmf = data[8], flg = data[9];
    if ((cmf & 0x0f) != 8 || ((cmf << 8) | flg) % 31 != 0) {
        set_reason(reason, reason_cap, "swf: CWS zlib header is invalid");
        return BSA_ERR_DEFLATE;
    }
    if (flg & 0x20) {
        set_reason(reason, reason_cap,
                   "swf: CWS preset dictionary is unsupported");
        return BSA_ERR_UNSUPPORTED;
    }
    return BSA_OK;
}

static void summary_clear(MenuSwfSummary *summary) {
    if (!summary) return;
    memset(summary, 0, sizeof *summary);
    summary->first_show_frame = -1;
}

static void add_name(MenuSwfSummary *s, uint16_t id, int kind,
                     const uint8_t *text, size_t len) {
    ++s->name_total;
    if (s->name_count >= MENU_SWF_MAX_NAMES) {
        s->names_truncated = 1;
        return;
    }
    MenuSwfName *name = &s->names[s->name_count++];
    name->id = id;
    name->kind = (uint8_t)kind;
    size_t copy = len < MENU_SWF_NAME_MAX - 1 ? len : MENU_SWF_NAME_MAX - 1;
    memcpy(name->name, text, copy);
    name->name[copy] = 0;
    if (copy != len) s->names_truncated = 1;
}

/* Read a NUL-terminated SWF string from one tag payload.  The string must end
 * inside the tag; overlong strings are stored truncated and marked. */
static int read_string(const uint8_t *payload, size_t len, size_t *off,
                       const uint8_t **text, size_t *text_len) {
    size_t start = *off;
    while (*off < len && payload[*off]) ++*off;
    if (*off >= len) return -1;
    *text = payload + start;
    *text_len = *off - start;
    ++*off;
    return 0;
}

static int parse_export_assets(const uint8_t *payload, size_t len,
                               MenuSwfSummary *s) {
    if (len < 2) return -1;
    uint32_t count = get_u16(payload);
    size_t off = 2;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 2 > len) return -1;
        uint16_t id = get_u16(payload + off);
        off += 2;
        const uint8_t *text = NULL;
        size_t text_len = 0;
        if (read_string(payload, len, &off, &text, &text_len)) return -1;
        add_name(s, id, MENU_SWF_NAME_EXPORT, text, text_len);
    }
    return off == len ? 0 : -1;
}

static int parse_import_assets(const uint8_t *payload, size_t len,
                               MenuSwfSummary *s, int version2) {
    size_t off = 0;
    const uint8_t *url = NULL;
    size_t url_len = 0;
    if (read_string(payload, len, &off, &url, &url_len)) return -1;
    add_name(s, 0, MENU_SWF_NAME_IMPORT_URL, url, url_len);
    if (version2) {
        /* ImportAssets2 has two reserved bytes between the URL and the count. */
        if (len - off < 2) return -1;
        off += 2;
    }
    if (off + 2 > len) return -1;
    uint32_t count = get_u16(payload + off);
    off += 2;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 2 > len) return -1;
        uint16_t id = get_u16(payload + off);
        off += 2;
        const uint8_t *text = NULL;
        size_t text_len = 0;
        if (read_string(payload, len, &off, &text, &text_len)) return -1;
        add_name(s, id, MENU_SWF_NAME_IMPORT, text, text_len);
    }
    return off == len ? 0 : -1;
}

static int parse_define_font_name(const uint8_t *payload, size_t len,
                                  MenuSwfSummary *s) {
    if (len < 3) return -1;
    uint16_t id = get_u16(payload);
    size_t off = 2;
    const uint8_t *text = NULL;
    size_t text_len = 0;
    if (read_string(payload, len, &off, &text, &text_len)) return -1;
    add_name(s, id, MENU_SWF_NAME_FONT, text, text_len);
    /* Retail DefineFontName carries a second copyright string after the font
     * name.  The font name is the bounded external name we keep; the remaining
     * bytes are ignored rather than rejected. */
    return 0;
}

static MenuSwfStatus parse_frame_header(const uint8_t *body, size_t body_len,
                                        size_t *tag_off, uint32_t *frame_bytes,
                                        char *reason, size_t reason_cap) {
    if (body_len < 1) {
        set_reason(reason, reason_cap, "swf: missing frame-size RECT");
        return MENU_SWF_ERR_TRUNCATED;
    }
    unsigned nbits = body[0] >> 3;
    size_t rect_bytes = (5u + 4u * nbits + 7u) / 8u;
    if (rect_bytes > body_len) {
        set_reason(reason, reason_cap,
                   "swf: frame-size RECT needs %llu bytes, body has %llu",
                   (unsigned long long)rect_bytes,
                   (unsigned long long)body_len);
        return MENU_SWF_ERR_TRUNCATED;
    }
    if (body_len - rect_bytes < 4) {
        set_reason(reason, reason_cap,
                   "swf: missing frame rate/count after the RECT");
        return MENU_SWF_ERR_TRUNCATED;
    }
    *tag_off = rect_bytes + 4u;
    if (frame_bytes) *frame_bytes = (uint32_t)*tag_off;
    return MENU_SWF_OK;
}

MenuSwfStatus menu_swf_view_open(const uint8_t *data, size_t size,
                                 MenuSwfView *view, char *reason,
                                 size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!view) return MENU_SWF_ERR_ARGUMENT;
    memset(view, 0, sizeof *view);
    if (!data || !size || size > UINT32_MAX) {
        set_reason(reason, reason_cap,
                   "swf: view needs a non-empty 32-bit-sized buffer");
        return MENU_SWF_ERR_ARGUMENT;
    }

    MenuSwfHeader header;
    BsaStatus header_status = menu_swf_header_parse(
        data, size, (uint32_t)size, &header, reason, reason_cap);
    if (header_status != BSA_OK) {
        MenuSwfStatus status = MENU_SWF_ERR_FORMAT;
        switch (header_status) {
        case BSA_ERR_ARGUMENT: status = MENU_SWF_ERR_ARGUMENT; break;
        case BSA_ERR_TRUNCATED: status = MENU_SWF_ERR_TRUNCATED; break;
        case BSA_ERR_DEFLATE: status = MENU_SWF_ERR_ZLIB; break;
        case BSA_ERR_UNSUPPORTED:
            if (header.format == MENU_SWF_ZWS) {
                status = MENU_SWF_ERR_NOTIMPL;
                set_reason(reason, reason_cap,
                           "swf: ZWS (LZMA) decompression is E_NOTIMPL");
            } else if (header.format == MENU_SWF_UNKNOWN) {
                status = MENU_SWF_ERR_FORMAT;
            } else {
                status = MENU_SWF_ERR_VERSION;
            }
            break;
        default: status = MENU_SWF_ERR_FORMAT; break;
        }
        return status;
    }

    if (header.declared_bytes > MENU_SWF_MAX_DECLARED) {
        set_reason(reason, reason_cap,
                   "swf: declared size %u exceeds the %u-byte cap",
                   header.declared_bytes, MENU_SWF_MAX_DECLARED);
        return MENU_SWF_ERR_BUDGET;
    }
    size_t body_len = (size_t)header.declared_bytes - 8u;
    const uint8_t *body = NULL;
    uint8_t *owned = NULL;

    if (header.format == MENU_SWF_FWS) {
        body = data + 8;
    } else {
        if (size < 14) {
            set_reason(reason, reason_cap,
                       "swf: CWS stream is shorter than a zlib header and trailer");
            return MENU_SWF_ERR_TRUNCATED;
        }
        size_t compressed_len = size - 8u;
        if (body_len) {
            owned = (uint8_t *)malloc(body_len);
            if (!owned) {
                set_reason(reason, reason_cap,
                           "swf: cannot allocate the %llu-byte body",
                           (unsigned long long)body_len);
                return MENU_SWF_ERR_BUDGET;
            }
        }
        size_t produced = 0;
        BsaStatus st = bsa_inflate_zlib_exact(data + 8, compressed_len, owned,
                                              body_len, &produced,
                                              reason, reason_cap);
        if (st != BSA_OK) {
            char inner[BSA_REASON_MAX];
            snprintf(inner, sizeof inner, "%s",
                     reason && reason[0] ? reason : bsa_status_name(st));
            free(owned);
            set_reason(reason, reason_cap, "swf: CWS inflate: %s", inner);
            return st == BSA_ERR_BUDGET ? MENU_SWF_ERR_BUDGET
                                        : MENU_SWF_ERR_ZLIB;
        }
        if (produced != body_len) {
            free(owned);
            set_reason(reason, reason_cap,
                       "swf: CWS produced %llu bytes, declared body is %llu",
                       (unsigned long long)produced,
                       (unsigned long long)body_len);
            return MENU_SWF_ERR_ZLIB;
        }
        body = owned;
    }

    size_t tag_offset = 0;
    MenuSwfStatus st = parse_frame_header(body, body_len, &tag_offset, NULL,
                                          reason, reason_cap);
    if (st != MENU_SWF_OK) {
        free(owned);
        return st;
    }
    view->header = header;
    view->body = body;
    view->body_len = body_len;
    view->tag_offset = tag_offset;
    view->owned = owned;
    return MENU_SWF_OK;
}

void menu_swf_view_close(MenuSwfView *view) {
    if (!view) return;
    free(view->owned);
    memset(view, 0, sizeof *view);
}

static MenuSwfStatus parse_tag_stream(const uint8_t *body, size_t body_len,
                                      unsigned depth, int top_level,
                                      MenuSwfSummary *s, size_t *consumed,
                                      int *ended, char *reason,
                                      size_t reason_cap) {
    size_t off = 0;
    int done = 0;
    while (off < body_len) {
        if (s->tag_count >= MENU_SWF_MAX_TAGS) {
            set_reason(reason, reason_cap,
                       "swf: more than %u tags", MENU_SWF_MAX_TAGS);
            return MENU_SWF_ERR_TAG;
        }
        if (body_len - off < 2) {
            set_reason(reason, reason_cap, "swf: truncated tag header at body %llu",
                       (unsigned long long)off);
            return MENU_SWF_ERR_TRUNCATED;
        }
        uint16_t header = get_u16(body + off);
        off += 2;
        uint32_t code = header >> 6;
        uint32_t len = header & 0x3fu;
        if (len == 0x3fu) {
            if (body_len - off < 4) {
                set_reason(reason, reason_cap,
                           "swf: truncated long tag header at body %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_TRUNCATED;
            }
            len = get_u32(body + off);
            off += 4;
        }
        if (len > body_len - off) {
            set_reason(reason, reason_cap,
                       "swf: tag code %u length %u runs past the %llu-byte body",
                       code, len, (unsigned long long)body_len);
            return MENU_SWF_ERR_TAG;
        }
        const uint8_t *payload = body + off;
        uint32_t payload_at = (uint32_t)off;

        switch (code) {
        case 0: /* End */
            if (len != 0) {
                set_reason(reason, reason_cap,
                           "swf: End tag has %u payload bytes", len);
                return MENU_SWF_ERR_FORMAT;
            }
            done = 1;
            break;
        case 1:
            ++s->show_frame;
            if (top_level && s->first_show_frame < 0) {
                s->first_show_frame = (int32_t)s->top_level_tag_count;
                s->first_show_frame_offset = payload_at;
            }
            break;
        case 2: case 22: case 32: case 83:
            ++s->define_shape;
            break;
        case 6: case 21: case 35: case 90:
            ++s->define_bits_jpeg;
            break;
        case 12:
            ++s->do_action;
            break;
        case 20: case 36:
            ++s->define_bits_lossless;
            break;
        case 26:
            ++s->place_object2;
            break;
        case 39: {
            ++s->define_sprite;
            if (len < 4) {
                set_reason(reason, reason_cap,
                           "swf: DefineSprite at body %llu is shorter than 4 bytes",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
            if (depth >= MENU_SWF_MAX_SPRITE_DEPTH) {
                set_reason(reason, reason_cap,
                           "swf: DefineSprite nesting exceeds %u",
                           MENU_SWF_MAX_SPRITE_DEPTH);
                return MENU_SWF_ERR_TAG;
            }
            size_t nested_consumed = 0;
            int nested_ended = 0;
            MenuSwfStatus nested = parse_tag_stream(
                payload + 4, len - 4, depth + 1, 0, s, &nested_consumed,
                &nested_ended, reason, reason_cap);
            if (nested != MENU_SWF_OK) return nested;
            if (!nested_ended || nested_consumed != (size_t)len - 4u) {
                set_reason(reason, reason_cap,
                           "swf: DefineSprite at body %llu has trailing tag bytes",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
            break;
        }
        case 48:
            ++s->define_font2;
            break;
        case 56:
            ++s->export_assets;
            if (parse_export_assets(payload, len, s)) {
                set_reason(reason, reason_cap,
                           "swf: malformed ExportAssets tag at body %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
            break;
        case 57:
            ++s->import_assets;
            if (parse_import_assets(payload, len, s, 0)) {
                set_reason(reason, reason_cap,
                           "swf: malformed ImportAssets tag at body %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
            break;
        case 70:
            ++s->place_object3;
            break;
        case 71:
            ++s->import_assets;
            if (parse_import_assets(payload, len, s, 1)) {
                set_reason(reason, reason_cap,
                           "swf: malformed ImportAssets2 tag at body %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
            break;
        case 75:
            ++s->define_font3;
            break;
        case 88:
            ++s->define_font_name;
            if (parse_define_font_name(payload, len, s)) {
                set_reason(reason, reason_cap,
                           "swf: malformed DefineFontName tag at body %llu",
                           (unsigned long long)(off - 2));
                return MENU_SWF_ERR_FORMAT;
            }
            break;
        default:
            break;
        }

        ++s->tag_count;
        if (top_level) {
            ++s->top_level_tag_count;
            s->tag_payload_bytes += len;
        } else {
            ++s->nested_tag_count;
        }
        off += len;
        if (done) break;
    }

    *consumed = off;
    *ended = done;
    if (!done) {
        set_reason(reason, reason_cap, "swf: tag stream has no End tag");
        return MENU_SWF_ERR_END;
    }
    return MENU_SWF_OK;
}

MenuSwfStatus menu_swf_summary(const uint8_t *data, size_t size,
                               MenuSwfSummary *out, char *reason,
                               size_t reason_cap) {
    if (reason && reason_cap) reason[0] = 0;
    if (!out) return MENU_SWF_ERR_ARGUMENT;
    summary_clear(out);

    MenuSwfView view;
    MenuSwfStatus st = menu_swf_view_open(data, size, &view, reason, reason_cap);
    if (st != MENU_SWF_OK) return st;

    out->header = view.header;
    out->frame_header_bytes = (uint32_t)view.tag_offset;
    size_t consumed = 0;
    int ended = 0;
    st = parse_tag_stream(view.body + view.tag_offset,
                          view.body_len - view.tag_offset, 0, 1, out,
                          &consumed, &ended, reason, reason_cap);
    if (st != MENU_SWF_OK) {
        menu_swf_view_close(&view);
        summary_clear(out);
        return st;
    }
    out->tag_stream_bytes = consumed;
    if (view.tag_offset + consumed < view.body_len)
        out->trailing_bytes =
            (uint32_t)(view.body_len - view.tag_offset - consumed);
    menu_swf_view_close(&view);
    return MENU_SWF_OK;
}
