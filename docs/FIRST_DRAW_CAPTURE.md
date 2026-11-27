# First D3D9 draw capture

Status: **integrated into the COM bridge and core.** The hook is bounded and
enabled by `TESV_D3D9_CAPTURE_DRAWS` on host; diagnostic 3DS builds capture the
first draw automatically because the console has no environment variables.
Version 2 adds the active render-target identity to every draw snapshot and a
standalone first-Present event that writes its own file pair even when no draw
has been captured. This document defines the snapshot contract, on-disk
formats, bounds and hook points.

## Files

| Path | Purpose |
| --- | --- |
| `source/d3d9_capture.h` | Snapshot schema, limits, options and public API. |
| `source/d3d9_capture.c` | Validation, FNV-1a hashing, text/binary writers, no-overwrite naming. |
| `tools/capture_raster.py` | Read-only, standard-library CPU rasterizer; writes RGBA PNG without a GPU. |
| `tools/capture_info.py` | Read-only text/JSON overview of the `.txt` capture snapshots (draws, presents, state, paired `.bin`). |
| `tests/d3d9_capture_smoke.c` | Host test: first draw, bounds/truncation, bad guest pointer, no-overwrite, unique signatures. |
| `tests/capture_raster_test.py` | Synthetic parser/raster/PNG tests for primitives, indices, colour, texture, blend and viewport. |
| `docs/FIRST_DRAW_CAPTURE.md` | This document. |

The module depends only on `guest.h` (page-safe guest reads) and
`d3d9_core.h` (shared `D9DrawState`, `D9Matrix`, `D9Decl` types). It does not
use the COM object table, TEV translation, citro3d, or platform APIs.

## Default policy

`d9_capture_init(NULL)` captures exactly one draw, the first snapshot that is
offered, and then stays inactive. The first snapshot is only accepted after
all guest pointers in it validate and both files have been written.

Use `D9CaptureOptions` for anything else:

```c
D9CaptureOptions options;
d9_capture_options_init(&options);        /* max_records=1, max_presents=1, UNIQUE_ONLY */
options.max_records = 8;                  /* at most 8 draw records      */
options.max_presents = 1;                 /* at most 1 present event     */
options.flags |= D9_CAPTURE_OPT_UNIQUE_ONLY; /* one per distinct signature */
options.directory = "sdmc:/";             /* NULL: sdmc:/ on ARM, "." on host */
options.basename = "Skyrim_N3DS_firstdraw";
if (d9_capture_init(&options) != D9_CAPTURE_OK) { ... }
```

`D9_CAPTURE_OPT_UNIQUE_ONLY` makes duplicate signatures free: a duplicate is
`D9_CAPTURE_SKIP` and does not consume a record slot or a file name.
`D9_CAPTURE_OPT_DISABLED` makes `d9_capture_active()` false and every
`d9_capture_offer()` return `D9_CAPTURE_SKIP` without touching the disk.
The directory and basename strings are copied during init. Before init,
`d9_capture_active()` is false and an offer is also a `D9_CAPTURE_SKIP`.

`D9_CAPTURE_MAX_RECORDS` is 32 and `D9_CAPTURE_MAX_PRESENTS` is 8; larger
values are clamped. Draw and Present records have independent counters, so a
Present capture never consumes a draw slot and vice versa.

## On-disk names

Defaults:

* ARM (`__3DS__`): `sdmc:/Skyrim_N3DS_firstdraw.txt` and
  `sdmc:/Skyrim_N3DS_firstdraw.bin`.
* Host: `Skyrim_N3DS_firstdraw.txt` and `Skyrim_N3DS_firstdraw.bin`
  in the current working directory.

A capture pair is **never overwritten**. Before writing, the module checks
both the `.txt` and `.bin` paths. If either exists, it tries `_2`, `_3`, ...,
up to 999, and uses the first pair where both files are absent. The actual
names are recorded in `D9CaptureRecordInfo.text_path` /
`binary_path` and are available through `d9_capture_last_text_path()` and
`d9_capture_last_binary_path()`. If no free pair exists the offer fails with
`D9_CAPTURE_LIMIT`.

The check plus create is not atomic; the module is single-threaded and expects
one guest thread to call it.

Present events use the configured basename plus `_present`, for example
`Skyrim_N3DS_firstdraw_present.txt` / `.bin`, with the same `_2`, `_3`, ...
search across the pair. This keeps the two event kinds visibly distinct even
when they share a directory.

## Bounds and truncation

Every variable-length input has a fixed upper bound. Counts above the bound
are **clamped** and reported with a truncation bit; a slice that is too small
for the bounded count is an **error**, not a silent partial capture.

| Data | Default bound | `TESV_D3D9_CAPTURE_FULL=1` | Flag |
| --- | --- | --- | --- |
| Vertex shader bytecode | `D9_CAPTURE_SHADER_MAX_BYTES` = 64 KiB | unchanged | `D9_CAPTURE_TRUNC_VS` |
| Pixel shader bytecode | 64 KiB | unchanged | `D9_CAPTURE_TRUNC_PS` |
| Vertices | `D9_CAPTURE_MAX_VERTICES` = 256 | 65,536 | `D9_CAPTURE_TRUNC_VERTICES` |
| Indices | `D9_CAPTURE_MAX_INDICES` = 1024 | 262,144 | `D9_CAPTURE_TRUNC_INDICES` |
| Texture pixels, all stages together | `D9_CAPTURE_TEXTURE_MAX_BYTES` = 8 MiB | unchanged | `D9_CAPTURE_TRUNC_TEXTURE`, `D9_CAPTURE_TRUNC_TEXTURE_TOTAL` |
| Streams | 16 (`D9_MAX_STREAMS`) | unchanged | `D9_CAPTURE_TRUNC_STREAMS` |
| Textures / stages | 4 | unchanged | `D9_CAPTURE_TRUNC_TEXTURES` |
| Declaration elements | 64 (`D9_MAX_DECL`) | unchanged | `D9_CAPTURE_TRUNC_DECL` |
| Draw record count | 32 | unchanged | `D9_CAPTURE_SKIP` after the cap |
| Present record count | 8 | unchanged | `D9_CAPTURE_SKIP` after the cap |

