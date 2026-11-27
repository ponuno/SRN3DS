#include "d3d9_capture.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Full capture is opt-in so normal runs keep the historical bounded payload.
 * These limits match the core's compact indexed-menu vertex/index bounds. */
#define D9_CAPTURE_FULL_MAX_VERTICES 65536u
#define D9_CAPTURE_FULL_MAX_INDICES  262144u

/* The standalone capture smoke links only this module and guest.c.  Weak
 * declarations let that test keep working while production COM builds resolve
 * the live compact stream used by TESV_D3D9_CAPTURE_FULL=1. */
#if defined(__GNUC__) || defined(__clang__)
#define D9_CAPTURE_WEAK __attribute__((weak))
#else
#define D9_CAPTURE_WEAK
#endif
extern D9Device *d9_com_device(void) D9_CAPTURE_WEAK;
extern int d9_get_stream_info(const D9Device *, unsigned, D9Handle *, size_t *, size_t *) D9_CAPTURE_WEAK;
extern int d9_get_buffer_data(const D9Device *, D9Handle, const void **, size_t *) D9_CAPTURE_WEAK;
extern int d9_decl_decode(unsigned, const void *, size_t, float *) D9_CAPTURE_WEAK;

/* ---- FNV-1a ---------------------------------------------------------------- */

#define FNV64_OFFSET UINT64_C(0xcbf29ce484222325)
#define FNV64_PRIME  UINT64_C(0x00000100000001b3)

uint64_t d9_capture_fnv1a(const void *data, size_t bytes) {
    uint64_t h = FNV64_OFFSET;
    const uint8_t *p = (const uint8_t *)data;
    if (!p) return h;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

static uint64_t fnv_update(uint64_t h, const void *data, size_t bytes) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

static uint64_t fnv_u32(uint64_t h, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return fnv_update(h, b, sizeof b);
}

static uint64_t fnv_u64(uint64_t h, uint64_t v) {
    uint8_t b[8];
    for (unsigned i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i));
    return fnv_update(h, b, sizeof b);
}

/* ---- session state --------------------------------------------------------- */

typedef struct {
    size_t declared;
    size_t stored;
    uint64_t hash;
} Blob;

typedef struct {
    Blob vs, ps;
    Blob vertices, indices;
    Blob textures[D9_CAPTURE_MAX_TEXTURES];
    D9CaptureBytes vertex_data;
    unsigned vertex_stride;
    unsigned vertex_count;
    int vertices_reconstructed;
    void *owned_vertices;
    unsigned texture_count;
    unsigned stream_count;
    unsigned decl_count;
    unsigned truncation;
    uint64_t signature;
} Plan;

typedef struct {
    int initialized;
    unsigned max_records;
    unsigned max_presents;
    unsigned flags;
    unsigned count;
    unsigned present_count;
    char directory[200];
    char basename[100];
    uint64_t seen[D9_CAPTURE_MAX_RECORDS];
    D9CaptureRecordInfo records[D9_CAPTURE_MAX_RECORDS];
    D9CapturePresentInfo present_records[D9_CAPTURE_MAX_PRESENTS];
    char error[D9_CAPTURE_ERROR_MAX];
    char last_text[D9_CAPTURE_PATH_MAX];
    char last_binary[D9_CAPTURE_PATH_MAX];
    char last_present_text[D9_CAPTURE_PATH_MAX];
    char last_present_binary[D9_CAPTURE_PATH_MAX];
} CaptureState;

static CaptureState g;

static void set_error(int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.error, sizeof g.error, fmt, ap);
    va_end(ap);
    (void)code;
}

/* handle 0 is the implicit back buffer even if a caller left the flag clear.
 * A nonzero implicit target is contradictory and rejected. */
static unsigned target_is_implicit(const D9CaptureRenderTarget *t) {
    return t->handle == 0 || t->implicit ? 1u : 0u;
}

static int validate_target(const D9CaptureRenderTarget *t) {
    if (t->implicit > 1u) {
        set_error(D9_CAPTURE_INVALID, "render-target implicit flag is %u", t->implicit);
        return D9_CAPTURE_INVALID;
    }
    if (t->implicit && t->handle != 0) {
        set_error(D9_CAPTURE_INVALID,
                  "render target 0x%08lx is marked implicit but handle is nonzero",
                  (unsigned long)t->handle);
        return D9_CAPTURE_INVALID;
    }
    return D9_CAPTURE_OK;
}

static int validate_rect(const D9CaptureRect *r, const char *name) {
    if (!r->valid) return D9_CAPTURE_OK;
    if (r->right < r->left || r->bottom < r->top) {
        set_error(D9_CAPTURE_INVALID, "%s rect is inverted (%d,%d)-(%d,%d)", name,
                  (int)r->left, (int)r->top, (int)r->right, (int)r->bottom);
        return D9_CAPTURE_INVALID;
    }
    return D9_CAPTURE_OK;
}

static int mul_size(size_t a, size_t b, size_t *out) {
    if (a && b > SIZE_MAX / a) return -1;
    *out = a * b;
    return 0;
}

static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

static int capture_full_enabled(void) {
    const char *value = getenv("TESV_D3D9_CAPTURE_FULL");
    return value && !strcmp(value, "1");
}

static unsigned vertex_limit(void) {
    return capture_full_enabled() ? D9_CAPTURE_FULL_MAX_VERTICES : D9_CAPTURE_MAX_VERTICES;
}

static unsigned index_limit(void) {
    return capture_full_enabled() ? D9_CAPTURE_FULL_MAX_INDICES : D9_CAPTURE_MAX_INDICES;
}

static void free_plan(Plan *plan) {
    if (!plan) return;
    free(plan->owned_vertices);
    plan->owned_vertices = NULL;
}

static const D9VertexElement *find_usage0(const D9CaptureSnapshot *s, unsigned usage) {
    unsigned count = min_size(s->declaration.count, D9_CAPTURE_MAX_DECL);
    for (unsigned i = 0; i < count; ++i) {
        const D9VertexElement *element = &s->declaration.elements[i];
        if (element->usage == usage && element->usage_index == 0) return element;
    }
    return NULL;
}

/* Compact indexed menu draws submit [BaseVertex+MinIndex, +NumVertices) without
 * expanding it into d->scratch.  The COM snapshot's ordinary vertex field can
 * therefore contain a stale earlier scratch stream.  Full capture reconstructs
 * exactly the compact source window through the existing read-only diagnostic
 * getters; default capture keeps the historical snapshot behaviour. */
