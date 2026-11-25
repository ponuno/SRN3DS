#pragma once
/* Standalone, bounded capture of the first D3D9 draw.
 *
 * This module deliberately does not know about the guest COM object table or
 * the D9Device internals.  A caller (the COM bridge, tests, a debugger shim)
 * fills a D9CaptureSnapshot and offers it to d9_capture_offer().  The module
 * validates every guest address it is given, bounds every variable-size array,
 * hashes all payloads with FNV-1a, and writes a human-readable text file plus a
 * little-endian binary file.
 *
 * Default policy is "first draw only".  Set max_records > 1 and
 * D9_CAPTURE_OPT_UNIQUE_ONLY to keep the first draw of each distinct signature.
 *
 * The capture is a diagnostic snapshot, not a replay engine.  It never
 * modifies guest memory and never overwrites an existing capture pair. */
#include <stddef.h>
#include <stdint.h>
#include "d3d9_core.h"
#include "guest.h"

#define D9_CAPTURE_SHADER_MAX_BYTES     (64u * 1024u)
#define D9_CAPTURE_MAX_VERTICES         256u
#define D9_CAPTURE_MAX_INDICES          1024u
#define D9_CAPTURE_TEXTURE_MAX_BYTES    (8u * 1024u * 1024u)
#define D9_CAPTURE_MAX_VERTEX_STRIDE    4096u
#define D9_CAPTURE_MAX_TEXTURES         4u
#define D9_CAPTURE_MAX_STREAMS          D9_MAX_STREAMS
#define D9_CAPTURE_MAX_DECL             D9_MAX_DECL
#define D9_CAPTURE_MAX_RECORDS          32u
#define D9_CAPTURE_MAX_PRESENTS         8u
#define D9_CAPTURE_PATH_MAX             320u
#define D9_CAPTURE_ERROR_MAX            256u
#define D9_CAPTURE_DEFAULT_BASENAME     "Skyrim_N3DS_firstdraw"
/* On-disk schema version for both files.  Version 1 (no render-target
 * identity, no Present event) remains readable; version 2 appends the target
 * section/keys and introduces the present event files. */
#define D9_CAPTURE_SCHEMA_VERSION       2u

/* Return values.  D9_CAPTURE_SKIP is a normal "policy says no" result. */
enum {
    D9_CAPTURE_OK = 0,
    D9_CAPTURE_SKIP = 1,
    D9_CAPTURE_INVALID = -1,   /* malformed snapshot or count/stride */
    D9_CAPTURE_POINTER = -2,   /* guest address is unmapped or truncated */
    D9_CAPTURE_IO = -3,        /* file create/write/close failed */
    D9_CAPTURE_CHANGED = -4,   /* guest payload changed between hash and write */
    D9_CAPTURE_LIMIT = -5      /* no free capture file name */
};

/* Reported in the text header, the binary header and D9CaptureRecordInfo. */
enum {
    D9_CAPTURE_TRUNC_VERTICES       = 1u << 0,
    D9_CAPTURE_TRUNC_INDICES        = 1u << 1,
    D9_CAPTURE_TRUNC_VS             = 1u << 2,
    D9_CAPTURE_TRUNC_PS             = 1u << 3,
    D9_CAPTURE_TRUNC_TEXTURE        = 1u << 4, /* a texture exceeded its share */
    D9_CAPTURE_TRUNC_TEXTURE_TOTAL  = 1u << 5, /* the 8 MiB total was exhausted */
    D9_CAPTURE_TRUNC_STREAMS        = 1u << 6,
    D9_CAPTURE_TRUNC_TEXTURES       = 1u << 7,
    D9_CAPTURE_TRUNC_DECL           = 1u << 8
};

enum {
    D9_CAPTURE_OPT_UNIQUE_ONLY = 1u << 0,
    D9_CAPTURE_OPT_DISABLED = 1u << 1
};

/* A host pointer and/or a guest address.  If host is non-NULL it is used;
 * otherwise guest is validated and read through GuestMemory.  bytes is the
 * number of bytes the caller guarantees are available at that address. */
typedef struct {
    const void *host;
    uint32_t guest;
    size_t bytes;
} D9CaptureBytes;

typedef struct {
    uint32_t x, y, width, height;
    float minz, maxz;
} D9CaptureViewport;