A vertex stride above 4096, an index size other than 2 or 4, or count×stride
overflow is `D9_CAPTURE_INVALID`. Vertex and index counts in the snapshot are
the counts the caller wants documented; by default the module captures at most
the first 256 vertices / 1024 indices and records both `declared` and `stored`
sizes.

`TESV_D3D9_CAPTURE_FULL=1` raises only the vertex/index bounds to 65,536 /
262,144. It also reconstructs an indexed triangle-list's compact source vertex
window when the snapshot carries a different scratch count. The reconstruction
uses the existing read-only `d9_com_device()`, stream/buffer getters and
`d9_decl_decode()`; it does not submit or mutate a draw. The text output records
`capture.full=1` and `vertices.source=decoded_compact` (or `snapshot` when the
caller already supplied the full vertex range). Standalone capture users that
supply a complete `vertices.count == draw.vertex_count` range need no COM device.

The truncation mask is written to the text header, the binary header, and
`D9CaptureRecordInfo.truncation`. `d9_capture_truncation_text()` renders it as
`none` or a comma-separated list such as
`vertices,indices,vs,ps,texture,texture_total,streams,textures,declaration`.

## Snapshot schema

`D9CaptureSnapshot` is plain data. No COM references are retained and the
module never writes guest memory. Important fields:

| Area | Fields |
| --- | --- |
| Draw | `type`, `start`, `primitives`, `indexed`, `base_vertex`, `min_vertex`, `vertex_count`, `start_index` |
| Geometry | `vertices` (`D9CaptureBytes`, stride, count) and `indices` (`D9CaptureBytes`, size, count) |
| Viewport | `x`, `y`, `width`, `height`, `minz`, `maxz` |
| Fixed-function state | `D9DrawState` (Z, cull, alpha, blend, color write, sampler, resident mips) |
| Transforms | `world`, `view`, `projection` (`D9Matrix`, D3D row-vector convention) |
| Shaders | `vs`/`ps`: COM object identity plus `D9CaptureBytes` bytecode |
| Input layout | `D9CaptureDeclaration`: active/count/position/uv/stream_mask, `required[16]`, up to 64 elements |
| Streams | up to 16 `D9CaptureStream` entries (index, handle, stride, offset, bytes, guest address) |
| Textures | up to 4 `D9CaptureTexture` entries (stage, handle, width, height, format, levels, pool, usage, captured mip level, row pitch, raw pixels) |
| Frame | `frame_index`, `draw_index`, `clear` (valid/color/flags/rect_count), `present` (present/flags/swap_chain) |
| Render target | `target`: active object handle, width, height, format, usage, implicit flag |

`D9DrawState.pixel_shader` is a borrowed pointer and is **not dereferenced**.
Only its presence is recorded (`state.pixel_shader=0/1`). TEV/TEX program
contents are not part of this snapshot.

### Render-target identity

```c
typedef struct {
    uint32_t handle;      /* guest COM render-target object; 0 = implicit back buffer */
    unsigned width, height;
    unsigned format;      /* D3DFORMAT value, e.g. D9_A8R8G8B8 = 21 */
    unsigned usage;       /* D3DUSAGE_* value */
    unsigned implicit;    /* 1 when this is the implicit back buffer */
} D9CaptureRenderTarget;
```

* `handle == 0` always means the implicit back buffer. A zero-initialised
  `D9CaptureSnapshot` therefore records the legacy default as
  `target.handle=0`, `target.implicit=1`; no caller change is required to keep
  capturing.
* A nonzero `handle` with `implicit=1`, or `implicit` greater than 1, is
  contradictory and rejected with `D9_CAPTURE_INVALID`.
* `width`, `height`, `format` and `usage` are metadata. The capture module
  never reads, copies or derives pixels from the active target. Zero values
  mean "not known" and are recorded verbatim.
* The field is appended at the **end** of `D9CaptureSnapshot`, so existing
  designated/field-wise initialisation keeps compiling. The struct is plain
  data; zero-initialise it as before.

`D9CaptureBytes` is a union-like slice:

```c
typedef struct {
    const void *host;  /* used when non-NULL */
    uint32_t guest;    /* used when host == NULL; validated through GuestMemory */
    size_t bytes;      /* bytes available at that address */
} D9CaptureBytes;
```

* If `host != NULL`, the bytes are read directly. The caller owns them and
  must keep them valid until `d9_capture_offer()` returns.
* Otherwise `guest` is read with `guest_read()`, page by page, so a guest
  mapping may span several pages/regions. An unmapped page fails the whole
  offer with `D9_CAPTURE_POINTER`; no file is written.
* A zero-length slice needs neither pointer and hashes to the FNV-1a empty
  value `0xcbf29ce484222325`.
* `bytes` is the guaranteed extent. For shaders it is the exact byte length;
  vertices/indices are `count * stride`; textures are the level's raw byte
  count (not pixels × 4 if the format is compressed).

Raw pixels are copied exactly as provided. The module does **not** swizzle,
untile, decode BC/ETC1, or convert formats. `format`, `row_pitch`, `level`,
dimensions and pool/usage are recorded so a later tool can interpret them.

## Standalone Present event

The first successful `IDirect3DDevice9::Present` is captured through a separate
API, even when no draw snapshot was ever offered:

```c
typedef struct {
    unsigned frame_index;
    D9CaptureRenderTarget target;  /* same struct as the draw snapshot */
    D9CaptureRect source, dest;    /* valid=0 == NULL RECT == full target */
    uint32_t flags;                /* D3DPRESENT_* flags passed to Present */
    unsigned swap_chain;           /* 0 = the implicit swap chain */
} D9CapturePresentEvent;

typedef struct {
    unsigned index;                /* 1-based present ordinal */
    unsigned frame_index;
    uint64_t signature;
    size_t text_bytes, binary_bytes;
    char text_path[D9_CAPTURE_PATH_MAX];
    char binary_path[D9_CAPTURE_PATH_MAX];
} D9CapturePresentInfo;

int d9_capture_offer_present(const D9CapturePresentEvent *event);
int d9_capture_present_active(void);
unsigned d9_capture_present_count(void);
int d9_capture_present_record(unsigned index, D9CapturePresentInfo *out);
uint64_t d9_capture_present_signature(const D9CapturePresentEvent *event);
const char *d9_capture_last_present_text_path(void);
const char *d9_capture_last_present_binary_path(void);
```

