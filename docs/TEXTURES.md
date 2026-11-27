# TEXTURES — DDS/BC → ETC1 pipeline, quality limits and GPU residency

Owner: texture/residency work. This file is the texture counterpart to
`docs/ASSETS.md` §4 and the TEV sections of `docs/SHADERS.md`; it does not
change the shader translator.

All measurements were taken on **2026-11-24** against the read-only install at
`E:\SteamLibrary\steamapps\common\Skyrim\Data` and the derived inventory under
`E:\Skyrim_N3DS_build\assets\textures`. No game bytes are stored in this
repository. Host-side numbers are labelled **host**; the isolated ARM number is
labelled **Azahar**.

## 1. What is implemented

| File | Purpose |
|---|---|
| `tools/skyrim/etc1.py` | bounded-memory DDS/BC1/BC2/BC3 → ETC1/ETC1A4 pipeline, SKTX container, inventory projection, ETC1 decoder for quality metrics |
| `tools/texture_pipeline.py` | CLI: `project`, `convert`, `sample`, `plan` |
| `source/d3d9_texcache.c/.h` | guest/GPU ownership, direct GPU-only ETC1/SKTX registration, durable MANAGED rehydrate tokens, drop-after-upload, LRU/working-set limits, explicit `no_evict` entries, deterministic GPU-failure behavior, accounting |
| `source/d3d9_texture_data.c/.h` | SKTX parser used by the runtime uploader |
| `tests/texture_pipeline_test.py` | synthetic BC1/BC2/BC3, mips, alpha, budgets, container corruption, inventory projection, optional real sample |
| `tests/texture_residency_smoke.c` | ownership/accounting/LRU/pin/GPU-failure unit tests |
| `tests/texture_residency_upload_smoke.c` | direct GPU-only add/remove, MANAGED eviction/reload, release failures, exact peaks |
| `tests/texture_runtime_smoke.c` | pool-aware core/COM ownership, split accounting, direct SKTX, retry and D3D9 state defaults |
| `tests/cube_volume_residency_smoke.c` | cube/volume D9TexCache accounting, rehydratable non-RT entries, RT-cube no-evict, retryable release/teardown, 2D/RT coexistence |
| `tests/cube_volume_rehydration_smoke.c` | evicted cube/volume rehydration, RT-cube no-evict, failure injection and counters |
| `tests/budget_trim_smoke.c` | automatic LRU trim under device-budget pressure, bound/locked/RT skips, named OOM, rebind rehydrate |
| `tests/texture_container_smoke.c` | SKTX parser and every rejection branch |
| `tests/isolated/texetc1/` | isolated ARM build; ETC1/ETC1A4 GPU init+upload, frame clear, residency contract |

Reproduce (PowerShell):

```powershell
$env:PYTHONPATH='tools'
.venv\Scripts\python.exe tests\texture_pipeline_test.py
.venv\Scripts\python.exe tools\texture_pipeline.py project
.venv\Scripts\python.exe tools\texture_pipeline.py sample
powershell -File tools\run_host_tests.ps1 -Only texture_residency,texture_residency_upload,texture_runtime,texture_container
```

Real-data conversion is opt-in:

```powershell
$env:SKYRIM_N3DS_REAL_DATA='1'
.venv\Scripts\python.exe tests\texture_pipeline_test.py
```

Outputs go under `E:\Skyrim_N3DS_build\assets\textures\etc1_sample` (or the
`--out` path); the default sample reads exactly the four menu textures named in
`docs/SHADERS.md` §5.

## 2. Conversion policy

### 2.1 Formats and alpha

| Source | Alpha in source | Default output | Alpha handling |
|---|---|---|---|
| DXT1 / BC1 opaque | none | ETC1 (8 B / 4×4 block, 4 bpp) | none |
| DXT1 / BC1 punch-through (`c0 <= c1`) | 1-bit | ETC1A4 | 4-bit alpha, **not** 1-bit |
| DXT3 / BC2 | 4-bit | ETC1A4 | 4-bit alpha, same bit width, different encoding |
| DXT5 / BC3 | 8-bit | ETC1A4 | 4-bit alpha, **not** 8-bit |
| BGR24/BGRA8/R8 | as decoded | ETC1A4 if alpha forced; ETC1 otherwise | BGR24/R8 have no alpha |