static int prepare_vertices(const D9CaptureSnapshot *s, Plan *p) {
    p->vertex_data = s->draw.vertices.data;
    p->vertex_stride = s->draw.vertices.stride;
    p->vertex_count = s->draw.vertices.count;

    if (!capture_full_enabled() || !s->draw.indexed || s->draw.type != D9_TRIANGLELIST ||
        !s->draw.vertex_count || p->vertex_count == s->draw.vertex_count ||
        s->draw.vertex_count > D9_CAPTURE_FULL_MAX_VERTICES)
        return D9_CAPTURE_OK;
    if (!s->declaration.active || s->declaration.position >= D9_CAPTURE_MAX_DECL ||
        !d9_com_device || !d9_get_stream_info || !d9_get_buffer_data || !d9_decl_decode) {
        set_error(D9_CAPTURE_INVALID,
                  "full capture needs a live COM device and active declaration for compact vertices");
        return D9_CAPTURE_INVALID;
    }

    const D9VertexElement *position = &s->declaration.elements[s->declaration.position];
    const D9VertexElement *uv = find_usage0(s, D9_USAGE_TEXCOORD);
    const D9VertexElement *color = find_usage0(s, D9_USAGE_COLOR);
    unsigned stream = position->stream;
    if ((uv && uv->stream != stream) || (color && color->stream != stream)) {
        set_error(D9_CAPTURE_INVALID,
                  "full compact capture requires POSITION/TEXCOORD0/COLOR0 on one stream");
        return D9_CAPTURE_INVALID;
    }

    const D9CaptureStream *recorded = NULL;
    unsigned stream_count = min_size(s->stream_count, D9_CAPTURE_MAX_STREAMS);
    for (unsigned i = 0; i < stream_count; ++i)
        if (s->streams[i].index == stream) { recorded = &s->streams[i]; break; }
    if (!recorded) {
        set_error(D9_CAPTURE_INVALID, "full compact capture has no metadata for stream %u", stream);
        return D9_CAPTURE_INVALID;
    }

    D9Device *device = d9_com_device();
    D9Handle handle = 0;
    size_t offset = 0, stride = 0;
    const void *data = NULL;
    size_t bytes = 0;
    if (!device || d9_get_stream_info(device, stream, &handle, &offset, &stride) ||
        !handle || !stride || stride > D9_CAPTURE_MAX_VERTEX_STRIDE ||
        offset != recorded->offset || stride != recorded->stride ||
        d9_get_buffer_data(device, handle, &data, &bytes) || !data) {
        set_error(D9_CAPTURE_POINTER, "full compact capture cannot read vertex stream %u", stream);
        return D9_CAPTURE_POINTER;
    }

    int64_t first_signed = (int64_t)s->draw.base_vertex + (int64_t)s->draw.min_vertex;
    if (first_signed < 0 || (uint64_t)first_signed > (uint64_t)SIZE_MAX) {
        set_error(D9_CAPTURE_INVALID, "full compact capture has an invalid first vertex");
        return D9_CAPTURE_INVALID;
    }
    size_t first = (size_t)first_signed;
    size_t first_bytes = 0, span = 0, decoded_bytes = 0;
    if (mul_size(first, stride, &first_bytes) || first_bytes > SIZE_MAX - offset ||
        offset + first_bytes > bytes || mul_size(s->draw.vertex_count, stride, &span) ||
        span > bytes - (offset + first_bytes) ||
        mul_size(s->draw.vertex_count, sizeof(D9ColorVertex), &decoded_bytes)) {
        set_error(D9_CAPTURE_INVALID, "full compact vertex window exceeds the bound stream");
        return D9_CAPTURE_INVALID;
    }

    D9ColorVertex *decoded = (D9ColorVertex *)malloc(decoded_bytes);
    if (!decoded) {
        set_error(D9_CAPTURE_INVALID, "full compact vertex allocation failed");
        return D9_CAPTURE_INVALID;
    }
    const uint8_t *source = (const uint8_t *)data + offset + first_bytes;
    for (unsigned i = 0; i < s->draw.vertex_count; ++i) {
        const uint8_t *row = source + (size_t)i * stride;
        float value[4];
        if (position->offset >= stride ||
            d9_decl_decode(position->type, row + position->offset,
                           stride - position->offset, value)) {
            free(decoded);
            set_error(D9_CAPTURE_INVALID, "full compact POSITION decode failed at vertex %u", i);
            return D9_CAPTURE_INVALID;
        }
        D9ColorVertex *out = &decoded[i];
        out->x = value[0]; out->y = value[1]; out->z = value[2];
        out->u = out->v = 0.0f;
        out->r = out->g = out->b = out->a = 1.0f;
        out->w = 0.0f;
        if (uv) {
            if (uv->offset >= stride ||
                d9_decl_decode(uv->type, row + uv->offset, stride - uv->offset, value)) {
                free(decoded);
                set_error(D9_CAPTURE_INVALID, "full compact TEXCOORD0 decode failed at vertex %u", i);
                return D9_CAPTURE_INVALID;
            }
            out->u = value[0]; out->v = value[1];
        }
        if (color) {
            if (color->offset >= stride ||
                d9_decl_decode(color->type, row + color->offset,
                               stride - color->offset, value)) {
                free(decoded);
                set_error(D9_CAPTURE_INVALID, "full compact COLOR0 decode failed at vertex %u", i);
                return D9_CAPTURE_INVALID;
            }
            out->r = value[0]; out->g = value[1]; out->b = value[2]; out->a = value[3];
        }
    }

    p->owned_vertices = decoded;
    p->vertex_data = (D9CaptureBytes){decoded, 0, decoded_bytes};
    p->vertex_stride = (unsigned)sizeof(D9ColorVertex);
    p->vertex_count = s->draw.vertex_count;
    p->vertices_reconstructed = 1;
    return D9_CAPTURE_OK;
}

/* ---- guest/host slices ----------------------------------------------------- */

/* Compute the FNV-1a hash of exactly `bytes` bytes from a slice.  For a guest
 * slice every page is validated first and an unmapped page fails the whole
 * capture instead of producing a partially valid file. */
static int hash_slice(const D9CaptureBytes *s, GuestMemory *memory, size_t bytes,
                      uint64_t *hash) {
    if (!s || !hash) return D9_CAPTURE_INVALID;
    if (bytes > s->bytes) {
        set_error(D9_CAPTURE_INVALID, "slice shorter than the requested range (%zu > %zu)",
                  bytes, s->bytes);
        return D9_CAPTURE_INVALID;
    }
    uint64_t h = FNV64_OFFSET;
    size_t off = 0;
    uint8_t buffer[4096];
    while (off < bytes) {
        size_t n = min_size(bytes - off, sizeof buffer);
        if (s->host) {
            memcpy(buffer, (const uint8_t *)s->host + off, n);
        } else {
            uint64_t address = (uint64_t)s->guest + off;
            if (!s->guest || address > UINT32_MAX) {
                set_error(D9_CAPTURE_POINTER, "missing guest pointer for %zu byte slice", bytes);
                return D9_CAPTURE_POINTER;
            }
            if (!memory) {
                set_error(D9_CAPTURE_POINTER, "guest pointer 0x%08lx needs a GuestMemory",
                          (unsigned long)address);
                return D9_CAPTURE_POINTER;
            }
            if (guest_read(memory, (uint32_t)address, buffer, n)) {
                set_error(D9_CAPTURE_POINTER, "guest pointer 0x%08lx is not readable for %zu bytes",
                          (unsigned long)address, n);
                return D9_CAPTURE_POINTER;
            }
        }
        h = fnv_update(h, buffer, n);
        off += n;
    }
    *hash = h;
    return D9_CAPTURE_OK;
}

/* Second pass: write the same range while re-hashing it.  A mismatch means the
 * guest mutated the draw between signature computation and file output; the
 * caller removes the partial pair. */
static int write_slice(FILE *file, const D9CaptureBytes *s, GuestMemory *memory,
                       size_t bytes, uint64_t expected) {
    uint64_t h = FNV64_OFFSET;
    size_t off = 0;
    uint8_t buffer[4096];
    while (off < bytes) {
        size_t n = min_size(bytes - off, sizeof buffer);
        const void *source;
        if (s->host) {
            source = (const uint8_t *)s->host + off;
        } else {
            uint64_t address = (uint64_t)s->guest + off;
            if (!s->guest || address > UINT32_MAX || !memory) {
                set_error(D9_CAPTURE_POINTER, "guest pointer disappeared while writing");
                return D9_CAPTURE_POINTER;
            }
            if (guest_read(memory, (uint32_t)address, buffer, n)) {
                set_error(D9_CAPTURE_POINTER, "guest pointer 0x%08lx became unreadable",
                          (unsigned long)address);
                return D9_CAPTURE_POINTER;
            }
            source = buffer;
        }
        h = fnv_update(h, source, n);
        if (fwrite(source, 1, n, file) != n) {
            set_error(D9_CAPTURE_IO, "short write while capturing payload");
            return D9_CAPTURE_IO;
        }
        off += n;
    }
    if (h != expected) {
        set_error(D9_CAPTURE_CHANGED, "payload changed between hashing and writing");
        return D9_CAPTURE_CHANGED;
    }
    return D9_CAPTURE_OK;
}

/* Bounded blob: declared may exceed limit; stored is what will be written. */
static int plan_slice(const D9CaptureBytes *slice, GuestMemory *memory, size_t declared,
                      size_t limit, unsigned truncation_flag, Blob *blob, unsigned *trunc) {
    blob->declared = declared;
    blob->stored = min_size(declared, limit);
    blob->hash = FNV64_OFFSET;
    if (declared > limit) *trunc |= truncation_flag;
    if (!blob->stored) return D9_CAPTURE_OK;
    if (!slice->host && !slice->guest) {
        set_error(D9_CAPTURE_POINTER, "missing pointer for %zu byte payload", blob->stored);
        return D9_CAPTURE_POINTER;
    }
    return hash_slice(slice, memory, blob->stored, &blob->hash);
}

/* ---- plan ------------------------------------------------------------------ */