`D9CaptureRect` carries `left`, `top`, `right`, `bottom` and `valid`. `valid=0`
models the NULL pointer argument: the full target. An inverted valid rect
(`right < left` or `bottom < top`) is `D9_CAPTURE_INVALID`. Negative coordinates
are legal and recorded as signed 32-bit values.

`d9_capture_offer_present()` requires no `GuestMemory` and no pixel pointer.
It validates the target identity and rects, then writes
`<basename>_present.txt` and `<basename>_present.bin` with the usual
no-overwrite search. It does not touch draw counters or `d9_capture_active()`.
Policy:

* disabled, uninitialised, or `present_count >= max_presents` → `D9_CAPTURE_SKIP`;
* contradictory implicit target, bad rect, or NULL event → `D9_CAPTURE_INVALID`;
* otherwise `D9_CAPTURE_OK` and a present record is appended.

The present signature is 64-bit FNV-1a with domain string `D9PRE1`; it covers
the frame index, flags, swap chain, both rects and the target identity. It is
only an identity aid, not used for deduplication.

## Failure and return codes

`d9_capture_offer()` returns:

| Value | Meaning |
| --- | --- |
| `D9_CAPTURE_OK` (0) | Files written; record appended. |
| `D9_CAPTURE_SKIP` (1) | Disabled, limit reached, or duplicate signature under `UNIQUE_ONLY`. |
| `D9_CAPTURE_INVALID` (-1) | Malformed snapshot, bad count/stride/index size, or a slice too small. |
| `D9_CAPTURE_POINTER` (-2) | Guest address missing/unmapped, or `GuestMemory` not supplied. |
| `D9_CAPTURE_IO` (-3) | File create/write/close failed. |
| `D9_CAPTURE_CHANGED` (-4) | Guest bytes changed between the signature pass and the write pass. |
| `D9_CAPTURE_LIMIT` (-5) | No free `_N` name remains. |

On any negative result, partially written files are removed and
`d9_capture_last_error()` contains a diagnostic. A failed offer does not
consume a record slot. `d9_capture_last_error()` is cleared at the start of
each offer and on a successful signature preview.

The capture must never change a guest HRESULT. The COM hook logs
`d9_capture_last_error()` and continues with the original draw result.

## Signature

The signature is 64-bit FNV-1a, domain string `D9CAP1`
(offset `0xcbf29ce484222325`, prime `1099511628211`). It covers:

* draw type, start, primitives, indexed, base vertex, min vertex, vertex
  count and start index;
* active render-target handle/implicit flag, dimensions, format and usage;
* VS/PS payload hashes plus declared and stored byte counts;
* declaration active/count/position/uv/stream mask, all 16 `required[]`
  values, and each captured element;
* stream index, stride, offset and size (not the guest identity);
* vertex stride/count and the hash of the stored (bounded) vertex bytes;
* index size/count and the hash of the stored index bytes;
* for each texture: stage, dimensions, format, levels, pool, usage, captured
  level, row pitch, declared byte count and payload hash (not the handle).

It deliberately **excludes** the viewport, `D9DrawState`, world/view/projection
matrices, clear/present state, frame/draw counters and COM shader/texture/stream
identities. Those are recorded in the files but do not make a draw "unique";
including every state value would make almost every draw a new signature. The
active render target *is* included so identical geometry into two targets is not
silently deduplicated.
Callers that want every draw can clear `D9_CAPTURE_OPT_UNIQUE_ONLY` and manage
`max_records`.

Including the target block is a schema-version-2 signature change. Version-1
captures remain readable, but their signatures predate target identity. Present
events use the separate `D9PRE1` signature described above and never share the
draw signature table.

Payload hashes cover only the bytes that are actually stored (first 64 KiB of
a shader, first 256 vertices, first 1024 indices, up to the 8 MiB texture
budget). `d9_capture_signature()` computes the same value without writing.

## Text output

The `.txt` file is a human summary. It starts with:

```text
format=d9_capture
version=2
signature=0x...
truncation=0x00000000
truncation.names=none
frame.index=...
draw.index=...
```

It then contains the draw fields, viewport, every `D9DrawState` field, four
rows per matrix, VS/PS object/bytes/stored/truncated/hash, declaration
metadata and elements, stream metadata, vertex/index declared/captured sizes
and hashes, every texture's metadata and pixel hash, clear/present fields and
the render-target block:

```text
target.handle=0x50004000
target.implicit=0
target.width=400
target.height=240
target.format=21
target.usage=0
```

Bytecode and pixel **payloads are only in the binary file**; the text keeps
their lengths, hashes and truncation status so it stays small.

A present text file is self-describing and has no payloads:

```text
format=d9_capture_present
version=2
signature=0x...
present.frame_index=...
present.flags=0x...
present.swap_chain=...
present.source.valid=0        # 0: NULL source rect, full target
present.source.rect=0,0,0,0
present.dest.valid=0
present.dest.rect=0,0,0,0
target.handle=0x00000000
target.implicit=1
target.width=400
target.height=240
target.format=21
target.usage=0
pixels.captured=0
```

`pixels.captured=0` is explicit: a Present event records identity and geometry,
never framebuffer contents.

## Binary output

All integers are little-endian; floats are IEEE-754 bit patterns written as
`uint32_t`. The file starts with a 32-byte header:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | `char[8]` | `"D9CAPBIN"` |
| 8 | `u32` | version, currently `2` (version `1` is readable) |
| 12 | `u32` | header bytes, `32` |
| 16 | `u64` | draw signature |
| 24 | `u32` | truncation mask |
| 28 | `u32` | section count |