`alpha_mode=auto` is the production default. `force` selects ETC1A4 even when
the source is opaque; `drop` selects ETC1 and sets `alpha_dropped=true` in the
plan and result. Dropping is never silent.

BC1 punch-through detection scans the top mip's colour endpoints; it does not
decode pixels. A BC1 file with no `c0 <= c1` block is treated as opaque. This is
a production decision, not a claim that every mip has the same alpha mode.

### 2.2 Dimensions and mip chains

The real inventory is all power-of-two; non-power-of-two is rejected explicitly
rather than padded, because padding changes UVs and PICA tiling.

PICA caps the surface at 1024. The pipeline **enters a source mip** instead of
decoding a 4096×4096 RGBA buffer:

```
target_max_dim = 1024
start_level    = ceil(log2(max(width,height) / 1024))
target dims    = source mip at start_level
```

That both respects the hardware limit and bounds host memory. Example:
`textures\clutter\mehrunesstatue01.dds` is 2048×2048 DXT1 with 12 source mips;
the plan starts at mip 1, targets 1024×1024 with 8 PICA levels, and estimates a
11.3 MB host peak (source + one decoded mip + PNG + raw).

The mip chain stops below 8×8, matching `d3d9_core.c` (`gpu_levels` only counts
levels with both dimensions ≥ 8). The tail of the source DDS is ignored by the
renderer and by this pipeline.

Two modes:

* `mip_mode=source` (default): decode one stored source mip at a time and encode
  it. Bethesda's filtered mips are preserved. If a stored level is missing, the
  previous RGBA level is box-filtered and the level record says
  `box-filtered from level N`.
* `mip_mode=generate`: decode only the entry level and let `tex3ds -m box`
  generate the chain. This is faster but replaces the source mips.

### 2.3 Bounded memory

`MemoryMeter` tracks the live host buffers the pipeline owns:

```
source_dds + decoded RGBA + PNG + encoded raw + final container
```

The largest live set is reported as `memory.peak_bytes`, `peak_live` and
`peak_event`. A policy cap (`max_memory_bytes`, default 16 MiB) turns an
over-budget request into a deterministic `TextureBudgetError` that names the
limit and the contributing buffers. The tex3ds child process heap is **not**
included; the report says the peak is a lower bound on total host memory.

Measured sample peaks (host, one texture at a time):

| Texture | Peak host bytes | Peak live set |
|---|---:|---|
| `gradsteamthin.dds` | 463,385 | source 87,536 + RGBA 262,144 + PNG 48,169 + raw 65,536 |
| `fxsteamthinanim.dds` | 1,978,696 | source 349,680 + RGBA 1,048,576 + PNG 318,296 + raw 262,144 |
| `logo02_d.dds` | 3,245,258 | source 349,672 + RGBA 2,097,152 + PNG 274,146 + raw 524,288 |
| `logo02_n.dds` | 3,995,966 | source 699,216 + RGBA 2,097,152 + PNG 675,310 + raw 524,288 |

Peak simultaneous bytes across the four-texture sample: **3,995,966 B** (host).
This is the conversion pipeline, not the runtime residency cache (§4).

### 2.4 Output container (SKTX)

The host writes a small validated container rather than an undocumented blob:

```
0   4  "SKTX"
4   4  u32 version = 1
8   2  u16 format (0 ETC1, 1 ETC1A4)
10  2  u16 levels
12  2  u16 width
14  2  u16 height
16  4  u32 data_bytes
20  4  u32 crc32 of the concatenated level data
24  12*n level table { u16 w, u16 h, u32 absolute offset, u32 bytes }
...    PICA-tiled ETC1/ETC1A4 blocks from tex3ds -r
```

`source/d3d9_texture_data.c` validates magic/version/format/level count, every
offset and size, the expected block count for the level dimensions, the
`data_bytes` sum and the CRC. It returns pointers into the caller's buffer; it
does not allocate. `read_sktx()` is the Python twin.

## 3. Measured inventory projection (host, no pixels decoded)

Source: `E:\Skyrim_N3DS_build\assets\textures\textures.json`, 18,580 DDS files.

| Item | Value |
|---|---:|
| Textures | 18,580 |
| DXT1 / BC1 | 15,379 |
| DXT5 / BC3 | 2,517 |
| DXT3 / BC2 | 493 |
| BGR24 / BGRA8 / R8 | 189 / 1 / 1 |
| Surfaces exceeding 1024 (entered at a lower mip) | 64 |
| ETC1, alpha dropped, 1024 cap, 8×8 mip floor | **1,544.14 MiB** |
| Mixed auto (ETC1 for opaque, ETC1A4 for alpha) | **2,089.53 MiB** |
| ETC1A4 for every texture | 3,088.27 MiB |