static uint64_t plan_signature(const D9CaptureSnapshot *s, const Plan *p) {
    uint64_t h = fnv_update(FNV64_OFFSET, "D9CAP1", 6);
    const D9CaptureDraw *d = &s->draw;
    h = fnv_u32(h, d->type);
    h = fnv_u32(h, d->start);
    h = fnv_u32(h, d->primitives);
    h = fnv_u32(h, d->indexed ? 1u : 0u);
    h = fnv_u32(h, (uint32_t)d->base_vertex);
    h = fnv_u32(h, d->min_vertex);
    h = fnv_u32(h, d->vertex_count);
    h = fnv_u32(h, d->start_index);

    h = fnv_u32(h, s->target.handle);
    h = fnv_u32(h, target_is_implicit(&s->target) ? 1u : 0u);
    h = fnv_u32(h, s->target.width);
    h = fnv_u32(h, s->target.height);
    h = fnv_u32(h, s->target.format);
    h = fnv_u32(h, s->target.usage);

    h = fnv_u64(h, p->vs.hash);
    h = fnv_u64(h, p->vs.stored);
    h = fnv_u64(h, p->vs.declared);
    h = fnv_u64(h, p->ps.hash);
    h = fnv_u64(h, p->ps.stored);
    h = fnv_u64(h, p->ps.declared);

    h = fnv_u32(h, s->declaration.active ? 1u : 0u);
    h = fnv_u32(h, s->declaration.count);
    h = fnv_u32(h, s->declaration.position);
    h = fnv_u32(h, s->declaration.uv);
    h = fnv_u32(h, s->declaration.stream_mask);
    for (unsigned i = 0; i < D9_CAPTURE_MAX_STREAMS; ++i)
        h = fnv_u32(h, s->declaration.required[i]);
    for (unsigned i = 0; i < p->decl_count; ++i) {
        const D9VertexElement *e = &s->declaration.elements[i];
        h = fnv_u32(h, e->stream);
        h = fnv_u32(h, e->offset);
        h = fnv_u32(h, e->type);
        h = fnv_u32(h, e->method);
        h = fnv_u32(h, e->usage);
        h = fnv_u32(h, e->usage_index);
    }

    for (unsigned i = 0; i < p->stream_count; ++i) {
        const D9CaptureStream *st = &s->streams[i];
        h = fnv_u32(h, st->index);
        h = fnv_u32(h, st->stride);
        h = fnv_u64(h, (uint64_t)st->offset);
        h = fnv_u64(h, (uint64_t)st->bytes);
    }

    h = fnv_u32(h, p->vertex_stride);
    h = fnv_u32(h, p->vertex_count);
    h = fnv_u64(h, p->vertices.hash);
    h = fnv_u32(h, d->indices.size);
    h = fnv_u32(h, d->indices.count);
    h = fnv_u64(h, p->indices.hash);

    for (unsigned i = 0; i < p->texture_count; ++i) {
        const D9CaptureTexture *t = &s->textures[i];
        h = fnv_u32(h, t->stage);
        h = fnv_u32(h, t->width);
        h = fnv_u32(h, t->height);
        h = fnv_u32(h, t->format);
        h = fnv_u32(h, t->levels);
        h = fnv_u32(h, t->pool);
        h = fnv_u32(h, t->usage);
        h = fnv_u32(h, t->level);
        h = fnv_u64(h, (uint64_t)t->row_pitch);
        h = fnv_u64(h, (uint64_t)t->pixels.bytes);
        h = fnv_u64(h, p->textures[i].hash);
    }
    return h;
}

static int build_plan(const D9CaptureSnapshot *s, GuestMemory *memory, Plan *p) {
    if (!s || !p) {
        set_error(D9_CAPTURE_INVALID, "missing snapshot");
        return D9_CAPTURE_INVALID;
    }
    memset(p, 0, sizeof *p);
    p->vs.hash = p->ps.hash = FNV64_OFFSET;
    p->vertices.hash = p->indices.hash = FNV64_OFFSET;

    int rc = validate_target(&s->target);
    if (rc) return rc;

    rc = plan_slice(&s->vs.code, memory, s->vs.code.bytes,
                        D9_CAPTURE_SHADER_MAX_BYTES, D9_CAPTURE_TRUNC_VS,
                        &p->vs, &p->truncation);
    if (rc) return rc;
    rc = plan_slice(&s->ps.code, memory, s->ps.code.bytes,
                    D9_CAPTURE_SHADER_MAX_BYTES, D9_CAPTURE_TRUNC_PS,
                    &p->ps, &p->truncation);
    if (rc) return rc;

    rc = prepare_vertices(s, p);
    if (rc) return rc;
    if (p->vertex_count) {
        size_t declared, capture_bytes;
        unsigned captured = (unsigned)min_size(p->vertex_count, vertex_limit());
        if (!p->vertex_stride || p->vertex_stride > D9_CAPTURE_MAX_VERTEX_STRIDE ||
            mul_size(p->vertex_count, p->vertex_stride, &declared) ||
            mul_size(captured, p->vertex_stride, &capture_bytes)) {
            set_error(D9_CAPTURE_INVALID, "invalid vertex count/stride (%u x %u)",
                      p->vertex_count, p->vertex_stride);
            return D9_CAPTURE_INVALID;
        }
        if (p->vertex_data.bytes < capture_bytes) {
            set_error(D9_CAPTURE_INVALID, "vertex slice holds %zu of %zu required bytes",
                      p->vertex_data.bytes, capture_bytes);
            return D9_CAPTURE_INVALID;
        }
        rc = plan_slice(&p->vertex_data, memory, capture_bytes, capture_bytes, 0,
                        &p->vertices, &p->truncation);
        if (rc) return rc;
        p->vertices.declared = declared;
        if (captured < p->vertex_count) p->truncation |= D9_CAPTURE_TRUNC_VERTICES;
    }

    const D9CaptureIndices *ix = &s->draw.indices;
    if (ix->count) {
        size_t declared, capture_bytes;
        unsigned captured = (unsigned)min_size(ix->count, index_limit());
        if ((ix->size != 2 && ix->size != 4) ||
            mul_size(ix->count, ix->size, &declared) ||
            mul_size(captured, ix->size, &capture_bytes)) {
            set_error(D9_CAPTURE_INVALID, "invalid index count/size (%u x %u)",
                      ix->count, ix->size);
            return D9_CAPTURE_INVALID;
        }
        if (ix->data.bytes < capture_bytes) {
            set_error(D9_CAPTURE_INVALID, "index slice holds %zu of %zu required bytes",
                      ix->data.bytes, capture_bytes);
            return D9_CAPTURE_INVALID;
        }
        rc = plan_slice(&ix->data, memory, capture_bytes, capture_bytes, 0,
                        &p->indices, &p->truncation);
        if (rc) return rc;
        p->indices.declared = declared;
        if (captured < ix->count) p->truncation |= D9_CAPTURE_TRUNC_INDICES;
    }

    p->texture_count = s->texture_count;
    if (p->texture_count > D9_CAPTURE_MAX_TEXTURES) {
        p->texture_count = D9_CAPTURE_MAX_TEXTURES;
        p->truncation |= D9_CAPTURE_TRUNC_TEXTURES;
    }
    size_t budget = D9_CAPTURE_TEXTURE_MAX_BYTES;
    for (unsigned i = 0; i < p->texture_count; ++i) {
        const D9CaptureTexture *t = &s->textures[i];
        size_t declared = t->pixels.bytes;
        size_t stored = min_size(declared, budget);
        p->textures[i].declared = declared;
        p->textures[i].stored = stored;
        p->textures[i].hash = FNV64_OFFSET;
        if (stored && !t->pixels.host && !t->pixels.guest) {
            set_error(D9_CAPTURE_POINTER, "texture %u has %zu pixels but no pointer",
                      i, stored);
            return D9_CAPTURE_POINTER;
        }
        if (stored) {
            rc = hash_slice(&t->pixels, memory, stored, &p->textures[i].hash);
            if (rc) return rc;
        }
        if (stored < declared) {
            p->truncation |= D9_CAPTURE_TRUNC_TEXTURE | D9_CAPTURE_TRUNC_TEXTURE_TOTAL;
        }
        budget -= stored;
    }

    p->stream_count = s->stream_count;
    if (p->stream_count > D9_CAPTURE_MAX_STREAMS) {
        p->stream_count = D9_CAPTURE_MAX_STREAMS;
        p->truncation |= D9_CAPTURE_TRUNC_STREAMS;
    }
    p->decl_count = s->declaration.count;
    if (p->decl_count > D9_CAPTURE_MAX_DECL) {
        p->decl_count = D9_CAPTURE_MAX_DECL;
        p->truncation |= D9_CAPTURE_TRUNC_DECL;
    }

    p->signature = plan_signature(s, p);
    return D9_CAPTURE_OK;
}