The rest is a sequence of `{u32 id, u32 payload_size, payload}` records. All
sections are independent so a reader can skip unknown ones. The final section
is `SECTION_END` with size 0. For version 2 the header's section count is
`14 + stream_count + texture_count`; a version-1 file has one section fewer
because `TARGET` is absent. Current IDs:

| ID | Name | Payload |
| --- | --- | --- |
| 1 | DRAW | 8×`u32`: type, start, primitives, indexed, base, min, vertex_count, start_index |
| 2 | VIEWPORT | 4×`u32` + 2×`f32`: x, y, width, height, minz, maxz |
| 3 | DRAWSTATE | 20×`u32` in `D9DrawState` order + pixel-shader presence |
| 4 | WORLD | 16×`f32`, row-major |
| 5 | VIEW | 16×`f32`, row-major |
| 6 | PROJECTION | 16×`f32`, row-major |
| 7 | VS | shader metadata (32 bytes) + bytecode |
| 8 | PS | shader metadata (32 bytes) + bytecode |
| 9 | DECLARATION | 5×`u32`, 16×`u32` required, then 8 bytes per element |
| 10 | STREAM | 40 bytes per stream |
| 11 | VERTICES | 40-byte metadata + stored vertex bytes |
| 12 | INDICES | 40-byte metadata + stored index bytes |
| 13 | TEXTURE | 76-byte metadata + stored pixels |
| 14 | FRAME | 9×`u32`: frame, draw, clear valid/color/flags/rects, present/flags/swap chain |
| 15 | END | empty |
| 16 | TARGET | 6×`u32`: handle, implicit, width, height, format, usage |
| 17 | PRESENT | 20×`u32` in a present-only file (see below) |

Shader metadata is `u32 object, u32 reserved, u64 hash, u64 declared_bytes,
u32 stored_bytes, u32 truncated`, followed by the stored bytecode.

Declaration elements are written as two `u32`s: `stream | offset<<16` and
`type | method<<8 | usage<<16 | usage_index<<24` (matching the 8-byte
`D9VertexElement` layout).

Stream payload: `u32 index, handle, stride, reserved, u64 offset, u64 bytes,
u32 guest, u32 reserved`.

Vertex payload: `u32 stride, declared_count, captured_count, stored_bytes,
u64 declared_bytes, u64 hash, u32 truncated, u32 reserved`.

Index payload: `u32 size, declared_count, captured_count, stored_bytes,
u64 declared_bytes, u64 hash, u32 truncated, u32 reserved`.

Texture metadata: `u32 stage, handle, width, height, format, levels, pool,
usage, level, truncated, reserved, u64 row_pitch, u64 declared_bytes,
u64 stored_bytes, u64 hash`.

`declared_bytes` is the caller's full extent; `stored_bytes` is what follows.
For shaders/vertices/indices `truncated` is `stored < declared`. For textures,
the same relation reports either the total budget or a per-texture cut.

`TARGET` is written after `FRAME` and before `END` in every version-2 draw
file. The target payload is `u32 handle, implicit, width, height, format,
usage`. A version-1 reader that trusts payload sizes skips it unchanged; a
version-2 reader of a version-1 file treats the target as unknown/implicit.

Present captures are a separate file with the 8-byte magic `D9CAPPRE`, the same
32-byte header layout, version 2, an event signature at offset 16, truncation 0,
section count 2, one `PRESENT` section and `END`. The `PRESENT` payload is 20
`u32` values:

```text
frame_index, flags, swap_chain,
source.valid, source.left, source.top, source.right, source.bottom,
dest.valid, dest.left, dest.top, dest.right, dest.bottom,
target.handle, target.implicit, target.width, target.height,
target.format, target.usage, reserved(0)
```

Rect coordinates are two's-complement `i32` bit patterns; `valid=0` means the
corresponding NULL argument, i.e. the full target. No pixel bytes follow.

## Versioning and compatibility

`D9_CAPTURE_SCHEMA_VERSION` is `3`. Version 3 appends five sampler values to
the `DRAWSTATE` section (payload `80` -> `100` bytes) and to the text output:
`state.address_w`, `state.lod_bias` (decoded float, PICA-clamped),
`state.max_anisotropy`, `state.srgb`, `state.border_color` (`0xAARRGGBB`).
Text readers should branch on the `version=` line and tolerate unknown keys;
binary readers should walk `{id, size}` records and skip unknown ids. Version-1
files remain fully readable: they simply have no `TARGET` section and no
`target.*` text keys. Version-2 files lack only the five sampler keys above.
Draw signatures are unchanged between versions, so a `UNIQUE_ONLY` comparison
across old and new files remains meaningful.

## Hash details

* Every payload hash is FNV-1a 64 over the stored bytes.
* The empty payload hash is `0xcbf29ce484222325`.
* `d9_capture_fnv1a("",0)`, `("a",1)` and `("foobar",6)` are covered by the
  smoke test against the standard vectors.
* Text and binary use the same hashes.

## Applied COM integration

### Build wiring

The module is wired into the production COM/TESV builds and the isolated ARM
probe:

* `d3d9_capture.o` is linked wherever `d3d9_com.o` is linked, including the main
  Makefile and `tests/isolated/uicopy/Makefile`.
* `tools/run_host_tests.ps1` registers `d3d9_capture`; `null_backend` also covers
  the COM draw/Present integration.
* `d3d9_com.c` includes `d3d9_capture.h` when `D9_CAPTURE_HOOK` is enabled.

Source-only worktrees do not carry the git-ignored `staged/` and generated D3DX
pilot inputs. `N3DS_STAGED_DIR` and `N3DS_PILOT_GEN` may point at read-only
copies outside the worktree; `fast_slice.ps1` inherits both. This avoids creating
junctions or modifying `staged/*` in the isolated tree.

### Initialization and shutdown

Initialize once in `d9_com_start()`, before any device can be created. The COM
layer reads `TESV_D3D9_CAPTURE_DRAWS`; on ARM diagnostic builds the default is one
draw. The implementation is:

```c
const char *requested = getenv("TESV_D3D9_CAPTURE_DRAWS");
#ifdef __3DS__
if (!requested || !*requested) requested = "1";
#endif
if (requested && *requested) {
    char *end = NULL;
    unsigned long n = strtoul(requested, &end, 10);
    if (end != requested && n) {
        D9CaptureOptions options;
        d9_capture_options_init(&options);
        if (n > D9_CAPTURE_MAX_RECORDS) n = D9_CAPTURE_MAX_RECORDS;
        options.max_records = (unsigned)n;
        options.flags |= D9_CAPTURE_OPT_UNIQUE_ONLY;
        if (d9_capture_init(&options) != D9_CAPTURE_OK)
            fprintf(stderr, "[d3d9-capture] init: %s\n", d9_capture_last_error());
    }
}
```

Call `d9_capture_shutdown()` in `d9_com_shutdown()` and in device destruction.

### Exact per-draw hook points

In `source/d3d9_com.c`:

* `static void draw_primitive(CPU *c)` (slot `DEVICE,81`, currently around
  line 660): after `shader_state()` returns 0 and **after**
  `d9_draw_primitive()` returns `D9_OK`, fill a snapshot and call
  `d9_capture_offer(&snapshot, g.mem)` before `RET(hr(rc),4)`.
* `static void draw_indexed(CPU *c)` (slot `DEVICE,82`, around line 661):
  identical, after `d9_draw_indexed_range()` returns `D9_OK`, before
  `RET(hr(rc),7)`.

Capturing after the core call means only draws the backend accepted are
recorded, and the core's decoded `d->scratch` is available. Guard the whole
block with `d9_capture_active()` so later draws cost only one branch.

**Do not** call the capture from `d3d9_core.c`'s internal `draw()` unless the
COM layer is deliberately bypassed. The module is a diagnostic consumer, not
part of the core draw contract.

### Snapshot field sources

| Snapshot | Current source / new getter |
| --- | --- |
| `draw.type/start/primitives` | `A(1)`, `A(2)`, `A(3)` in `draw_primitive`; `A(1)`, `A(5)`, `A(6)` in `draw_indexed` (both `start` and `start_index` carry `StartIndex`) |
| `draw.indexed/base_vertex/min_vertex/vertex_count/start_index` | `draw_indexed`: 1, `A(2)`, `A(3)`, `A(4)`, `A(5)`; zero for `draw_primitive` |
| `viewport` | `g.viewport[0..3]` are `x,y,w,h`; `g.viewport[4..5]` are the `minz/maxz` float bits |
| `state` | New `d9_get_draw_state(g.device, &snapshot.state)` (currently `d->state`) |
| `world/view/projection` | New `d9_get_transform(g.device, which, &matrix)` for `D3DTS_WORLD` (256), `D3DTS_VIEW` (2), `D3DTS_PROJECTION` (3) |
| `vs`/`ps` | `g.vs`/`g.ps`: `object = o->guest`, `code.host = o->code`, `code.bytes = o->words*4` (stable while bound) |
| `declaration` | `g.declaration ? g.declaration->declaration : {0}`; copy count/elements. The FVF path has no `D9Decl`; set `active=0` |
| `streams` | `g.streams[s]`, `g.offsets[s]`, `g.strides[s]`; `handle` is `g.streams[s]->guest`; `bytes` from the buffer descriptor |
| vertices | New `d9_get_buffer_data()` plus the computed first vertex/count (see below) |
| indices | `g.ib` (`handle`, `format`, size) plus `d9_get_buffer_data()` and `start_index` |
| textures | `g.tex` only today: metadata from the `Obj` fields, pixels via new `d9_get_texture_capture()`; set `stage=0`, `texture_count=1` |
| `clear` | `g.frame_clear` says whether this frame actually called `Clear`; `g.clear`/`g.clear_flags` carry its values; `rect_count=0` (the COM rejects rectangle clears) |
| `present` | Remembered in the `Present` handler: `present=1`, `flags=A(4)`, `swap_chain=0`; reset after the next captured draw or at `BeginScene` |
| `target` | `g.rt`: `handle=0, implicit=1` for `g.back`; otherwise `handle=g.rt->guest`, `implicit=0`, with width/height/format/usage from the COM surface object |
| `frame_index/draw_index` | Counters maintained in the COM layer, incremented at `BeginScene`/each successful draw |

The `D3DTS_WORLD/VIEW/PROJECTION` matrices are the D3D transforms. Do not put
`g.constants` there: `g.constants` holds the four `c0..c3` shader clip rows
fed through `d9_set_shader_clip()`, which is a separate path.

### Existing core getters

Add these to `source/d3d9_core.h` and implement in `source/d3d9_core.c`
(staying in the core, so the capture module remains independent):

```c
int d9_get_draw_state(const D9Device *d, D9DrawState *out);
int d9_get_transform(const D9Device *d, unsigned which, D9Matrix *out);
int d9_get_stream_info(const D9Device *d, unsigned stream, D9Handle *buffer,
                       size_t *offset, size_t *stride);
int d9_get_index_info(const D9Device *d, D9Handle *buffer, unsigned *format);
int d9_get_buffer_data(const D9Device *d, D9Handle buffer,
                       const void **data, size_t *bytes);
int d9_get_texture_capture(const D9Device *d, D9Handle texture, unsigned level,
                           const void **pixels, size_t *pitch, size_t *bytes,
                           unsigned *width, unsigned *height, unsigned *format);
int d9_get_last_draw_vertices(const D9Device *d, const D9Vertex **vertices,
                              size_t *count);
```

Semantics:

* `d9_get_draw_state` copies `d->state` (the pointer `pixel_shader` stays
  borrowed; the capture only records its presence).
* `d9_get_transform` copies `d->transforms[which]`; reject `which > 2`.
* `d9_get_stream_info` returns `d->streams[stream].id/offset/stride`.
* `d9_get_index_info` returns `d->index` and the index buffer format.
* `d9_get_buffer_data` returns the buffer's host `data` pointer and `bytes`;
  valid until unlock/release. The caller must not modify it during capture.