The older `docs/ASSETS.md` §3 figures (1,646 MiB ETC1 / 2,219 MiB mixed) did
not apply the 1024 cap or the 8×8 mip floor. The difference is the 64 large
surfaces and the discarded sub-8×8 tail, not a change to the format.

`needs_alpha` in the inventory comes from the format classification, not a BC1
pixel scan; a BC1 punch-through file can therefore be projected as ETC1 while
the per-file converter correctly chooses ETC1A4. The projection is a budget
estimate, not a conversion.

A 4 MiB runtime cache holds roughly 23 full-mip 512×512 ETC1 surfaces
(174,776 B each) or 95 full-mip 256×256 surfaces (43,704 B each). The inventory
projection is a budget estimate; it is larger than VRAM because it counts every
shipped texture, not the working set.

## 4. Quality: what ETC1/ETC1A4 really is

**ETC1 is not a lossless or BC-equivalent format.** It stores one 64-bit block
per 4×4 pixels: two base colours, two modifier tables selected from eight
fixed tables, a diff/flip mode and 16 two-bit selectors. It has **no alpha**.
ETC1A4 appends **4-bit alpha** (16 levels, step 17, maximum round-trip error 8),
not BC3's 8-bit alpha and not BC3's independent colour/alpha endpoint blocks.
Calling ETC1A4 “BC3” would be wrong.

Quality is measured by decoding the encoder's raw ETC1 blocks (the pipeline's
decoder is validated on solid colour and four-quadrant fixtures) and comparing
RGB with the decoded source mip:

* `all`: every pixel.
* `visible`: only pixels whose **source alpha ≥ 8**. ETC1A4 encoders may spend
  no bits on RGB hidden by zero alpha, so the all-pixel figure punishes data a
  renderer cannot show. Both are reported.

Menu sample (host; source decoded from the retail DDS, output from tex3ds -q high;
“Bytes” is the ETC1/ETC1A4 surface; the SKTX container adds 24 + 12·levels bytes,
i.e. 108 B for these 7-level and 6-level chains):

| Texture | Source | Output | Bytes | Visible RGB PSNR | Max error | Peak host bytes |
|---|---|---|---:|---:|---:|---:|
| `gradsteamthin.dds` | DXT5 256×256 | ETC1A4 | 87,360 | 48.142 dB | 174 | 463,385 |
| `fxsteamthinanim.dds` | DXT5 512×512 | ETC1A4 | 349,504 | 37.539 dB | 247 | 1,978,696 |
| `logo02_d.dds` | DXT1 512×1024 | ETC1A4 | 699,008 | 45.679 dB | 16 | 3,245,258 |
| `logo02_n.dds` | DXT5 512×1024 | ETC1A4 | 699,008 | 30.909 dB | 255 | 3,995,966 |

Interpretation, honestly:

* **logo02_d** is a mostly opaque logo; 45.7 dB visible RGB and a 16-LSB maximum
  error are a good match for a menu texture. Its 1-bit punch-through alpha
  becomes 4-bit alpha, which is coarser at edges.
* **logo02_n** is a normal map with 48% zero-alpha pixels. Visible RGB is
  30.9 dB; all-pixel PSNR is only 6.2 dB because the encoder does not preserve
  RGB under zero alpha. If a shader samples normal RGB without respecting
  alpha, ETC1A4 will visibly damage it. A normal-map quality gate is required
  before shipping this class; the pipeline reports the metric but does not
  pretend the result is lossless.
* **gradsteamthin / fxsteamthinanim** are soft alpha gradients. Visible RGB is
  good (48.1 / 37.5 dB), but alpha is 4-bit. A smooth fade will show 16 alpha
  steps unless the blend/dither hides them. This is a real ETC1A4 limitation,
  not a measurement artefact.
* ETC1 versus BC1 is 1:1 in bytes (both 4 bpp) but not 1:1 in quality: ETC1 has
  a different block model and no hardware BC1 endpoint interpolation.
* ETC1A4 versus BC3 is 1:1 in bytes (both 8 bpp) only when alpha is preserved;
  the alpha precision is 4 vs 8 bits and the colour coding is different. The
  older “ETC1 halves BC3” statement is true only when alpha is silently dropped,
  which this pipeline refuses to do by default.