/* ---- naming ---------------------------------------------------------------- */

static const char *platform_directory(void) {
#ifdef __3DS__
    return "sdmc:/";
#else
    return ".";
#endif
}

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static int join_path(char *out, size_t capacity, const char *directory,
                     const char *basename, const char *suffix, const char *extension) {
    int n;
    if (!directory || !directory[0] || !strcmp(directory, ".")) {
        n = snprintf(out, capacity, "%s%s%s", basename, suffix, extension);
    } else {
        size_t len = strlen(directory);
        char last = len ? directory[len - 1] : 0;
        const char *separator = (last == '/' || last == '\\') ? "" : "/";
        n = snprintf(out, capacity, "%s%s%s%s%s", directory, separator,
                     basename, suffix, extension);
    }
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}

/* Pick the first suffix whose .txt/.bin pair does not exist.  A capture is
 * never written over an existing file, even if only one half is present. */
static int choose_paths_named(const char *basename,
                              char text[D9_CAPTURE_PATH_MAX],
                              char binary[D9_CAPTURE_PATH_MAX]) {
    for (unsigned n = 1; n <= 999; ++n) {
        char suffix[16] = "";
        if (n > 1) snprintf(suffix, sizeof suffix, "_%u", n);
        if (join_path(text, D9_CAPTURE_PATH_MAX, g.directory, basename, suffix, ".txt") ||
            join_path(binary, D9_CAPTURE_PATH_MAX, g.directory, basename, suffix, ".bin")) {
            return D9_CAPTURE_LIMIT;
        }
        if (!file_exists(text) && !file_exists(binary)) return D9_CAPTURE_OK;
    }
    return D9_CAPTURE_LIMIT;
}

static int choose_paths(char text[D9_CAPTURE_PATH_MAX], char binary[D9_CAPTURE_PATH_MAX]) {
    return choose_paths_named(g.basename, text, binary);
}

/* ---- binary output --------------------------------------------------------- */

enum {
    SECTION_DRAW = 1,
    SECTION_VIEWPORT = 2,
    SECTION_DRAWSTATE = 3,
    SECTION_WORLD = 4,
    SECTION_VIEW = 5,
    SECTION_PROJECTION = 6,
    SECTION_VS = 7,
    SECTION_PS = 8,
    SECTION_DECLARATION = 9,
    SECTION_STREAM = 10,
    SECTION_VERTICES = 11,
    SECTION_INDICES = 12,
    SECTION_TEXTURE = 13,
    SECTION_FRAME = 14,
    SECTION_END = 15,
    SECTION_TARGET = 16,
    SECTION_PRESENT = 17
};

static int wu8(FILE *f, unsigned v) {
    uint8_t b = (uint8_t)v;
    return fwrite(&b, 1, 1, f) == 1 ? 0 : -1;
}

static int wu16(FILE *f, unsigned v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    return fwrite(b, 1, 2, f) == 2 ? 0 : -1;
}

static int wu32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

static int wu64(FILE *f, uint64_t v) {
    uint8_t b[8];
    for (unsigned i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i));
    return fwrite(b, 1, 8, f) == 8 ? 0 : -1;
}

static int wf32(FILE *f, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    return wu32(f, bits);
}

static void matrix_row(FILE *f, const D9Matrix *m, unsigned row) {
    for (unsigned column = 0; column < 4; ++column) (void)wf32(f, m->m[row][column]);
}

static int write_section(FILE *f, uint32_t id, uint32_t size) {
    return wu32(f, id) || wu32(f, size) ? -1 : 0;
}

static int write_shader_section(FILE *f, uint32_t id, const D9CaptureShader *source,
                                const Blob *blob, GuestMemory *memory) {
    uint32_t payload = 32u + (uint32_t)blob->stored;
    if (write_section(f, id, payload) ||
        wu32(f, source->object) || wu32(f, 0u) ||
        wu64(f, blob->hash) || wu64(f, (uint64_t)blob->declared) ||
        wu32(f, (uint32_t)blob->stored) ||
        wu32(f, blob->stored < blob->declared ? 1u : 0u)) return D9_CAPTURE_IO;
    return write_slice(f, &source->code, memory, blob->stored, blob->hash);
}