/* Identity is the guest COM object address (or any stable caller value).
 * guest/bytes describe the stream buffer and its known extent; this is
 * metadata only.  Vertices are carried separately below. */
typedef struct {
    unsigned index;
    unsigned handle;
    unsigned stride;
    size_t offset;
    size_t bytes;
    uint32_t guest;
} D9CaptureStream;

typedef struct {
    unsigned stage;
    unsigned handle;
    unsigned width, height, format, levels, pool, usage;
    unsigned level;          /* mip level the pixels slice describes */
    size_t row_pitch;
    D9CaptureBytes pixels;   /* raw level bytes, row-major */
} D9CaptureTexture;

typedef struct {
    D9CaptureBytes data;
    unsigned stride;
    unsigned count;
} D9CaptureVertices;

typedef struct {
    D9CaptureBytes data;
    unsigned size;           /* 2 or 4 */
    unsigned count;
} D9CaptureIndices;

typedef struct {
    unsigned type, start, primitives;
    unsigned indexed;
    int32_t base_vertex;
    unsigned min_vertex, vertex_count, start_index;
    D9CaptureVertices vertices;
    D9CaptureIndices indices;
} D9CaptureDraw;

typedef struct {
    unsigned active, count, position, uv, stream_mask;
    unsigned required[D9_CAPTURE_MAX_STREAMS];
    D9VertexElement elements[D9_CAPTURE_MAX_DECL];
} D9CaptureDeclaration;

typedef struct {
    uint32_t object;         /* COM shader object identity, optional */
    D9CaptureBytes code;     /* bytecode address/host pointer and byte length */
} D9CaptureShader;

typedef struct {
    unsigned valid;
    uint32_t color;
    uint32_t flags;
    unsigned rect_count;
} D9CaptureClear;

typedef struct {
    unsigned present;
    uint32_t flags;
    unsigned swap_chain;
} D9CapturePresent;

/* Active LDR render target for one draw, or the presented swap-chain target for
 * a Present event. handle is the guest COM object identity (or any stable caller
 * value); 0 always means the implicit back buffer and is recorded with implicit=1.  Pixels are never read
 * through this struct: width/height/format/usage are metadata only.
 * A nonzero handle combined with implicit=1 is contradictory and rejected. */
typedef struct {
    uint32_t handle;
    unsigned width, height;
    unsigned format;      /* D3DFORMAT value, e.g. D9_A8R8G8B8 */
    unsigned usage;       /* D3DUSAGE_* value */
    unsigned implicit;    /* 1 when this is the implicit back buffer */
} D9CaptureRenderTarget;

/* A D3D9 RECT.  valid=0 models a NULL RECT argument: the full target. */
typedef struct {
    int32_t left, top, right, bottom;
    unsigned valid;
} D9CaptureRect;

/* Everything the capture needs.  Zero-initialise and fill the parts that are
 * known; empty shaders/declarations/streams/textures are valid.  A zero target
 * is the legacy default and is recorded as the implicit back buffer. */
typedef struct {
    unsigned frame_index, draw_index;
    D9CaptureDraw draw;
    D9CaptureViewport viewport;
    D9DrawState state;
    D9Matrix world, view, projection;
    D9CaptureShader vs, ps;
    D9CaptureDeclaration declaration;
    unsigned stream_count;
    D9CaptureStream streams[D9_CAPTURE_MAX_STREAMS];
    unsigned texture_count;
    D9CaptureTexture textures[D9_CAPTURE_MAX_TEXTURES];
    D9CaptureClear clear;
    D9CapturePresent present;
    D9CaptureRenderTarget target;
} D9CaptureSnapshot;

typedef struct {
    const char *directory;   /* NULL: "sdmc:/" on ARM, "." on host */
    const char *basename;    /* NULL: D9_CAPTURE_DEFAULT_BASENAME */
    unsigned max_records;    /* 0: 1; capped at D9_CAPTURE_MAX_RECORDS */
    unsigned flags;          /* D9_CAPTURE_OPT_* */
    unsigned max_presents;   /* 0: 1; capped at D9_CAPTURE_MAX_PRESENTS */
} D9CaptureOptions;