* `d9_get_texture_capture` returns the host `pixels` pointer for the requested
  level, its pitch, byte size, and level dimensions/format.
* `d9_get_last_draw_vertices` returns `d->scratch` and the last draw's vertex
  count. This is the decoded `XYZ + UV` stream after `d9_draw_*()` and is the
  simplest way to capture exactly what was drawn. Pair it with
  `stride = sizeof(D9Vertex)` and `vertices.count = count`; the raw index
  buffer can still be captured separately.

If host pointers from these getters are used, set `D9CaptureBytes.host` and
leave `guest = 0`. If the COM layer instead forwards guest lock windows, set
`guest` and let the module validate. Both paths are supported.

### Vertex/index range selection

For a raw-stream snapshot (rather than decoded `d->scratch`):

* Non-indexed list: first vertex = `draw.start`, count = `3*primitives`.
* Non-indexed strip/fan: first vertex = `draw.start`, count = `primitives+2`.
* Indexed: the COM `draw_indexed` already receives `MinIndex` (`A(3)`) and
  `NumVertices` (`A(4)`), so first vertex = `base_vertex + min_vertex` and
  count = `vertex_count`; the default module cap is 256, or 65,536 with
  `TESV_D3D9_CAPTURE_FULL=1`.
* Indices: first index = `start_index`, count = the same primitive vertex
  count; the default module cap is 1024, or 262,144 in full mode, and the
  declared count is recorded.
* Byte offset for raw streams is
  `stream.offset + first_vertex * stream.stride`; for indices it is
  `start_index * index_size`. Use `d9_get_buffer_data()` to obtain the base
  pointer and validate the arithmetic in the COM layer.

### Shader bytecode pointer

`shader_new()` currently copies the guest token stream into `Obj.code` and
drops the original address. Two valid options:

1. Use the retained host copy: `vs.code.host = g.vs->code`,
   `vs.code.bytes = g.vs->words*4`, `vs.object = g.vs->guest`. This is the
   recommended option; it captures exactly the validated bytecode including
   comments.
2. Add `uint32_t source;` to `Obj`, set it in `shader_new()` after validation,
   and pass `vs.code.guest = o->source`, `vs.code.bytes = o->words*4` so the
   module exercises its guest-pointer validation and the on-disk copy is the
   guest's original bytes.

Either way, do not hold the pointer past the bound shader's lifetime.

### Clear and Present hooks

* `static void clear(CPU *c)` (slot `DEVICE,43`, around line 559): after a
  successful clear, set the COM-side `g.clear`, `g.clear_flags`,
  `g.clear_pending` (already done) and copy them into the next snapshot.
* `static void begin_scene(CPU *c)` (slot `DEVICE,41`): on success bump the
  frame counter, reset the per-frame draw counter, and latch the pending clear.
* `static void present(CPU *c)` (slot `DEVICE,17`): after `d9_present()` succeeds,
  set `present=1`, `flags=A(4)`, `swap_chain=0` in the frame context. The next
  draw snapshot reports it. The current COM rejects every nonzero `Present`
  argument, so source/destination rects and flags are normally zero/NULL.
* Also in `present`, after success, `capture_present_event()` offers one event
  whenever `d9_capture_present_active()` is true. The module's independent cap
  and counter enforce the first-Present policy; no extra COM latch is needed.
  The event uses the implicit swap-chain back buffer identity, because D3D9
  `Present` always presents that back buffer even if an offscreen render target
  is still bound. The active offscreen target remains visible in draw snapshots.
  Because nonzero Present arguments are rejected, both RECTs have `valid=0`.

The draw and Present hooks are independent producers. A Present-only run (for
example a device that clears and presents without drawing) still writes its
present pair; a draw-only run still writes the draw pair.

### Failure policy at the hook

```c
if (d9_capture_active()) {
    D9CaptureSnapshot snapshot;
    if (build_snapshot_from_com(&snapshot, ...) != 0) {
        fprintf(stderr, "[d3d9-capture] snapshot: %s\n", d9_capture_last_error());
    } else {
        int cr = d9_capture_offer(&snapshot, g.mem);
        if (cr < 0)
            fprintf(stderr, "[d3d9-capture] offer %d: %s\n", cr,
                    d9_capture_last_error());
    }
}
/* The guest-visible HRESULT is unaffected by capture success/failure. */
RET(hr(rc), 4);
```

The snapshot contains borrowed host/guest pointers only for the duration of
`d9_capture_offer()`. Do not unlock buffers or release textures until it
returns. Call the hook on the guest thread; the module uses static state and
is not reentrant.

## Verification

Host (clang, no Windows-specific dependencies beyond the CRT):

```powershell
clang -std=gnu11 -O1 -g -Isource `
  source/d3d9_capture.c source/guest.c tests/d3d9_capture_smoke.c `
  -o d3d9_capture_smoke.exe
.\d3d9_capture_smoke.exe
```

ARM syntax/compile:

```powershell
arm-none-eabi-gcc -std=gnu11 -O2 -Wall -Wextra -march=armv6k -mtune=mpcore `
  -mfloat-abi=hard -mtp=soft -mword-relocations -D__3DS__ -Isource `
  -c source/d3d9_capture.c -o d3d9_capture.o
```

The smoke test covers:

* FNV-1a standard vectors and truncation-name rendering;
* default first-draw capture, text/binary header consistency, section count,
  getters and paths;
* all bound clamps: 300 vertices → 256, 2000 indices → 1024, 70000-byte
  shaders → 65536, 8 MiB + 4096 texture → 8 MiB, plus stream/declaration/
  texture-count clamps;
* unmapped shader and texture guest pointers (`D9_CAPTURE_POINTER`), too-small
  vertex slice and bad index size (`D9_CAPTURE_INVALID`), with no files left;
* no-overwrite suffix search when the base `.txt` and the `_2` `.bin` already
  exist (writes `_3`, leaves both sentinels untouched);