static int write_binary(FILE *f, const D9CaptureSnapshot *s, const Plan *p,
                        GuestMemory *memory) {
    static const char magic[8] = {'D', '9', 'C', 'A', 'P', 'B', 'I', 'N'};
    unsigned sections = 14 + p->stream_count + p->texture_count;
    if (fwrite(magic, 1, sizeof magic, f) != sizeof magic ||
        wu32(f, D9_CAPTURE_SCHEMA_VERSION) || wu32(f, 32u) ||
        wu64(f, p->signature) || wu32(f, p->truncation) || wu32(f, sections))
        return D9_CAPTURE_IO;

    if (write_section(f, SECTION_DRAW, 32u) ||
        wu32(f, s->draw.type) || wu32(f, s->draw.start) || wu32(f, s->draw.primitives) ||
        wu32(f, s->draw.indexed ? 1u : 0u) || wu32(f, (uint32_t)s->draw.base_vertex) ||
        wu32(f, s->draw.min_vertex) || wu32(f, s->draw.vertex_count) ||
        wu32(f, s->draw.start_index)) return D9_CAPTURE_IO;

    if (write_section(f, SECTION_VIEWPORT, 24u) ||
        wu32(f, s->viewport.x) || wu32(f, s->viewport.y) ||
        wu32(f, s->viewport.width) || wu32(f, s->viewport.height) ||
        wf32(f, s->viewport.minz) || wf32(f, s->viewport.maxz)) return D9_CAPTURE_IO;

    if (write_section(f, SECTION_DRAWSTATE, 100u) ||
        wu32(f, s->state.z_enable) || wu32(f, s->state.z_write) || wu32(f, s->state.z_func) ||
        wu32(f, s->state.cull) || wu32(f, s->state.alpha_test) ||
        wu32(f, s->state.alpha_func) || wu32(f, s->state.alpha_ref) ||
        wu32(f, s->state.blend) || wu32(f, s->state.src_blend) ||
        wu32(f, s->state.dst_blend) || wu32(f, s->state.blend_op) ||
        wu32(f, s->state.color_write) || wu32(f, s->state.address_u) ||
        wu32(f, s->state.address_v) || wu32(f, s->state.mag_filter) ||
        wu32(f, s->state.min_filter) || wu32(f, s->state.mip_filter) ||
        wu32(f, s->state.max_mip) || wu32(f, s->state.resident_mips) ||
        wu32(f, s->state.pixel_shader ? 1u : 0u) ||
        wu32(f, s->state.address_w) || wf32(f, d9_sampler_lod_bias(s->state.lod_bias)) ||
        wu32(f, s->state.max_anisotropy) || wu32(f, s->state.srgb) ||
        wu32(f, s->state.border_color)) return D9_CAPTURE_IO;

    if (write_section(f, SECTION_WORLD, 64u)) return D9_CAPTURE_IO;
    for (unsigned row = 0; row < 4; ++row) matrix_row(f, &s->world, row);
    if (write_section(f, SECTION_VIEW, 64u)) return D9_CAPTURE_IO;
    for (unsigned row = 0; row < 4; ++row) matrix_row(f, &s->view, row);
    if (write_section(f, SECTION_PROJECTION, 64u)) return D9_CAPTURE_IO;
    for (unsigned row = 0; row < 4; ++row) matrix_row(f, &s->projection, row);

    int rc = write_shader_section(f, SECTION_VS, &s->vs, &p->vs, memory);
    if (rc) return rc;
    rc = write_shader_section(f, SECTION_PS, &s->ps, &p->ps, memory);
    if (rc) return rc;

    if (write_section(f, SECTION_DECLARATION, 84u + p->decl_count * 8u) ||
        wu32(f, s->declaration.active ? 1u : 0u) ||
        wu32(f, s->declaration.count) || wu32(f, s->declaration.position) ||
        wu32(f, s->declaration.uv) || wu32(f, s->declaration.stream_mask))
        return D9_CAPTURE_IO;
    for (unsigned i = 0; i < D9_CAPTURE_MAX_STREAMS; ++i)
        if (wu32(f, s->declaration.required[i])) return D9_CAPTURE_IO;
    for (unsigned i = 0; i < p->decl_count; ++i) {
        const D9VertexElement *e = &s->declaration.elements[i];
        if (wu16(f, e->stream) || wu16(f, e->offset) ||
            wu8(f, e->type) || wu8(f, e->method) ||
            wu8(f, e->usage) || wu8(f, e->usage_index)) return D9_CAPTURE_IO;
    }

    for (unsigned i = 0; i < p->stream_count; ++i) {
        const D9CaptureStream *st = &s->streams[i];
        if (write_section(f, SECTION_STREAM, 40u) ||
            wu32(f, st->index) || wu32(f, st->handle) || wu32(f, st->stride) ||
            wu32(f, 0u) || wu64(f, (uint64_t)st->offset) ||
            wu64(f, (uint64_t)st->bytes) || wu32(f, st->guest) || wu32(f, 0u))
            return D9_CAPTURE_IO;
    }

    if (write_section(f, SECTION_VERTICES, 40u + (uint32_t)p->vertices.stored) ||
        wu32(f, p->vertex_stride) || wu32(f, p->vertex_count) ||
        wu32(f, (uint32_t)(p->vertices.stored / (p->vertex_stride ?
              p->vertex_stride : 1))) ||
        wu32(f, (uint32_t)p->vertices.stored) ||
        wu64(f, (uint64_t)p->vertices.declared) || wu64(f, p->vertices.hash) ||
        wu32(f, p->vertices.stored < p->vertices.declared ? 1u : 0u) || wu32(f, 0u))
        return D9_CAPTURE_IO;
    rc = write_slice(f, &p->vertex_data, memory, p->vertices.stored,
                     p->vertices.hash);
    if (rc) return rc;

    if (write_section(f, SECTION_INDICES, 40u + (uint32_t)p->indices.stored) ||
        wu32(f, s->draw.indices.size) || wu32(f, s->draw.indices.count) ||
        wu32(f, (uint32_t)(p->indices.stored / (s->draw.indices.size ?
              s->draw.indices.size : 1))) ||
        wu32(f, (uint32_t)p->indices.stored) ||
        wu64(f, (uint64_t)p->indices.declared) || wu64(f, p->indices.hash) ||
        wu32(f, p->indices.stored < p->indices.declared ? 1u : 0u) || wu32(f, 0u))
        return D9_CAPTURE_IO;
    rc = write_slice(f, &s->draw.indices.data, memory, p->indices.stored,
                     p->indices.hash);
    if (rc) return rc;

    for (unsigned i = 0; i < p->texture_count; ++i) {
        const D9CaptureTexture *t = &s->textures[i];
        const Blob *blob = &p->textures[i];
        if (write_section(f, SECTION_TEXTURE, 76u + (uint32_t)blob->stored) ||
            wu32(f, t->stage) || wu32(f, t->handle) ||
            wu32(f, t->width) || wu32(f, t->height) ||
            wu32(f, t->format) || wu32(f, t->levels) ||
            wu32(f, t->pool) || wu32(f, t->usage) ||
            wu32(f, t->level) ||
            wu32(f, blob->stored < blob->declared ? 1u : 0u) || wu32(f, 0u) ||
            wu64(f, (uint64_t)t->row_pitch) || wu64(f, (uint64_t)blob->declared) ||
            wu64(f, (uint64_t)blob->stored) || wu64(f, blob->hash))
            return D9_CAPTURE_IO;
        rc = write_slice(f, &t->pixels, memory, blob->stored, blob->hash);
        if (rc) return rc;
    }

    if (write_section(f, SECTION_FRAME, 36u) ||
        wu32(f, s->frame_index) || wu32(f, s->draw_index) ||
        wu32(f, s->clear.valid ? 1u : 0u) || wu32(f, s->clear.color) ||
        wu32(f, s->clear.flags) || wu32(f, s->clear.rect_count) ||
        wu32(f, s->present.present ? 1u : 0u) || wu32(f, s->present.flags) ||
        wu32(f, s->present.swap_chain)) return D9_CAPTURE_IO;

    /* Version 2 addition; version 1 readers skip it by payload size. */
    if (write_section(f, SECTION_TARGET, 24u) ||
        wu32(f, s->target.handle) || wu32(f, target_is_implicit(&s->target)) ||
        wu32(f, s->target.width) || wu32(f, s->target.height) ||
        wu32(f, s->target.format) || wu32(f, s->target.usage))
        return D9_CAPTURE_IO;

    if (write_section(f, SECTION_END, 0u)) return D9_CAPTURE_IO;
    if (ferror(f)) {
        set_error(D9_CAPTURE_IO, "error while writing binary capture");
        return D9_CAPTURE_IO;
    }
    return D9_CAPTURE_OK;
}

/* ---- text output ----------------------------------------------------------- */

int d9_capture_truncation_text(unsigned flags, char *out, size_t size) {
    static const struct { unsigned bit; const char *name; } names[] = {
        {D9_CAPTURE_TRUNC_VERTICES, "vertices"},
        {D9_CAPTURE_TRUNC_INDICES, "indices"},
        {D9_CAPTURE_TRUNC_VS, "vs"},
        {D9_CAPTURE_TRUNC_PS, "ps"},
        {D9_CAPTURE_TRUNC_TEXTURE, "texture"},
        {D9_CAPTURE_TRUNC_TEXTURE_TOTAL, "texture_total"},
        {D9_CAPTURE_TRUNC_STREAMS, "streams"},
        {D9_CAPTURE_TRUNC_TEXTURES, "textures"},
        {D9_CAPTURE_TRUNC_DECL, "declaration"}
    };
    if (!out || !size) return -1;
    size_t used = 0;
    out[0] = 0;
    if (!flags) {
        const char *none = "none";
        if (size <= strlen(none)) return -1;
        strcpy(out, none);
        return (int)strlen(none);
    }
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (!(flags & names[i].bit)) continue;
        size_t len = strlen(names[i].name);
        if (used + len + (used ? 1 : 0) + 1 > size) return -1;
        if (used) out[used++] = ',';
        memcpy(out + used, names[i].name, len);
        used += len;
    }
    if (!used) {
        const char *unknown = "unknown";
        if (strlen(unknown) + 1 > size) return -1;
        strcpy(out, unknown);
        return (int)strlen(unknown);
    }
    out[used] = 0;
    return (int)used;
}

static int print_matrix(FILE *f, const char *name, const D9Matrix *m) {
    for (unsigned row = 0; row < 4; ++row) {
        if (fprintf(f, "%s.r%u=%.9g,%.9g,%.9g,%.9g\n", name, row,
                    m->m[row][0], m->m[row][1], m->m[row][2], m->m[row][3]) < 0)
            return -1;
    }
    return 0;
}