## 5. Runtime residency and accounting

`source/d3d9_texcache.c` is the policy layer that prevents the guest source and
the GPU copy from both being retained for the lifetime of a texture.

### 5.1 States and ownership

| Entry state | Guest bytes | GPU bytes | Meaning |
|---|---:|---:|---|
| guest only | yes | no | decoded source, not uploaded |
| uploading | yes | no | pinned by `begin_upload` |
| GPU only | no | yes | direct ETC1/SKTX native; no retained guest source |
| resident | dropped if disposable | yes | normal drawable texture |
| pinned resident | yes/no | yes | readback or upload in progress; not evictable |
| failed upload | yes | no | GPU allocation failed; source kept for retry |
| MANAGED token only | no | no | GPU evicted; opaque handle retained for reload |

`commit_upload(cache, key, gpu, bytes, drop_guest)` takes ownership of `gpu`
only when it returns `D9TEXCACHE_OK`.
With `drop_guest != 0` and an owned, unpinned guest, it calls `release_guest`
and removes the guest bytes. If lifetime semantics need CPU readback, pass
`drop_guest = 0`; the cache then reports `both_bytes` and `peak_both` explicitly
so the duplicate is visible rather than hidden.

### 5.2 Limits and deterministic failure

* `guest_limit` / `gpu_limit` are byte caps; 0 means unlimited.
* `add_guest` evicts the oldest unpinned guest copies until the new source fits.
  If it cannot (pinned or too large), it returns `D9TEXCACHE_GUEST_FULL` and
  inserts nothing.
* `commit_upload` evicts the oldest unpinned GPU copies until the new GPU copy
  fits. If it cannot, it returns `D9TEXCACHE_GPU_FULL`, does **not** record the
  GPU copy, does **not** release the guest source, sets `gpu_failed`, and leaves
  the caller owning `gpu`. The caller can evict/pin-release and retry the same
  call deterministically.
* `gpu_alloc_failed` is the explicit path when the backend allocation itself
  fails before a pointer exists; it does the same accounting and returns
  `D9TEXCACHE_GPU_FULL`.
* `add_gpu` registers an already-uploaded native texture and evicts LRU GPU
  copies until its explicit byte cost fits. Oversized or pinned-blocked requests
  return `D9TEXCACHE_GPU_FULL`; the caller retains the native pointer.
* Pinned entries are never evicted. `trim` returns `*_FULL` if a pinned entry is
  the only thing preventing the target from being reached; it still evicts every
  unpinned entry it can.
* A release callback failure leaves that copy and its accounting unchanged and
  returns `D9TEXCACHE_RELEASE_FAILED`. Earlier successful LRU evictions in the
  same operation remain committed; the candidate is not recorded.

### 5.3 Working set

`d9_texcache_working_set(cache, max_age)` counts entries used within
`max_age` ticks of the current clock. `d9_texcache_trim(cache, guest_target,
gpu_target)` evicts LRU unpinned entries until both byte counts are at or below
the targets. Both are deterministic for a given operation order; the host unit
test repeats a sequence and checks the same eviction.

Concrete menu example (bytes, from the sample plans):

| Texture | Guest source (BC) | GPU copy (ETC1A4) | Both during upload | Resident after drop |
|---|---:|---:|---:|---:|
| `logo02_d.dds` | 349,672 | 699,008 | 1,048,680 | 699,008 |
| `logo02_n.dds` | 699,216 | 699,008 | 1,398,224 | 699,008 |

The old core keeps the guest source for the texture's whole lifetime, so it
would retain the “both” column. The cache drops the disposable source after the
last successful level upload; a texture that needs CPU readback can pass
`drop_guest=0` and the duplicate stays visible in `both_bytes`.

### 5.4 Accounting fields

`D9TexCacheStats` exposes current `guest_bytes`, `gpu_bytes`, `both_bytes`,
entries/guests/gpus/pending-GPUs/rehydratable/pinned, `peak_guest`, `peak_gpu`, `peak_total`,
`peak_both`, uploads, upload failures, guest/GPU evictions, guest drops and the
LRU clock. The host test asserts the exact numbers for the drop and no-drop
cases, including `peak_both = guest + GPU` at the moment before the guest is
dropped. Direct GPU registrations count as uploads and have no guest/both bytes.

### 5.5 Runtime integration