* unique-signature limiting across duplicate and distinct draws, including a
  draw into a different render target;
* render-target identity in the text file and `TARGET` binary section, plus
  legacy zero-target normalisation to the implicit back buffer;
* a standalone Present capture with no draw: header/magic/section layout,
  NULL rects (`valid=0`), implicit and explicit targets, `_present` naming,
  independent counters, present-cap skip and contradictory/inverted input
  rejection with no files left behind;
* disabled mode (draws and presents) and `NULL` default options;
* COM integration in `null_backend`: an explicit target on the first draw and
  standalone implicit-backbuffer Present files.

## Known limitations / next steps

* Present capture records identity, dimensions, format and rects only; it does
  not read back framebuffer pixels. `d9_render_target_read()` now exists for
  explicit targets, but capture deliberately does not invoke it: readback is a
  GPU synchronization/staging operation and remains an explicit surface-lock
  feature.
* The COM hook forwards active render-target identity and invokes the standalone
  Present producer. Present RECTs remain `valid=0` because this bridge rejects
  nonzero Present arguments; the capture module cannot discover them itself.
* The FVF path has no `D9Decl`; the snapshot records `declaration.active=0`.
  A future field could record the raw FVF value.
* Only the active texture stage 0 is currently available from COM, although
  the schema and writer support four stages. GPU-only SKTX textures are recorded
  with metadata and an empty pixel payload rather than omitted.
* Pixels are raw level bytes; interpretation (BGRA, DXT, ETC1) is left to the
  reader.
* The draw signature intentionally omits transforms, render state and target
  identity; see above.
* Binary output is uncompressed. The texture budget caps the worst case at
  8 MiB + ~1.3 MiB of shaders/geometry + metadata. Present files are tiny
  (80-byte payload).

## Host PNG rasterizer

`tools/capture_raster.py` turns draw captures into a diagnostic RGBA8 PNG on the
host. It uses only Python's standard library (`struct`, `array`, `zlib`,
`concurrent.futures`); there is no GPU, Pillow, NumPy, emulator, or device step.
Input files are opened read-only. An output path is always explicit, so the tool
never writes beside a capture implicitly.

### Usage

From the repository root:

```powershell
# One draw. A .txt argument is also accepted and resolves to its .bin sibling.
python tools/capture_raster.py E:\menu_capture\run_tesv_slice_8260\Skyrim_N3DS_firstdraw.bin `
  --output "$env:TEMP\firstdraw.png"

# Compose distinct draws in argument order. Clear is applied on the first draw
# (and again only when frame.index changes), not once per draw.
python tools/capture_raster.py `
  E:\menu_capture\run_tesv_slice_8260\Skyrim_N3DS_firstdraw.bin `
  E:\menu_capture\run_tesv_slice_8260\Skyrim_N3DS_firstdraw_2.bin `
  E:\menu_capture\run_tesv_slice_8260\Skyrim_N3DS_firstdraw_3.bin `
  E:\menu_capture\run_tesv_slice_8260\Skyrim_N3DS_firstdraw_4.bin `
  --output "$env:TEMP\firstdraw_sequence.png" --jobs 4
```

`--jobs 1..16` parallelizes independent file parsing and FNV-1a validation.
Rasterization remains ordered because later draws can blend into earlier ones.
The default is up to 16 parsers, bounded by the number of inputs. The summary
reports dimensions, covered fragments, non-zero pixels, their inclusive bounding
box, signatures and wall time. `D9CAPPRE` Present files are rejected: they contain no framebuffer bytes.

The binary file is authoritative; the sibling `.txt` is useful for inspection
but is not needed for rendering. The reader checks `D9CAPBIN`, the 32-byte
little-endian header, section bounds, required payload sizes and every stored
shader/vertex/index/texture FNV-1a hash. It reads schema v3 and also accepts the
v1/v2 section variants (missing target/sampler fields get documented defaults).

### Geometry and position semantics

Supported primitive types are the three D3D9 triangle types:
`D3DPT_TRIANGLELIST=4`, `D3DPT_TRIANGLESTRIP=5`, and
`D3DPT_TRIANGLEFAN=6`, both indexed and non-indexed. Strip winding alternates;
the fan keeps vertex zero as its centre. Index buffers may be 16- or 32-bit.

Vertex payloads are the capture hook's **decoded host structures**, not the raw
guest declaration stream:

* 20-byte `D9Vertex { x, y, z, u, v }` is the current COM output. It has no
  COLOR0, so the rasterizer uses opaque white vertex colour and modulates that
  with the sampled texture.
* 40-byte `D9ColorVertex { x, y, z, u, v, r, g, b, a, w }` is also recognized.
  Its normalized float RGBA is interpolated; the trailing TEXCOORD0.w is not
  used by the 2D blit. Full compact indexed capture emits this layout after
  decoding the declaration's POSITION/TEXCOORD0/optional COLOR0.
* Other strides fail with a named error instead of guessing a raw FVF layout.

`--position-space auto|screen|clip|object` controls positions:

* `screen`: pretransformed D3D9 window pixels, offset by viewport x/y. This is
  the POSITIONT/XYZRHW case.
* `clip`: homogeneous D3D9 clip positions; x/y/w and z/w are mapped through the
  viewport.
* `object`: row-vector world × view × projection, then viewport mapping. This
  is the fixed-function POSITION case.
* `auto`: an active declaration's POSITIONT selects `screen`; POSITION selects
  `object` when no VS bytecode is present. A POSITION+VS draw requires an
  explicit choice because shader constants are absent. With an inactive
  declaration and no VS, `auto` assumes `screen` and warns: the raw FVF value is
  not captured, so XYZ versus XYZRHW cannot be recovered from v3 alone.

The current COM hook stores `d->scratch`, which is already expanded into
primitive order and already offset by `draw.start`. The rasterizer recognizes
that layout and does not apply topology a second time. It also accepts a
conventional compact non-indexed range. For indexed captures, raw indices are
always parsed and hash-checked. When vertices are the expanded scratch stream,
the indices are **not** applied again (doing so would index the expanded list a
second time); compact vertex payloads use 16/32-bit indices plus `base_vertex`.
The index payload is already sliced at `start_index` by the COM hook, so the
rasterizer does not add `start_index` again.