static int write_text(FILE *f, const D9CaptureSnapshot *s, const Plan *p) {
    char trunc[160];
    if (d9_capture_truncation_text(p->truncation, trunc, sizeof trunc) < 0)
        strcpy(trunc, "unknown");

    if (fprintf(f,
        "format=d9_capture\n"
        "version=%u\n"
        "capture.full=%u\n"
        "signature=0x%016llx\n"
        "truncation=0x%08x\n"
        "truncation.names=%s\n"
        "frame.index=%u\n"
        "draw.index=%u\n"
        "draw.type=%u\n"
        "draw.start=%u\n"
        "draw.primitives=%u\n"
        "draw.indexed=%u\n"
        "draw.base_vertex=%d\n"
        "draw.min_vertex=%u\n"
        "draw.vertex_count=%u\n"
        "draw.start_index=%u\n"
        "viewport.x=%u\n"
        "viewport.y=%u\n"
        "viewport.width=%u\n"
        "viewport.height=%u\n"
        "viewport.minz=%.9g\n"
        "viewport.maxz=%.9g\n"
        "state.z_enable=%u\n"
        "state.z_write=%u\n"
        "state.z_func=%u\n"
        "state.cull=%u\n"
        "state.alpha_test=%u\n"
        "state.alpha_func=%u\n"
        "state.alpha_ref=%u\n"
        "state.blend=%u\n"
        "state.src_blend=%u\n"
        "state.dst_blend=%u\n"
        "state.blend_op=%u\n"
        "state.color_write=%u\n"
        "state.address_u=%u\n"
        "state.address_v=%u\n"
        "state.mag_filter=%u\n"
        "state.min_filter=%u\n"
        "state.mip_filter=%u\n"
        "state.max_mip=%u\n"
        "state.resident_mips=%u\n"
        "state.pixel_shader=%u\n"
        "state.address_w=%u\n"
        "state.lod_bias=%.9g\n"
        "state.max_anisotropy=%u\n"
        "state.srgb=%u\n"
        "state.border_color=0x%08x\n",
        D9_CAPTURE_SCHEMA_VERSION, capture_full_enabled() ? 1u : 0u,
        (unsigned long long)p->signature, p->truncation, trunc,
        s->frame_index, s->draw_index,
        s->draw.type, s->draw.start, s->draw.primitives,
        s->draw.indexed ? 1u : 0u, (int)s->draw.base_vertex,
        s->draw.min_vertex, s->draw.vertex_count, s->draw.start_index,
        (unsigned)s->viewport.x, (unsigned)s->viewport.y,
        (unsigned)s->viewport.width, (unsigned)s->viewport.height,
        (double)s->viewport.minz, (double)s->viewport.maxz,
        s->state.z_enable, s->state.z_write, s->state.z_func, s->state.cull,
        s->state.alpha_test, s->state.alpha_func, s->state.alpha_ref,
        s->state.blend, s->state.src_blend, s->state.dst_blend,
        s->state.blend_op, s->state.color_write, s->state.address_u,
        s->state.address_v, s->state.mag_filter, s->state.min_filter,
        s->state.mip_filter, s->state.max_mip, s->state.resident_mips,
        s->state.pixel_shader ? 1u : 0u,
        s->state.address_w, (double)d9_sampler_lod_bias(s->state.lod_bias),
        s->state.max_anisotropy, s->state.srgb, s->state.border_color) < 0) return D9_CAPTURE_IO;

    if (print_matrix(f, "world", &s->world) ||
        print_matrix(f, "view", &s->view) ||
        print_matrix(f, "projection", &s->projection)) return D9_CAPTURE_IO;

    if (fprintf(f,
        "vs.object=0x%08x\n"
        "vs.bytes=%zu\n"
        "vs.stored=%zu\n"
        "vs.truncated=%u\n"
        "vs.hash=0x%016llx\n"
        "ps.object=0x%08x\n"
        "ps.bytes=%zu\n"
        "ps.stored=%zu\n"
        "ps.truncated=%u\n"
        "ps.hash=0x%016llx\n"
        "declaration.active=%u\n"
        "declaration.count=%u\n"
        "declaration.captured=%u\n"
        "declaration.position=%u\n"
        "declaration.uv=%u\n"
        "declaration.stream_mask=0x%x\n",
        (unsigned)s->vs.object, p->vs.declared, p->vs.stored,
        p->vs.stored < p->vs.declared ? 1u : 0u,
        (unsigned long long)p->vs.hash,
        (unsigned)s->ps.object, p->ps.declared, p->ps.stored,
        p->ps.stored < p->ps.declared ? 1u : 0u,
        (unsigned long long)p->ps.hash,
        s->declaration.active ? 1u : 0u, s->declaration.count, p->decl_count,
        s->declaration.position, s->declaration.uv,
        s->declaration.stream_mask) < 0) return D9_CAPTURE_IO;

    for (unsigned i = 0; i < p->decl_count; ++i) {
        const D9VertexElement *e = &s->declaration.elements[i];
        if (fprintf(f, "declaration.element[%u]=%u,%u,%u,%u,%u,%u\n", i,
                    e->stream, e->offset, e->type, e->method,
                    e->usage, e->usage_index) < 0) return D9_CAPTURE_IO;
    }

    if (fprintf(f, "stream_count=%u\n", p->stream_count) < 0) return D9_CAPTURE_IO;
    for (unsigned i = 0; i < p->stream_count; ++i) {
        const D9CaptureStream *st = &s->streams[i];
        if (fprintf(f,
            "stream[%u].index=%u\n"
            "stream[%u].handle=0x%08x\n"
            "stream[%u].stride=%u\n"
            "stream[%u].offset=%zu\n"
            "stream[%u].bytes=%zu\n"
            "stream[%u].guest=0x%08x\n",
            i, st->index, i, st->handle, i, st->stride,
            i, st->offset, i, st->bytes, i, (unsigned)st->guest) < 0)
            return D9_CAPTURE_IO;
    }

    if (fprintf(f,
        "vertices.stride=%u\n"
        "vertices.count=%u\n"
        "vertices.captured=%u\n"
        "vertices.bytes=%zu\n"
        "vertices.declared_bytes=%zu\n"
        "vertices.hash=0x%016llx\n"
        "vertices.truncated=%u\n"
        "vertices.source=%s\n"
        "indices.size=%u\n"
        "indices.count=%u\n"
        "indices.captured=%u\n"
        "indices.bytes=%zu\n"
        "indices.declared_bytes=%zu\n"
        "indices.hash=0x%016llx\n"
        "indices.truncated=%u\n",
        p->vertex_stride, p->vertex_count,
        (unsigned)(p->vertices.stored / (p->vertex_stride ?
                  p->vertex_stride : 1)),
        p->vertices.stored, p->vertices.declared,
        (unsigned long long)p->vertices.hash,
        p->vertices.stored < p->vertices.declared ? 1u : 0u,
        p->vertices_reconstructed ? "decoded_compact" : "snapshot",
        s->draw.indices.size, s->draw.indices.count,
        (unsigned)(p->indices.stored / (s->draw.indices.size ?
                  s->draw.indices.size : 1)),
        p->indices.stored, p->indices.declared,
        (unsigned long long)p->indices.hash,
        p->indices.stored < p->indices.declared ? 1u : 0u) < 0)
        return D9_CAPTURE_IO;

    if (fprintf(f, "texture_count=%u\n", p->texture_count) < 0) return D9_CAPTURE_IO;
    size_t texture_bytes = 0;
    for (unsigned i = 0; i < p->texture_count; ++i) {
        const D9CaptureTexture *t = &s->textures[i];
        const Blob *blob = &p->textures[i];
        texture_bytes += blob->stored;
        if (fprintf(f,
            "texture[%u].stage=%u\n"
            "texture[%u].handle=0x%08x\n"
            "texture[%u].width=%u\n"
            "texture[%u].height=%u\n"
            "texture[%u].format=%u\n"
            "texture[%u].levels=%u\n"
            "texture[%u].pool=%u\n"
            "texture[%u].usage=%u\n"
            "texture[%u].level=%u\n"
            "texture[%u].row_pitch=%zu\n"
            "texture[%u].pixels.declared=%zu\n"
            "texture[%u].pixels.stored=%zu\n"
            "texture[%u].pixels.hash=0x%016llx\n"
            "texture[%u].pixels.truncated=%u\n",
            i, t->stage, i, t->handle, i, t->width, i, t->height,
            i, t->format, i, t->levels, i, t->pool, i, t->usage,
            i, t->level, i, t->row_pitch,
            i, blob->declared, i, blob->stored,
            i, (unsigned long long)blob->hash,
            i, blob->stored < blob->declared ? 1u : 0u) < 0)
            return D9_CAPTURE_IO;
    }

    if (fprintf(f,
        "texture.bytes=%zu\n"
        "clear.valid=%u\n"
        "clear.color=0x%08x\n"
        "clear.flags=0x%x\n"
        "clear.rect_count=%u\n"
        "present.present=%u\n"
        "present.flags=0x%x\n"
        "present.swap_chain=%u\n"
        "target.handle=0x%08x\n"
        "target.implicit=%u\n"
        "target.width=%u\n"
        "target.height=%u\n"
        "target.format=%u\n"
        "target.usage=%u\n",
        texture_bytes, s->clear.valid ? 1u : 0u, (unsigned)s->clear.color,
        (unsigned)s->clear.flags, s->clear.rect_count,
        s->present.present ? 1u : 0u, (unsigned)s->present.flags,
        s->present.swap_chain,
        (unsigned)s->target.handle, target_is_implicit(&s->target),
        s->target.width, s->target.height, s->target.format,
        s->target.usage) < 0) return D9_CAPTURE_IO;

    if (ferror(f)) {
        set_error(D9_CAPTURE_IO, "error while writing text capture");
        return D9_CAPTURE_IO;
    }
    return D9_CAPTURE_OK;
}