The core/COM ownership wiring is applied; the exact pool behavior and remaining
lifetime gaps are documented in `docs/TEXTURE_RESIDENCY_INTEGRATION.md`.

```
guest-backed: add_guest -> attach pending GPU -> pin -> commit(drop_guest=0)
backend fail: keep lock/pin for retry
direct SKTX:  parse/upload -> add_gpu(native, cost, owned, token)
release:      d9_texcache_remove(key)
```

DEFAULT and MANAGED still retain `Texture.pixels` after upload, so this pass does
**not** reduce their texture RAM. SYSTEMMEM is CPU-only. Direct SKTX textures are
GPU-only and never retain the container. MANAGED token eviction/rehydrate,
Reset handle preservation, dynamic DISCARD, and automatic disposable guest drop
remain pending.

### 5.6 Volume sampling in a draw

Volume textures can now be sampled by a draw through the append-only
`D9Backend.draw_volume(ctx, vertices, count, clip, D9VolumeView, state)`
callback:

* The core passes `vertices[i].w` (TEXCOORD0.z) and a `D9VolumeView` with the
  level-major CPU slice store, per-level width/height/depth, pitch, plane size
  and native handle.  `D9ColorVertex` gained a trailing `w` field; every 2D/FVF
  vertex leaves it zero.
* A volume draw is stage 0 only, needs an active stage-0 colour operation, and
  refuses a pixel shader or a second texture stage by name.  It requires an
  uploaded GPU-resident volume with A8R8G8B8/X8R8G8B8 slices and a 3-component
  TEXCOORD0; 2D vertices (including FVF `XYZ|TEX1`) are a named refusal, never a
  slice-0 fallback.  Mip filtering requires every mip level uploaded; point
  sampling only needs level 0.
* The null backend point-samples the nearest voxel of level 0 and records the
  first three ARGB texels and `w` values in `D9NullStats`.  There is no
  derivative-based LOD, so mip selection stays at level 0 even when the chain is
  resident (documented point behavior, not a trilinear claim).  `ADDRESSW` is
  not modelled; W uses the V wrap/mirror/clamp policy.
* PICA does not implement volume sampling.  libctru/citro3d exposes
  `GPU_TEX_2D`, `GPU_TEX_CUBE_MAP`, `GPU_TEX_SHADOW_2D`, `GPU_TEX_PROJECTION`
  and `GPU_TEX_SHADOW_CUBE` but no 3D texture mode, and the PICA backend has no
  `texture_create_volume`/`draw_volume`.  Volume creation and a volume-bound draw
  are therefore named `D3DERR_NOTAVAILABLE` refusals with the missing callback in
  the diagnostic; no 2D slice is substituted.

`tests/volume_sampling_smoke.c` uses a 4x4x4 A8R8G8B8 volume with a distinct
colour per z slice and verifies `(u,v,w)` across all four layers, plus named
refusals for an unuploaded volume, 2D texcoords, an incomplete mip chain, stage 1
and a backend without `draw_volume` (the PICA shape).  DevCaps stay
`0x00082000`.  No hardware run or frame is claimed.

## 5.1 UpdateTexture / UpdateSurface / ColorFill

Device slots 31/30/35 are implemented for CPU-backed stores; no GPU copy is
invented.

* `UpdateTexture(source, destination)` copies every mip level from the source
  CPU store into the destination CPU store, then re-uploads each destination
  level through the existing surface-unlock/TexCache rules. Both textures must
  be 2D, unlocked, share format/dimensions/level count and retain a guest copy.
  Render-target/GPU-only textures, cube/volume handles, locks, format/size/level
  mismatches and a missing guest copy are named refusals.
* `UpdateSurface(source, sourceRect, destination, destPoint)` copies a
  rectangle between CPU-backed surfaces. `NULL` source rect means the whole
  source; `NULL` destination point means `(0,0)`. Formats must match, the rect
  must be non-empty/inside the source, and the copy must fit the destination.
  Destination texture-level views are re-uploaded after the copy. Depth, cube
  faces, compressed surfaces, locked surfaces and any GPU-only render
  target/back buffer are named refusals.
* `ColorFill(surface, argb, rect)` fills the whole level (`rect = NULL`) or a
  sub-rect. CPU-backed surfaces and texture-level views are filled directly and
  re-uploaded through the level-unlock rule. A render target with a backend
  target is filled through `d9_render_target_write` (the same read/modify/write
  path Clear/Unlock use), so the bytes really reach the backend; a target
  without read/write support is a named refusal, not a CPU-only fake. Depth and
  cube-face surfaces are refused.