typedef struct {
    unsigned index;                  /* 1-based capture ordinal */
    uint64_t signature;
    unsigned truncation;
    uint64_t vs_hash, ps_hash;
    unsigned vertex_count, vertex_captured;
    unsigned index_count, index_captured;
    size_t vertex_bytes, index_bytes;
    size_t vs_bytes, ps_bytes;
    size_t texture_bytes;
    unsigned texture_count, stream_count;
    size_t text_bytes, binary_bytes;
    char text_path[D9_CAPTURE_PATH_MAX];
    char binary_path[D9_CAPTURE_PATH_MAX];
} D9CaptureRecordInfo;

/* A standalone Present event.  The COM layer offers the first successful
 * Present even when no draw snapshot was ever written; the draw and present
 * paths are independent (separate counters, files and basename suffix).
 * source/dest use the D3D9 RECT convention: valid=0 is a NULL argument and
 * means the full target. */
typedef struct {
    unsigned frame_index;
    D9CaptureRenderTarget target;
    D9CaptureRect source;
    D9CaptureRect dest;
    uint32_t flags;        /* D3DPRESENT_* flags as passed by the guest */
    unsigned swap_chain;   /* 0 = the implicit swap chain */
} D9CapturePresentEvent;

typedef struct {
    unsigned index;        /* 1-based present ordinal */
    unsigned frame_index;
    uint64_t signature;
    size_t text_bytes, binary_bytes;
    char text_path[D9_CAPTURE_PATH_MAX];
    char binary_path[D9_CAPTURE_PATH_MAX];
} D9CapturePresentInfo;

/* Defaults: directory NULL (platform default), basename NULL (default),
 * max_records 1, max_presents 1, D9_CAPTURE_OPT_UNIQUE_ONLY. */
void d9_capture_options_init(D9CaptureOptions *options);
/* Starts a fresh session.  NULL options uses the defaults.  Repeated calls
 * reset the counters and the seen-signature table. */
int d9_capture_init(const D9CaptureOptions *options);
void d9_capture_shutdown(void);

/* Offer one draw.  Returns D9_CAPTURE_OK when a pair was written,
 * D9_CAPTURE_SKIP when policy/limit says no, or a negative error.  On error no
 * capture files are left behind and d9_capture_last_error() describes why.
 * Guest pointers are validated before anything is written. */
int d9_capture_offer(const D9CaptureSnapshot *snapshot, GuestMemory *memory);

/* Offer a standalone Present event.  Call once for the first successful
 * Present; this path does not need a draw snapshot, GuestMemory or pixel data
 * and does not consume a draw record slot.  Files are named
 * "<basename>_present[ _N ].txt/.bin" with the same no-overwrite policy.
 * Returns D9_CAPTURE_OK, D9_CAPTURE_SKIP (disabled / present cap reached) or a
 * negative error.  The present signature is FNV-1a over the event fields. */
int d9_capture_offer_present(const D9CapturePresentEvent *event);
/* True while initialized, enabled and below max_presents. */
int d9_capture_present_active(void);
unsigned d9_capture_present_count(void);
/* 0-based access to previously written present metadata. */
int d9_capture_present_record(unsigned index, D9CapturePresentInfo *out);
/* Preview a present signature without writing anything.  Returns 0 for NULL. */
uint64_t d9_capture_present_signature(const D9CapturePresentEvent *event);
const char *d9_capture_last_present_text_path(void);
const char *d9_capture_last_present_binary_path(void);

/* True while initialized, enabled, and below max_records.  COM can use this to
 * avoid building snapshots for every later draw. */
int d9_capture_active(void);
unsigned d9_capture_count(void);
unsigned d9_capture_remaining(void);

/* 0-based access to previously written record metadata. */
int d9_capture_record(unsigned index, D9CaptureRecordInfo *out);
const char *d9_capture_last_error(void);
const char *d9_capture_last_text_path(void);
const char *d9_capture_last_binary_path(void);

/* Standard 64-bit FNV-1a (offset basis 0xcbf29ce484222325, prime 1099511628211). */
uint64_t d9_capture_fnv1a(const void *data, size_t bytes);
/* Preview the draw signature without writing anything.  Returns 0 and stores
 * the signature, or returns a negative D9_CAPTURE_* error. */
uint64_t d9_capture_signature(const D9CaptureSnapshot *snapshot, GuestMemory *memory,
                              int *error);
/* "none", "vertices,indices,...", or "unknown".  Returns characters written. */
int d9_capture_truncation_text(unsigned flags, char *out, size_t size);