/* ---- present event output -------------------------------------------------- */

uint64_t d9_capture_present_signature(const D9CapturePresentEvent *event) {
    if (!event) return 0;
    uint64_t h = fnv_update(FNV64_OFFSET, "D9PRE1", 6);
    h = fnv_u32(h, event->frame_index);
    h = fnv_u32(h, event->flags);
    h = fnv_u32(h, event->swap_chain);
    h = fnv_u32(h, event->source.valid ? 1u : 0u);
    h = fnv_u32(h, (uint32_t)event->source.left);
    h = fnv_u32(h, (uint32_t)event->source.top);
    h = fnv_u32(h, (uint32_t)event->source.right);
    h = fnv_u32(h, (uint32_t)event->source.bottom);
    h = fnv_u32(h, event->dest.valid ? 1u : 0u);
    h = fnv_u32(h, (uint32_t)event->dest.left);
    h = fnv_u32(h, (uint32_t)event->dest.top);
    h = fnv_u32(h, (uint32_t)event->dest.right);
    h = fnv_u32(h, (uint32_t)event->dest.bottom);
    h = fnv_u32(h, event->target.handle);
    h = fnv_u32(h, target_is_implicit(&event->target));
    h = fnv_u32(h, event->target.width);
    h = fnv_u32(h, event->target.height);
    h = fnv_u32(h, event->target.format);
    h = fnv_u32(h, event->target.usage);
    return h;
}

static int write_present_binary(FILE *f, const D9CapturePresentEvent *event,
                               uint64_t signature) {
    static const char magic[8] = {'D', '9', 'C', 'A', 'P', 'P', 'R', 'E'};
    if (fwrite(magic, 1, sizeof magic, f) != sizeof magic ||
        wu32(f, D9_CAPTURE_SCHEMA_VERSION) || wu32(f, 32u) ||
        wu64(f, signature) || wu32(f, 0u) || wu32(f, 2u))
        return D9_CAPTURE_IO;

    /* 20 u32: frame, flags, swap chain, source rect, dest rect, target. */
    if (write_section(f, SECTION_PRESENT, 80u) ||
        wu32(f, event->frame_index) || wu32(f, event->flags) ||
        wu32(f, event->swap_chain) ||
        wu32(f, event->source.valid ? 1u : 0u) ||
        wu32(f, (uint32_t)event->source.left) ||
        wu32(f, (uint32_t)event->source.top) ||
        wu32(f, (uint32_t)event->source.right) ||
        wu32(f, (uint32_t)event->source.bottom) ||
        wu32(f, event->dest.valid ? 1u : 0u) ||
        wu32(f, (uint32_t)event->dest.left) ||
        wu32(f, (uint32_t)event->dest.top) ||
        wu32(f, (uint32_t)event->dest.right) ||
        wu32(f, (uint32_t)event->dest.bottom) ||
        wu32(f, event->target.handle) ||
        wu32(f, target_is_implicit(&event->target)) ||
        wu32(f, event->target.width) || wu32(f, event->target.height) ||
        wu32(f, event->target.format) || wu32(f, event->target.usage) ||
        wu32(f, 0u)) return D9_CAPTURE_IO;

    if (write_section(f, SECTION_END, 0u)) return D9_CAPTURE_IO;
    if (ferror(f)) {
        set_error(D9_CAPTURE_IO, "error while writing present capture");
        return D9_CAPTURE_IO;
    }
    return D9_CAPTURE_OK;
}

static int write_present_text(FILE *f, const D9CapturePresentEvent *event,
                              uint64_t signature) {
    if (fprintf(f,
        "format=d9_capture_present\n"
        "version=%u\n"
        "signature=0x%016llx\n"
        "present.frame_index=%u\n"
        "present.flags=0x%x\n"
        "present.swap_chain=%u\n"
        "present.source.valid=%u\n"
        "present.source.rect=%d,%d,%d,%d\n"
        "present.dest.valid=%u\n"
        "present.dest.rect=%d,%d,%d,%d\n"
        "target.handle=0x%08x\n"
        "target.implicit=%u\n"
        "target.width=%u\n"
        "target.height=%u\n"
        "target.format=%u\n"
        "target.usage=%u\n"
        "pixels.captured=0\n",
        D9_CAPTURE_SCHEMA_VERSION, (unsigned long long)signature,
        event->frame_index, (unsigned)event->flags, event->swap_chain,
        event->source.valid ? 1u : 0u,
        (int)event->source.left, (int)event->source.top,
        (int)event->source.right, (int)event->source.bottom,
        event->dest.valid ? 1u : 0u,
        (int)event->dest.left, (int)event->dest.top,
        (int)event->dest.right, (int)event->dest.bottom,
        (unsigned)event->target.handle,
        target_is_implicit(&event->target),
        event->target.width, event->target.height,
        event->target.format, event->target.usage) < 0)
        return D9_CAPTURE_IO;
    if (ferror(f)) {
        set_error(D9_CAPTURE_IO, "error while writing present capture");
        return D9_CAPTURE_IO;
    }
    return D9_CAPTURE_OK;
}

/* ---- public API ------------------------------------------------------------ */

void d9_capture_options_init(D9CaptureOptions *options) {
    if (!options) return;
    options->directory = NULL;
    options->basename = NULL;
    options->max_records = 1;
    options->flags = D9_CAPTURE_OPT_UNIQUE_ONLY;
    options->max_presents = 1;
}

int d9_capture_init(const D9CaptureOptions *options) {
    D9CaptureOptions defaults;
    d9_capture_options_init(&defaults);
    const D9CaptureOptions *o = options ? options : &defaults;
    memset(&g, 0, sizeof g);
    const char *directory = o->directory && o->directory[0] ?
                            o->directory : platform_directory();
    const char *basename = o->basename && o->basename[0] ?
                           o->basename : D9_CAPTURE_DEFAULT_BASENAME;
    if (strlen(directory) >= sizeof g.directory) {
        set_error(D9_CAPTURE_INVALID, "capture directory is too long");
        return D9_CAPTURE_INVALID;
    }
    if (strlen(basename) >= sizeof g.basename) {
        set_error(D9_CAPTURE_INVALID, "capture basename is too long");
        return D9_CAPTURE_INVALID;
    }
    strcpy(g.directory, directory);
    strcpy(g.basename, basename);
    g.max_records = o->max_records ? o->max_records : 1;
    if (g.max_records > D9_CAPTURE_MAX_RECORDS)
        g.max_records = D9_CAPTURE_MAX_RECORDS;
    g.max_presents = o->max_presents ? o->max_presents : 1;
    if (g.max_presents > D9_CAPTURE_MAX_PRESENTS)
        g.max_presents = D9_CAPTURE_MAX_PRESENTS;
    g.flags = o->flags;
    g.initialized = 1;
    return D9_CAPTURE_OK;
}

void d9_capture_shutdown(void) {
    memset(&g, 0, sizeof g);
}

int d9_capture_active(void) {
    return g.initialized && !(g.flags & D9_CAPTURE_OPT_DISABLED) &&
           g.count < g.max_records;
}

unsigned d9_capture_count(void) { return g.count; }

unsigned d9_capture_remaining(void) {
    if (!g.initialized || (g.flags & D9_CAPTURE_OPT_DISABLED)) return 0;
    return g.count < g.max_records ? g.max_records - g.count : 0;
}

const char *d9_capture_last_error(void) { return g.error; }
const char *d9_capture_last_text_path(void) { return g.last_text; }
const char *d9_capture_last_binary_path(void) { return g.last_binary; }

int d9_capture_record(unsigned index, D9CaptureRecordInfo *out) {
    if (!out || index >= g.count) return D9_CAPTURE_INVALID;
    *out = g.records[index];
    return D9_CAPTURE_OK;
}

uint64_t d9_capture_signature(const D9CaptureSnapshot *snapshot, GuestMemory *memory,
                              int *error) {
    g.error[0] = 0;
    Plan plan = {0};
    int rc = build_plan(snapshot, memory, &plan);
    uint64_t signature = rc == D9_CAPTURE_OK ? plan.signature : 0;
    free_plan(&plan);
    if (error) *error = rc;
    return signature;
}