## 6. Isolated ARM / Azahar evidence

Build and run:

```powershell
$bash = 'C:\devkitPro\msys2\usr\bin\bash.exe'
& $bash -lc "cd /c/users/vasya/documents/skyrim_n3ds/tests/isolated/texetc1 && make"
powershell -File tools\run_azahar.ps1 `
  -ThreeDsx tests\isolated\texetc1\texetc1.3dsx -Name texetc1 -TimeoutSec 12
```

**Azahar result (2026-11-24, evidence
`build_azahar/20261124-162334-texetc1`): PASS, 11 checks, 0 failures.**

The isolated test verifies on ARM:

* `C3D_TexInit(..., GPU_ETC1)` and `GPU_ETC1A4` accept the surface;
* `C3D_TexLoadImage` uploads the 8-byte/16-byte PICA-tiled blocks produced by
  the host pipeline;
* a render-target clear completes through the GPU;
* the residency contract (guest drop after upload, GPU accounting, peak
  coexistence) holds on the console;
* the SKTX parser/CRC code compiles and links on ARM (CRC test vector).

It does **not** read back sampled pixels, so it is not a visual/colour
equivalence test. The PSNR figures in §4 are host-only.

## 7. Host test evidence (2026-11-24)

| Test | Result | Coverage |
|---|---|---|
| `texture_residency` (`tests/texture_residency_smoke.c`) | **91 checks, 0 failures** | drop/no-drop ownership, peak coexistence, LRU order, pin, guest/GPU limits, GPU failure retry, release-failure atomicity, deterministic repeat, working set |
| `texture_residency_upload` (`tests/texture_residency_upload_smoke.c`) | **286 checks, 0 failures** (also ASan/UBSan) | GPU-only owned/borrowed add/remove, MANAGED token eviction/reload, deterministic GPU_FULL/GUEST_FULL, release callback retry, exact current/peak bytes, temporary SKTX release |
| `texture_runtime` (`tests/texture_runtime_smoke.c`) | **108 checks, 0 failures** (also ASan/UBSan) | DEFAULT/MANAGED/SYSTEMMEM behavior, split accounting, lock/unlock retry, direct SKTX validation/upload, release/destroy retry, D3D9 defaults, missing-UV rejection |
| `texture_container` (`tests/texture_container_smoke.c`) | **24 checks, 0 failures** (29 with a real container argument) | CRC test vector, ETC1 and ETC1A4 parse, level pointers, magic/version/count/size/offset/data/CRC/truncation rejection; optional real `logo02_d` container parsed as 512×1024 ETC1A4, 7 levels, 699,008 data bytes |
| `texture_pipeline_test.py` | **all sections pass** | BC1/BC2/BC3 plans, alpha auto/force/drop, BC1 punch-through, 2048 entry-mip plan, memory/output budget failures, NPOT rejection, conversion, mips source/generate, container corruption, PSNR identity/mask, inventory projection |
| real sample (`SKYRIM_N3DS_REAL_DATA=1`) | **4/4 menu textures converted** | sizes/PSNR/peaks in §4; peak simultaneous 3,995,966 B |
| `graphics`, `bc_shader` regression runs | **PASS** | the shared D3D core and DDS block decoder still pass with these files present (they are not in the main `OFILES`, so no runtime link change yet) |

The shared runner invokes `texture_residency`, `texture_residency_upload`,
`texture_runtime`, `texture_container`, and the Python pipeline test. The main
`Makefile` includes `d3d9_texcache.o` and `d3d9_texture_data.o` in graphics, COM,
and TESV targets.

## 8. Limitations and next steps

* The encoder is devkitPro **tex3ds**; this tree still does not contain a
  from-scratch ETC1 encoder. That is deliberate: a toy encoder would make the
  quality numbers meaningless.
* The cache supports durable MANAGED rehydrate tokens and direct GPU-only
  registration, but asset-loader token plumbing and the SD streaming policy
  (lookup, bounded read buffer, mip-by-mip upload) remain pending.
* ETC1A4 alpha is 4-bit. Menu fades and soft particles need either dithering,
  a different alpha source (constant/vertex alpha), or acceptance of 16 levels.
  Do not describe this as BC3-equivalent.
* Normal maps with zero alpha need a quality gate; the pipeline reports
  visible/all PSNR but does not fail a texture automatically. A production gate
  should reject visible PSNR below a chosen threshold for `_n` maps.
* 64 shipped surfaces exceed 1024 and are entered at a lower mip. That loses
  detail relative to the source top mip; it is a hardware limit, not a bug.
* Core ownership wiring is applied, but full MANAGED eviction/rehydrate, Reset
  handle preservation, dynamic DISCARD, and automatic disposable guest drop
  remain pending. DEFAULT and MANAGED still retain guest copies.

## 9. Frame pinning and cache limits (tex-limits)

The PICA backend submits draws asynchronously. A native texture referenced by a
draw that was already sent to the GPU must not be evicted or deleted until the
GPU has consumed it. `d3d9_texcache` therefore carries two frame-scoped pins per
entry, in addition to the plain LockRect/upload pins:

* `bound_frame` — set when a texture occupies a sampler slot (`SetTexture`,
  `SetCubeTexture`, `SetVolumeTexture`, stage 0 or 1). Rebinding the slot
  releases only this pin, so a texture that was bound but never drawn becomes
  LRU-eligible again.
* `draw_frame` — set when a draw that samples the currently bound slots is
  submitted. It survives an unbind; the GPU command still reads the native.
* Both pins are idempotent within one frame generation, are replaced (not
  accumulated) if a guest spans generations without Present, and are released
  only by `d9_texcache_frame_clear()`.

The core (`d3d9_core.c`) increments a generation per Present. After `ops.end`
closes the frame and after deferred uploads, `Present` calls the optional
backend `frame_sync` hook and only then clears the old generation. The citro3d
backend implements `frame_sync` as `C3D_FrameSync()`; the null/host backend
leaves it NULL and is documented as a synchronous frame boundary. Textures
still bound at that point are re-pinned for the next generation, because D3D9
sampler bindings survive Present. Explicit `ReleaseTexture` is allowed to drop
frame pins and remove the entry: every backend delete callback already waits for
the GPU (citro3d calls `C3D_FrameSync` before `C3D_TexDelete`). Plain pins
(LockRect, in-flight upload) still refuse removal.

Eviction considers only `!pins && !no_evict` LRU entries. If the pool is full
and every candidate is pinned, the allocation fails with
`D9TEXCACHE_GPU_FULL`/`_GUEST_FULL`, `stats.pinned_refusals` increments, and the
core sets a named diagnosis:

```
texture cache GPU limit N bytes reached (used M); all K live candidates are
pinned by textures referenced by submitted GPU draws (P pinned); allocation
refused until Present
```

Nothing is freed or silently reused in that path. `stats.pinned` reports entries
with any pin, `stats.non_evictable` the explicit-release-only entries.

### Limits

`N3DS_TEX_GUEST_LIMIT` / `N3DS_TEX_GPU_LIMIT` are read by `d9_create_device()`.
They accept a byte count or a `K`/`M`/`G` suffix (`8M` = 8 MiB); unset, empty or
invalid means **0 = unlimited**, so default behaviour is unchanged. On reaching
a limit, reserve/trim evicts unpinned, evictable LRU entries (oldest `last_use`
first) and retries. `d9_texture_cache_set_limits()` changes the caps at runtime;
`d9_texture_cache_get_limits()` reads them back.

Counters in `d9_texture_cache_stats()` / the `mem_stats` tex line:
`trims`, `budget_trims`, `budget_trim_evictions`, `evictions_gpu`,
`non_evictable`, `pinned`, `pinned_refusals`, plus peaks and byte counts. The
existing `tex_trims`/`tex_trim_evictions` overlay fields keep their budget-trim
meaning; `tex_non_evictable`, `tex_pinned` and `tex_pinned_refusals` are new.

Host coverage: `texcache_pinning` (cache policy) and `texcache_limits_core`
(env parser, 2D/cube/volume/RT pins, named refusal, unpin at Present). Both run
under ASan/UBSan. Regressions `budget_trim`, `texture_update`,
`cube_volume_residency`, `render_texture`, `com`, `menu_indexed_draw` pass.

Remaining: frame pins assume one logical frame per Present; a guest that
submits across several Present calls is covered per generation, but a texture
kept bound forever stays resident (correct for D3D9). PICA volume sampling is
still refused by the citro3d backend, so the volume pin path is exercised only
through the host/null backend.