Rasterization uses pixel centres, viewport clipping, a top-left fill rule and
D3D9 cull modes. Colour/UV use perspective-correct interpolation. A basic
depth buffer and all eight D3DCMP depth functions are supported. Triangles with
non-positive w or vertices outside the D3D9 near/far range are skipped; general
polygon clipping is not implemented.

### Textures, alpha and blending

The first captured stage-0 texture is sampled. Supported raw formats are:

| Format | Value | Decode |
| --- | ---: | --- |
| `A8R8G8B8` | 21 | little-endian B,G,R,A |
| `X8R8G8B8` | 22 | little-endian B,G,R, opaque A |
| `A8` | 28 | opaque RGB multiplied by A |
| `DXT1` (BC1) | `0x31545844` | 8-byte 4×4 colour blocks, DDS little-endian order |
| `DXT5` (BC3) | `0x35545844` | 8-byte alpha + 8-byte colour 4×4 blocks |

For DXT1/DXT5, `row_pitch` is measured in compressed block-row bytes, not
pixels. Edge blocks are decoded and clipped to the texture dimensions.
`row_pitch` is honoured, including padding. Only the captured mip level is
available. Point and bilinear filtering are implemented, with WRAP, MIRROR,
CLAMP, BORDER and MIRRORONCE addressing. DXT3, ATI/BC4/BC5/BC7, ETC1, cube and
volume textures are named unsupported rather than decoded incorrectly.
Additional stages are not combined because v3 does not contain the texture-stage/TEV
operations needed to combine them.

Alpha test supports all eight D3DCMP functions. Alpha blending supports ADD,
SUB, REVSUB, MIN and MAX, with ZERO, ONE, SRC/DEST colour, SRC/DEST alpha,
inverses and SRCALPHASAT. `color_write` preserves masked destination channels.
Separate-alpha blend state and BLENDFACTOR are not in the capture and therefore
fail by name if required.

### Output and verification

PNG output is non-interlaced RGBA8 with filter-zero scanlines, written directly
as IHDR/IDAT/IEND chunks and zlib-compressed data. Dimensions come from the
active target, falling back to the viewport when target dimensions are zero.
The PNG is a diagnostic approximation, not a replay of guest shaders.

Run the standard-library tests:

```powershell
python tests/capture_raster_test.py
```

They cover list/strip/fan, indexed 16/32 draws, colour interpolation, DXT1/BC1
and DXT5/BC3 reference blocks, compressed row pitch, texture sampling, alpha
blend/test, viewport offsets, fixed-function transforms, PNG chunks/dimensions,
hash validation and named malformed-stride rejection.

A real four-capture Bink sequence from
`E:\menu_capture\run_tesv_slice_8260` was rendered at 512×1024. It produced
27,880 fragments/non-clear pixels (the clear is opaque black), 8 triangles and
no culled triangles; four-way parsing took 1.285 s wall time on the host. The
early captures contain only the blue Bink placeholder geometry, not the menu.

### Rasterizer limitations

* Vertex/pixel shader bytecode and TEV programs are recorded but not executed.
  Shader constants, texture-stage operations and the full vertex declaration
  semantics are not in v3. The tool draws decoded positions/UV, white colour
  for `D9Vertex`, and a simple stage-0 texture approximation.
* Default 20-byte captures cannot reproduce menu vertex tint/COLOR0. Full
  compact indexed capture preserves it as 40-byte `D9ColorVertex` when the
  declaration supplies COLOR0; declarations without COLOR0 still use white.
* No stencil, scissor rectangle, fog, lighting, mip selection, sRGB conversion,
  multiple texture stages, cube/volume sampling or shader texkill.
* A Present event has `pixels.captured=0`; it cannot be used as a screenshot.
* Draws must target the same dimensions when composed. Clear colour is known,
  but a non-default clear depth/stencil value is not captured.

## Capture metadata overview

`tools/capture_info.py` is the text/JSON companion to the rasterizer. It reads
only the `.txt` snapshots, so it works even where the `.bin` payload is absent
or corrupt; the `BIN` column merely reports whether a sibling `.bin` exists.
All `*firstdraw*.txt` files below the given directory are scanned, recursively
by default (`--no-recursive` limits the scan to one level). A single `.txt` path
is also accepted. Standard library only, no writes to the capture directory.

```powershell
# Table: one row per file, plus per-directory totals.
python tools/capture_info.py -Dir E:\menu_capture\run_tesv_slice_8260
python tools/capture_info.py E:\menu_draw\          # positional -Dir also works

# Structured output for scripts: "summary" and "files" with raw fields and names.
python tools/capture_info.py -Dir E:\menu_draw\ --json
```

The table lists the file, draw type, primitives, indexed flag, the raw D3D9
`draw.vertex_count` versus the actually captured `vertices.count`
(`VCOUNT/CAPT` - the former is often `0` for `DrawPrimitiveUP` while the latter
is the payload length), viewport, short z/blend/cull/alpha state labels, captured
texture formats (`0:DXT1,1:DXT5`, etc.) and truncation names. JSON output also
includes numeric texture formats, row pitches, stored/declared pixel sizes and
per-texture truncation flags. Present files are reported separately and never
participate in draw totals. The summary counts draws/presents, primitives,
captured vertices/indices, truncated draws and files with a `.bin` sibling.
Type, cull and blend names follow
`source/d3d9_core.h` (`D9_TRIANGLELIST=4`, `D9_TRIANGLESTRIP=5`,
`D9_TRIANGLEFAN=6`).

Typical workflow: run `capture_info.py` first to see which snapshots exist and
which states/geometry they carry, then feed the selected `.bin` files to
`capture_raster.py` as shown in *Usage* above. For example, a Present file has
no pixels (`pixels.captured=0`), so it should be skipped when choosing PNG
inputs, while draws sharing one viewport can be composed in argument order.