int d9_capture_offer(const D9CaptureSnapshot *snapshot, GuestMemory *memory) {
    g.error[0] = 0;
    if (!g.initialized || (g.flags & D9_CAPTURE_OPT_DISABLED))
        return D9_CAPTURE_SKIP;
    if (g.count >= g.max_records) return D9_CAPTURE_SKIP;
    if (!snapshot) {
        set_error(D9_CAPTURE_INVALID, "missing snapshot");
        return D9_CAPTURE_INVALID;
    }

    Plan plan = {0};
    int rc = build_plan(snapshot, memory, &plan);
    if (rc) { free_plan(&plan); return rc; }

    if (g.flags & D9_CAPTURE_OPT_UNIQUE_ONLY) {
        for (unsigned i = 0; i < g.count; ++i)
            if (g.seen[i] == plan.signature) { free_plan(&plan); return D9_CAPTURE_SKIP; }
    }

    char text_path[D9_CAPTURE_PATH_MAX];
    char binary_path[D9_CAPTURE_PATH_MAX];
    rc = choose_paths(text_path, binary_path);
    if (rc) {
        set_error(D9_CAPTURE_LIMIT, "no free capture name below %s/%s",
                  g.directory, g.basename);
        free_plan(&plan);
        return rc;
    }

    FILE *binary = fopen(binary_path, "wb");
    if (!binary) {
        set_error(D9_CAPTURE_IO, "cannot create %s", binary_path);
        free_plan(&plan);
        return D9_CAPTURE_IO;
    }
    rc = write_binary(binary, snapshot, &plan, memory);
    long binary_bytes = -1;
    if (!rc) binary_bytes = ftell(binary);
    if (fclose(binary) && !rc) rc = D9_CAPTURE_IO;
    if (rc || binary_bytes < 0) {
        if (!rc) rc = D9_CAPTURE_IO;
        remove(binary_path);
        if (!g.error[0])
            set_error(rc, "failed to write %s", binary_path);
        free_plan(&plan);
        return rc;
    }

    FILE *text = fopen(text_path, "wb");
    if (!text) {
        set_error(D9_CAPTURE_IO, "cannot create %s", text_path);
        remove(binary_path);
        free_plan(&plan);
        return D9_CAPTURE_IO;
    }
    rc = write_text(text, snapshot, &plan);
    long text_bytes = -1;
    if (!rc) text_bytes = ftell(text);
    if (fclose(text) && !rc) rc = D9_CAPTURE_IO;
    if (rc || text_bytes < 0) {
        if (!rc) rc = D9_CAPTURE_IO;
        remove(text_path);
        remove(binary_path);
        if (!g.error[0])
            set_error(rc, "failed to write %s", text_path);
        free_plan(&plan);
        return rc;
    }

    D9CaptureRecordInfo *info = &g.records[g.count];
    memset(info, 0, sizeof *info);
    info->index = g.count + 1;
    info->signature = plan.signature;
    info->truncation = plan.truncation;
    info->vs_hash = plan.vs.hash;
    info->ps_hash = plan.ps.hash;
    info->vertex_count = plan.vertex_count;
    info->vertex_captured = (unsigned)(plan.vertices.stored /
        (plan.vertex_stride ? plan.vertex_stride : 1));
    info->index_count = snapshot->draw.indices.count;
    info->index_captured = (unsigned)(plan.indices.stored /
        (snapshot->draw.indices.size ? snapshot->draw.indices.size : 1));
    info->vertex_bytes = plan.vertices.stored;
    info->index_bytes = plan.indices.stored;
    info->vs_bytes = plan.vs.stored;
    info->ps_bytes = plan.ps.stored;
    info->texture_count = plan.texture_count;
    info->stream_count = plan.stream_count;
    for (unsigned i = 0; i < plan.texture_count; ++i)
        info->texture_bytes += plan.textures[i].stored;
    info->text_bytes = (size_t)text_bytes;
    info->binary_bytes = (size_t)binary_bytes;
    strcpy(info->text_path, text_path);
    strcpy(info->binary_path, binary_path);
    g.seen[g.count] = plan.signature;
    g.last_text[0] = 0;
    g.last_binary[0] = 0;
    strcpy(g.last_text, text_path);
    strcpy(g.last_binary, binary_path);
    g.count++;
    g.error[0] = 0;
    free_plan(&plan);
    return D9_CAPTURE_OK;
}

int d9_capture_present_active(void) {
    if (!g.initialized || (g.flags & D9_CAPTURE_OPT_DISABLED)) return 0;
    return g.present_count < g.max_presents;
}

unsigned d9_capture_present_count(void) { return g.present_count; }

const char *d9_capture_last_present_text_path(void) { return g.last_present_text; }
const char *d9_capture_last_present_binary_path(void) { return g.last_present_binary; }

int d9_capture_present_record(unsigned index, D9CapturePresentInfo *out) {
    if (!out || index >= g.present_count) return D9_CAPTURE_INVALID;
    *out = g.present_records[index];
    return D9_CAPTURE_OK;
}

int d9_capture_offer_present(const D9CapturePresentEvent *event) {
    g.error[0] = 0;
    if (!g.initialized || (g.flags & D9_CAPTURE_OPT_DISABLED))
        return D9_CAPTURE_SKIP;
    if (g.present_count >= g.max_presents) return D9_CAPTURE_SKIP;
    if (!event) {
        set_error(D9_CAPTURE_INVALID, "missing present event");
        return D9_CAPTURE_INVALID;
    }

    int rc = validate_target(&event->target);
    if (rc) return rc;
    rc = validate_rect(&event->source, "source");
    if (rc) return rc;
    rc = validate_rect(&event->dest, "destination");
    if (rc) return rc;

    char present_basename[128];
    if (snprintf(present_basename, sizeof present_basename, "%s_present",
                 g.basename) < 0 ||
        strlen(present_basename) >= sizeof present_basename - 8) {
        set_error(D9_CAPTURE_LIMIT, "present basename is too long");
        return D9_CAPTURE_LIMIT;
    }

    char text_path[D9_CAPTURE_PATH_MAX];
    char binary_path[D9_CAPTURE_PATH_MAX];
    rc = choose_paths_named(present_basename, text_path, binary_path);
    if (rc) {
        set_error(D9_CAPTURE_LIMIT, "no free present capture name below %s/%s",
                  g.directory, present_basename);
        return rc;
    }

    uint64_t signature = d9_capture_present_signature(event);

    FILE *binary = fopen(binary_path, "wb");
    if (!binary) {
        set_error(D9_CAPTURE_IO, "cannot create %s", binary_path);
        return D9_CAPTURE_IO;
    }
    rc = write_present_binary(binary, event, signature);
    long binary_bytes = -1;
    if (!rc) binary_bytes = ftell(binary);
    if (fclose(binary) && !rc) rc = D9_CAPTURE_IO;
    if (rc || binary_bytes < 0) {
        if (!rc) rc = D9_CAPTURE_IO;
        remove(binary_path);
        if (!g.error[0])
            set_error(rc, "failed to write %s", binary_path);
        return rc;
    }

    FILE *text = fopen(text_path, "wb");
    if (!text) {
        set_error(D9_CAPTURE_IO, "cannot create %s", text_path);
        remove(binary_path);
        return D9_CAPTURE_IO;
    }
    rc = write_present_text(text, event, signature);
    long text_bytes = -1;
    if (!rc) text_bytes = ftell(text);
    if (fclose(text) && !rc) rc = D9_CAPTURE_IO;
    if (rc || text_bytes < 0) {
        if (!rc) rc = D9_CAPTURE_IO;
        remove(text_path);
        remove(binary_path);
        if (!g.error[0])
            set_error(rc, "failed to write %s", text_path);
        return rc;
    }

    D9CapturePresentInfo *info = &g.present_records[g.present_count];
    memset(info, 0, sizeof *info);
    info->index = g.present_count + 1;
    info->frame_index = event->frame_index;
    info->signature = signature;
    info->text_bytes = (size_t)text_bytes;
    info->binary_bytes = (size_t)binary_bytes;
    strcpy(info->text_path, text_path);
    strcpy(info->binary_path, binary_path);
    g.last_present_text[0] = 0;
    g.last_present_binary[0] = 0;
    strcpy(g.last_present_text, text_path);
    strcpy(g.last_present_binary, binary_path);
    g.present_count++;
    g.error[0] = 0;
    return D9_CAPTURE_OK;
}
