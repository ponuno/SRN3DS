# PICA200 TEV backend integration

## Status: integrated

The backend gate and citro3d adapter are now wired into core, COM, the graphics
builds, and the TESV 3DS runner.  The host TESV runner remains on the null
backend.  Texture cache/core ownership code was not changed.

Integrated files:

| File | Change |
|---|---|
| `source/d3d9_pica_tev.h/.c` | Host-testable validation, raw ps_3_0 preflight, and backend descriptor |
| `source/d3d9_pica_citro3d.h/.c` | 3DS-only `C3D_TexEnv` apply/restore adapter |
| `source/d3d9_core.c/.h` | Borrowed active TEV pointer carried through `D9DrawState` |
| `source/d3d9_com.c` | Named creation-time validation, SetPixelShader wiring, lifetime clearing |
| `source/d3d9_citro3d.c` | TEV apply path plus deterministic fixed-function restore |
| `source/tesv_slice_main.c` | Real PICA backend on 3DS, null fallback, teardown after COM |
| `Makefile` | Validator/adapter/shbin objects and citro3d libraries for TESV |
| `tools/run_host_tests.ps1` | Standalone `pica_tev` and validator linkage for COM tests |
| `tests/tev_com_smoke.c`, `tests/com_smoke.c` | Bound-pointer, zero-texture, SM2/NULL, and rejection coverage |
| `tests/isolated/pica_tev/` | Isolated ARM citro3d state probe; no draw |
| `tests/isolated/uicopy/` | Host mock regression plus ARM/Azahar COM -> core -> PICA200 draw with pixel readback |

There is **no real Skyrim frame** in this evidence.  The older ARM state probe
programs TEV state only.  The newer `ui.copy` fixture proves one synthetic
submission and deterministic texel output; it is not the Skyrim menu.  The TESV
runner verifies backend startup/reachability separately and does not prove a
game frame or visual equivalence.

## 1. Accepted subset

`d9_pica_tev_validate()` accepts a translated `D9TevProgram` only when all of the
following hold:

There are two explicit tiers.  Generic bytecode translation keeps the older
strict subset; pre-baked menu plans opt into the wider tier with
`D9TevProgram.backend_flags`.

| Requirement | Generic translation | Menu plans (`D9TEV_PLAN_MENU_*`) |
|---|---|---|
| Shader | `ps_3_0` | `ps_3_0` |
| TEV stages | 1..6 | 1..6 (the menu-light ambient split uses all 6) |
| Textures | 0 or 1 | 0..2, sampler `s0`/`s1`, 2D only |
| UV input | `texcoord0` only | `texcoord0` only |
| PS inputs | no vertex colour, VPOS/VFACE, normal, or fragment-colour varying | `color0` as `GPU_PRIMARY_COLOR`; `color1` only through the menu-light ambient fold |
| Constants | literal `def c#` only | literal `def c#` plus external c0..c7 |
| Ambient fold | n/a | `color1` is folded into internal c8 (`D9_PICA_TEV_AMBIENT_CONST`) |
| Stage 0 | must not depend on a meaningful `PREVIOUS` value | same |

The zero-texture case is intentional: a literal constant-colour TEV program does
not need a bound texture.  A textured program does; the citro3d adapter rejects a
missing texture rather than silently drawing the wrong result.

### Menu-light ambient fold

PICA has one interpolated vertex colour (`GPU_PRIMARY_COLOR`) and one TEV
constant per stage.  The retail menu-light stage
`NdotL*c4 + color1` therefore cannot be emitted literally.  For
`D9TEV_PLAN_MENU_LIGHT`, `d9_pica_tev_validate()`:

1. treats `color1` as an explicit material constant, not a per-vertex varying;
2. splits that stage into `MODULATE(PREV, c4)` plus `ADD(PREV, c8)`;
3. records c3/c4/c7/c8 in `D9PicaTevProgram.constant_reg[]`.

The algebra is preserved (rounding aside) and the result is six stages, but
per-vertex color1 interpolation is **not** claimed.  `c8` is an internal menu
slot; `MaxVertexShaderConst` remains 4.  `d9_pica_citro3d_apply_tev_ex()` takes
the c0..c8 bank and materialises each referenced constant with
`C3D_TexEnvColor()`.

### Important translator detail

`D9TevProgram.constants` counts **all referenced `c#` registers**, including
registers defined by literal `def` instructions.  It is not an external-only
count despite the older comment in `d3d9_tev.h`.  The backend gate uses each
stage's `const_is_literal` flag and verifies that every entry in
`program->constant_reg[]` is materialised by a stage.  A generic stage that reads
`GPU_CONSTANT` with `const_is_literal == 0` is still rejected; only menu plans
may use the external bank.

### Named rejections

`D9PicaTevReject` and `d9_pica_tev_reject_name()` provide stable reason names.
The human-readable `why` names the unsupported feature, for example:

- `multiple textures (2); generic translation binds only texture unit 0`
- `texture 0 must use sampler s0, not sampler s1`
- `external constant c9` (outside the c0..c8 menu bank)
- `color1 has no PICA vertex output; only the menu-light ambient fold is supported`
- `VPOS (dcl_position vPos) is not a PICA TEV source`
- `TEV stage 0 reads fragment varying FRAG_PRIMARY ...`
- `7 TEV stages; PICA200 supports 1..6`
- non-2D sampler, non-texcoord input, invalid TEV source/operand/function/scale,
  malformed count, missing END, and truncated instruction/comment

The current translator does not retain a VPOS flag in `D9TevProgram`.
`d9_pica_tev_preflight_pixel()` closes that gap by scanning raw ps_3_0 declaration
and register tokens before translation.  `d9_pica_tev_validate_pixel()` combines
that preflight, the existing translator, and the descriptor gate.

## 2. Descriptor and citro3d state contract

`D9PicaTevProgram` is independent of citro3d.  It contains a stage count, a
zero/two texture count, s0/s1 sampler and texcoord markers, a c# constant list,
primary/ambient flags, and up to six flat `D9PicaTevStage` descriptors.  Unused
descriptor slots are zeroed.  `d9_pica_citro3d_apply_tev_ex()` binds both 2D
texture units and materialises each stage constant from the c0..c8 float bank.

Host coverage: `pica_tev` **152 checks, 0 failures** (including the exact
menu-light plan, fog/tint plans, third-texture/duplicate-sampler/c9/color1
negatives), `two_stage` submits a synthetic 2-texture + color0 + external-c7
menu plan through `draw_menu`, and the full suite is **83 passed, 1 diagnostic,
0 failed**.  ARM `-Mode com` builds `Skyrim_N3DS_graphics_com_menu_pica`
(`.3dsx` 805,924 B, `.elf` 5,484,136 B).  No Azahar pixel run is claimed.

### Render-target texture handles

A target native and a sampleable texture native are separate objects.  The
append-only `D9Backend.target_texture(ctx, native)` returns the sampleable view;
`d9_create_render_texture()` stores that result in `Texture.native` instead of
assuming the target pointer is a texture.  A backend without the callback gets a
named `D9_UNSUPPORTED`; a callback returning NULL gets `D9_BACKEND`.

Citro3d keeps an embedded `NativeTex` view (`C3D_Tex` with the target's data and
separate filter/wrap state) inside `NativeTarget`.  The target's own `C3D_Tex`
owns the allocation; the view is never deleted, and `target_delete()` is the
only delete.  The null backend returns the target handle; its draw ignores it.
`tests/render_texture_sample_smoke.c` uses distinct target/texture magic tags and
also covers bind-before/after-target-render, `draw_two` with a second RT
texture, exactly-once deletion, and the colour-budget/backend split.

### Bounded native render targets

Explicit render targets and render-target textures keep their D3D logical size
but cap native colour storage at **512 per axis** (`D9_TARGET_NATIVE_MAX`).  A
1024x1024 target therefore allocates 512x512 RGBA8 (1 MiB) and keeps 1024x1024
in `GetDesc`/`GetLevelDesc` and `d9_render_target_info()`.  The
`d9_render_target_storage_info()` accessor exposes the native size; `bytes`
from `d9_render_target_info()` is the native allocation.

The append-only `D9Backend.target_viewport_scaled(ctx, logical_w, logical_h,
native_w, native_h)` maps the full logical surface into native storage.  citro3d
sets the PICA viewport to native_w x native_h, so NDC covers the whole image
instead of clipping at the native edge; the logical D3D viewport and clip
matrices are unchanged.  A backend without this callback refuses a target whose
native size is smaller than its logical size with a named `D9_UNSUPPORTED`
(`target_viewport_scaled` in the diagnosis).  Equal sizes (512x512, 256x256) and
the retained back buffer keep the existing `target_viewport` path; the
retained back buffer is created unbounded because its 400x240 logical surface
already uses a 512x256 native corner.

Scaled `LockRect`/`UnlockRect` and `StretchRect` are named refusals rather than
silently reading or copying 1024 logical pixels from 512 native storage.  A
lockable explicit target larger than 512 is refused at creation with a named
`D9_UNSUPPORTED` for the same reason.  Equal-size targets and the retained back
buffer keep read/write/blit support.

### Render-target budget

The core budget counts **native colour bytes only**.  A 1024x1024 logical
target is 512x512 native = 1 MiB, not 4 MiB.  The citro3d target also allocates
a DEPTH24_STENCIL8 attachment at the same 512x512 size, so a single target needs
about 2 MiB: that fits the 6 MiB N3DS VRAM budget.  Five simultaneous 1024
logical targets need five 1 MiB colour buffers plus five 1 MiB depth
attachments = 10 MiB, so at most three fit by raw accounting and the fourth is
surfaced as `D9_BACKEND` by `target_create()`, never as a fake success.  A
future shared/depth-lazy scheme is still required to hold all five measured TESV
targets.  No hardware memory proof is claimed here.

The translator packs literals as RGBA8 with red in bits 0..7.  PICA TEV constants
are ABGR8888, so the descriptor stores `constant_abgr`, produced by
`d9_pica_tev_color_abgr()`:

```text
RGBA8 0xFFC08040  ->  TEV ABGR 0x4080C0FF
```

`d9_pica_citro3d_apply_tev()`:

1. validates the `D9TevProgram` again and builds the flat descriptor;
2. checks that citro3d is active and a sampled texture is present and 2D;
3. initialises **all six** `C3D_TexEnv` stages to deterministic `PREVIOUS`
   passthroughs;
4. copies active RGB/alpha sources, operands, functions, scales, and constants
   into `C3D_TexEnv`;
5. binds the one native texture to unit 0 when needed;
6. marks every stage dirty.

`d9_pica_citro3d_restore_tev()` restores deterministic fixed-function state:
stage 0 replaces `GPU_PRIMARY_COLOR`, and stages 1..5 pass `GPU_PREVIOUS`
through.  The backend calls it after every draw and before any
fixed-function draw.

citro3d has no public texture-unbind API.  A zero-texture TEV program leaves an
old unit-0 binding untouched but unreferenced by any active TEV source.  This is
semantically deterministic; `restore_tev()` also removes every `TEXTURE0`
reference before fixed-function rendering.

## 3. Standalone evidence

### Host

From the repository root in a Visual Studio developer command prompt:

```powershell
clang -std=gnu11 -O2 -Wall -Wextra -Werror -Isource `
  source/d3d9_tev.c source/d3d9_pica_tev.c tests/pica_tev_smoke.c `
  -o pica_tev_smoke.exe
.\pica_tev_smoke.exe
```

Result:

```text
pica TEV backend: 134 checks, 0 failures
```

The same test also passes at `-O1` with
`-fsanitize=address,undefined -fno-omit-frame-pointer`.

Coverage includes accepted constant blit, textured blit, tint, three-stage
textured chain, and exactly six stages; rejection of multiple textures,
sampler 1, cube sampler, external constants, vertex colour, VPOS, fragment
varyings, more than six stages, invalid TEV fields, missing texture metadata,
and malformed token streams.

Full registered host suite (including the stable TESV host slice):

```powershell
powershell -File tools/run_host_tests.ps1 `
  -TesvSliceGen E:\Skyrim_N3DS_build\analysis\growth13\gen01 `
  -AllowDiagnosticFault -OutDir build_host_full_pica
```

Result: **36 passed, 1 diagnostic, 0 failed**.  The diagnostic is the expected
host TESV `ExitProcess(0)` memory-gate path; it is not a rendering PASS.
Focused `pica_tev`, `tev_com`, `com`, and `null_backend` tests also pass with
AddressSanitizer enabled.

### ARM compile

From `tests/isolated/pica_tev`:

```powershell
$env:PATH = 'C:\devkitPro\msys2\usr\bin;' + $env:PATH
& 'C:\devkitPro\msys2\usr\bin\make.exe' DEVKITPRO=C:/devkitPro
```

Result: `main.c`, `d3d9_tev.c`, `d3d9_pica_tev.c`, and
`d3d9_pica_citro3d.c` compile for ARM11 with `-Werror`; the probe links against
citro3d/libctru and produces `pica_tev_arm.elf` and `pica_tev_arm.3dsx`.

Integrated graphics and COM ARM targets also build in separate directories:

```powershell
powershell -File tools/build.ps1 -Mode graphics -Isolated picagfx
powershell -File tools/build.ps1 -Mode com -Isolated picacom
```

Both produced `.elf` and `.3dsx` artifacts under `dist/`.

### ARM/Azahar state probe

```powershell
powershell -File tools/run_azahar.ps1 `
  -ThreeDsx tests/isolated/pica_tev/pica_tev_arm.3dsx `
  -Name pica_tev_arm -TimeoutSec 12
```

The application log and wrapper both recorded PASS:

```text
pica_tev_arm: PASS (66 checks, errors=0; no draw)
AZAHAR RESULT: PASS
```

Evidence: `build_azahar/20261124-172439-pica_tev_arm/`.  The probe log uses
`errors=0` because the runner's default case-insensitive `FAIL` grep also
matches the word `failures`.  This is state-programming evidence only; no
pixel/frame result is claimed.

### Integrated `ui.copy` draw and pixel proof

`tests/isolated/uicopy/` drives the exact three-instruction `ui.copy` token
stream through the real guest COM/stdcall bridge, `d3d9_core`, the real
Citro3D backend, `C3D_DrawArrays`, and `Present`.  It uses a position+texcoord0
fullscreen triangle and one 8x8 texture with 64 distinct known texels.  The host
mock regression checks shader-pointer propagation, native texture binding and
upload bytes, draw/Present counters, vertex/clip state, and sampler/render
state.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_host_tests.ps1 `
  -Only uicopy_host -OutDir build_host_uicopy

# From tests/isolated/uicopy, with devkitPro MSYS2 make on PATH:
make clean
make DEVKITPRO=C:/devkitPro

# From the repository root:
powershell -ExecutionPolicy Bypass -File tools\run_azahar.ps1 `
  -ThreeDsx tests\isolated\uicopy\uicopy_arm.3dsx `
  -Name uicopy_arm -TimeoutSec 12
```

Final host result: **52 checks, 0 errors**.  Final ARM/Azahar result:

```text
ui.copy ARM: PASS (16 checks, 0 errors)
tev_apply_calls=1 draw_calls=1 primitive=0 first=0 size=3 cursor=3 scene=0
PIXEL_PROOF=PASS capture=128x128 detail=unknown=0 bad_counts=0 bad_shapes=0 expected_each=256
scope=synthetic ui.copy only; NOT a Skyrim menu or game frame
AZAHAR RESULT: PASS
```

The ARM fixture renders to a VRAM colour target, synchronizes after `Present`,
and reads the colour buffer back with `C3D_SyncDisplayTransfer`.  Every one of
the 64 expected texels must appear exactly 256 times in a contiguous 16x16
block, with no unknown pixels.  If capture allocation/transfer is unavailable,
the log explicitly reports `PIXEL_PROOF=NO` and only state/counter evidence is
claimed.  A transfer mismatch remains a test error.

Evidence: `build_azahar/20261124-203413-uicopy_arm_final/` and
`build_host_uicopy_final/logs/host_tests_20261124-203008-25544.log`.  The pixel
log includes the full-buffer FNV-1a32 hash `e34b4dc5` and all 64 block-center
words.  See `tests/isolated/uicopy/README.md` for the full contract and artifact
hash.

### TESV forced-memory-probe ARM run

The isolated TESV target built successfully:

```text
Skyrim_N3DS_tesv_picaprobe.3dsx
72,958,916 bytes
```

Azahar was run against that artifact:

```powershell
powershell -File tools/run_azahar.ps1 `
  -ThreeDsx Skyrim_N3DS_tesv_picaprobe.3dsx `
  -Name tesv_picaprobe -TimeoutSec 180 -AllowFaultReport
```

The forced probe got past the 512 MiB gate and executed all 200,000 steps.  It
then stopped at:

```text
Guest fault at 00403780: unknown indirect function
guest_used=35653056
IR cache: blocks=0 bytes=0 hits=0 misses=0 evicted=0
```

Evidence: `build_azahar/20261124-182727-tesv_picaprobe/`.  The runner registers
the PICA backend before guest `CreateDevice`, but this run produced no
draw-specific evidence before the fault.  The guest did **not** reach a
confirmed D3D9/PICA draw, and no Skyrim frame is claimed.

## 4. Applied integration

The following changes are applied in this pass.  They are recorded here as the
exact integration reference for review and future backends.

### 4.1 `Makefile`

In both graphics object lists, add the validator and 3DS adapter immediately
after `d3d9_tev.o`:

```make
# GRAPHICS and GRAPHICS_COM OFILES
... d3d9_tev.o d3d9_pica_tev.o d3d9_pica_citro3d.o ...
```

TESV also includes the validator, 3DS adapter, citro3d backend, and shbin object
because its 3DS runner now uses PICA200 with a null fallback:

```make
# TESV_SLICE OFILES
... d3d9_tev.o d3d9_pica_tev.o d3d9_pica_citro3d.o ...
... d3d9_com.o d3d9_null.o d3d9_citro3d.o d9shader.shbin.o ...
```

TESV links `-lcitro3d -lopendsp -lctru -lm`; the host TESV test does not compile
the 3DS adapter.

### 4.2 `tools/run_host_tests.ps1`

Add a standalone host test without changing existing test sources:

```powershell
$picaSources = @("source/d3d9_pica_tev.c")
Add-Test "pica_tev" $common @(
    "source/d3d9_tev.c",
    "source/d3d9_pica_tev.c",
    "tests/pica_tev_smoke.c"
) @()
```

Every existing host test that compiles `source/d3d9_com.c` must also compile
`$picaSources`: `tev_com`, `com`, `adapter`, `device_create`, `null_backend`,
and the optional `tesv_slice`.  For example:

```powershell
Add-Test "tev_com" $liftCommon ($legacySources + @(
    "source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c",
    "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c",
    "source/d3d9_tev.c", "source/d3d9_com.c"
) + $picaSources + @("tests/tev_com_smoke.c")) @()
```

### 4.3 `source/d3d9_core.h`

Include the translated-program type and carry a borrowed pointer in the draw
state.  This avoids changing `D9Backend::draw` and therefore avoids churning all
existing mock backends:

```c
#include "d3d9_tev.h"
```

Append to `D9DrawState`:

```c
const D9TevProgram *pixel_shader; /* borrowed; transient, not guest render state */
```

Declare:

```c
int d9_set_pixel_shader(D9Device *d, const D9TevProgram *program);
```

### 4.4 `source/d3d9_core.c`

Add the active borrowed pointer to `D9Device`:

```c
D9Handle index,texture;
const D9TevProgram *pixel_shader;
```

Add the setter next to `d9_set_texture()`:

```c
int d9_set_pixel_shader(D9Device *d,const D9TevProgram *program){
    if(!d)return D9_INVALID;
    d->pixel_shader=program;
    return D9_OK;
}
```

In `draw()`, attach it to the per-draw state copy:

```c
D9DrawState state=d->state;
state.pixel_shader=d->pixel_shader;
```

Clear it before device teardown:

```c
if(d->pixel_shader)d->pixel_shader=NULL;
```

The pointer is not owned by core and must not be freed by core.

### 4.5 `source/d3d9_com.c`

Include:

```c
#include "d3d9_pica_tev.h"
```

In `shader_new()`, after guest bytecode is copied and before
`d9_shader_analyze_tev()`, preflight only ps_3_0 pixel shaders so VPOS and other
unsupported declarations get backend-specific names:

```c
if(kind==PS && count && code[0]==0xFFFF0300u){
    D9PicaTevReject reason;
    char backend_why[256];
    if(d9_pica_tev_preflight_pixel(code,count,&reason,
                                   backend_why,sizeof backend_why)){
        snprintf(g.last_shader_rejection,sizeof g.last_shader_rejection,
                 "PICA200 backend: %s",backend_why);
        fprintf(stderr,"D3D9 shader rejected: %s\n",g.last_shader_rejection);
        free(code);
        return (reason==D9_PICA_TEV_MALFORMED ||
                reason==D9_PICA_TEV_PROGRAM_VERSION) ? INVALID : UNAVAILABLE;
    }
}
```

After the existing analyzer accepts `D9_SHADER_TEV`, validate the translated
descriptor before retaining the shader object:

```c
if(kind==PS && result==D9_SHADER_TEV){
    D9PicaTevReject reason;
    char backend_why[256];
    if(d9_pica_tev_validate(&tev,NULL,&reason,
                            backend_why,sizeof backend_why)){
        snprintf(g.last_shader_rejection,sizeof g.last_shader_rejection,
                 "PICA200 backend: %s",backend_why);
        fprintf(stderr,"D3D9 shader rejected: %s\n",g.last_shader_rejection);
        free(code);
        return UNAVAILABLE;
    }
}
```

Keep the existing `o->tev=tev; o->has_tev=1;` retention.

Update `set_ps()` to install the borrowed core pointer before releasing the old
COM binding:

```c
static void set_ps(CPU *c){
    Obj *o=obj(A(1),PS);
    if(!obj(A(0),DEVICE)||(A(1)&&!o)||g.phase==2)RET(INVALID,2);
    const D9TevProgram *program=o&&o->has_tev?&o->tev:NULL;
    if(d9_set_pixel_shader(g.device,program))RET(INVALID,2);
    bind(&g.ps,o);
    RET(OK,2);
}
```

The accepted SM2 texture template has `has_tev == 0` and intentionally maps to
the existing fixed-function path (`program == NULL`).

Fix `shader_state()` so a literal zero-texture TEV program can draw without a
texture, while the SM2 template still requires one:

```c
static int shader_state(void){
    int needs_texture=g.ps && (!g.ps->has_tev || g.ps->tev.textures!=0);
    if(needs_texture&&!g.tex)return D9_INVALID;
    if(!g.vs)return d9_set_shader_clip(g.device,NULL);
    if(g.constants_mask!=15)return D9_INVALID;
    return d9_set_shader_clip(g.device,&g.constants);
}
```

In `destroy_obj()` for `DEVICE`, clear the borrowed pointer before `g.ps` is
cleared/released:

```c
if(g.device)d9_set_pixel_shader(g.device,NULL);
g.ib=g.tex=g.vs=g.ps=g.declaration=NULL;
```

Also clear it in `d9_com_shutdown()` before the object loop frees shader storage.
This matters for an adopted `D9Device`, which survives COM shutdown:

```c
if(g.phase)d9_present(g.device);
g.phase=0;
if(g.device)d9_set_pixel_shader(g.device,NULL);
```

Lifetime rule: COM retains the bound PS object; core only borrows its `tev`
member.  Unbinding, device destruction, or COM shutdown must clear the core
pointer before the old COM object can be released.

### 4.6 `source/d3d9_citro3d.c`

Include:

```c
#include "d3d9_pica_citro3d.h"
```

Keep the existing native texture filter/mip/wrap code, but factor it into a
helper that runs before either TEV or fixed-function setup:

```c
static void configure_native_texture(void *texture,const D9DrawState *state){
    NativeTex *t=texture;
    D9CitroTextureConfig config=d9_citro_texture_config(t->levels,state);
    t->tex.minLevel=(u8)config.min_level;
    t->tex.maxLevel=(u8)config.max_level;
    C3D_TexSetFilter(&t->tex,config.mag_filter==2?GPU_LINEAR:GPU_NEAREST,
                     config.min_filter==2?GPU_LINEAR:GPU_NEAREST);
    C3D_TexSetFilterMipmap(&t->tex,config.mip_filter==2?GPU_LINEAR:GPU_NEAREST);
    C3D_TexSetWrap(&t->tex,wrap(state->address_u),wrap(state->address_v));
}
```

`d9_citro_texture_config()` is a pure helper in `d3d9_citro3d.h`.  It clamps
`MAXMIPLEVEL` to `[0, resident_mips-1]` but does not collapse that range for
`MIPFILTER_NONE`.  The backend maps NONE to `GPU_NEAREST` for the mip filter:
PICA still selects a mip from the resident range, but does not interpolate
between adjacent mip levels.  MAGFILTER and MINFILTER are unchanged.

Replace the current single-stage REPLACE block in `draw()` with:

```c
if(texture) configure_native_texture(texture,state);

if(state->pixel_shader){
    NativeTex *native=texture;
    if(d9_pica_citro3d_apply_tev(state->pixel_shader,
                                 native?&native->tex:NULL,
                                 NULL,0))return -1;
}else{
    d9_pica_citro3d_restore_tev();
    C3D_TexEnv *env=C3D_GetTexEnv(0);
    if(!env)return -1;
    if(texture){
        NativeTex *native=texture;
        C3D_TexBind(0,&native->tex);
        C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,0,0);
    }else{
        C3D_TexEnvSrc(env,C3D_Both,GPU_PRIMARY_COLOR,0,0);
    }
    C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
}

C3D_DrawArrays(GPU_TRIANGLES,(int)c->cursor,(int)n);
d9_pica_citro3d_restore_tev();
c->cursor+=n;
return 0;
```

A TEV validation/apply failure returns `-1`; core reports `D9_BACKEND`.  Do not
fall back to the fixed REPLACE stage for an unsupported program.

`begin()` also calls `d9_pica_citro3d_restore_tev()` so every new frame starts
from the same TEV state.

### 4.7 `source/tesv_slice_main.c` and TESV ARM linking

On 3DS, `main()` opens citro3d immediately after `gfxInitDefault()` and passes
the backend into `run()`.  After `d9_com_init()` and before guest `CreateDevice`,
the runner calls `d9_com_set_backend()`.  If citro3d open or registration fails,
it explicitly installs the null backend.  `d9_com_shutdown()` and device teardown
run inside `run()`; `d9_citro_close()` runs afterward in `main()`.

The host build passes no backend and continues to use `d9_null_register()`.

TESV ARM builds now include `d3d9_pica_tev.o`, `d3d9_pica_citro3d.o`,
`d3d9_citro3d.o`, and `d9shader.shbin.o`, expose the build directory for the
generated shbin header, and link `-lcitro3d -lopendsp -lctru -lm`.

Use a forced-memory-probe isolated build for renderer-reach verification:

```powershell
powershell -File tools/build.ps1 -Mode tesv `
  -TesvGen E:\Skyrim_N3DS_build\analysis\growth13\gen01 `
  -Isolated picaprobe -ForceMemProbe
```

Without `-ForceMemProbe`, the guest exits at the expected 512 MiB memory gate
before renderer initialization.

### 4.8 Regression tests

- Extend a mock backend to record `state->pixel_shader` and assert that COM
  passes the bound TEV program through core.
- Draw an accepted literal tint and assert the recorded stage/texture metadata.
- Draw an accepted zero-texture literal program with no bound texture.
- Assert `SetPixelShader(NULL)` and the accepted SM2 template both restore the
  fixed-function null-program path.
- Leave a TEV shader bound while shutting down COM with an adopted `D9Device`,
  then draw directly from core and assert the borrowed pointer was cleared.
- Assert multi-texture/external-constant/vertex-colour/VPOS PS creation returns
  `UNAVAILABLE` (or `INVALID` for malformed tokens) with the named reason.
- Run `pica_tev`, `tev_translate`, `tev_accuracy`, `tev_com`, `com`, and the
  graphics host tests, then rebuild `GRAPHICS` and `GRAPHICS_COM` for ARM.

## 5. First-frame audit follow-up (2026-11-24)

The audit findings below remain split by ownership.  This pass closes the two
backend/vertex items with regressions; viewport is proven but still requires the
core/COM hand-off documented in 5.2.

### 5.0 Closed in the shader pass

- `texld` UV registers are bounds-checked; unsupported register-token high bits,
  source/destination modifiers and relative addressing are rejected.
- Non-finite or out-of-[0,1] `def` literals are rejected instead of clamped.
- Menu-light stage 4 now matches the exact evaluator: RGB × `c7.a`, alpha
  passthrough, `c7.rgb` ignored.

### 5.0 Single LDR render-target surface

The production backend now implements one explicit `D3DUSAGE_RENDERTARGET`
`D3DRTYPE_SURFACE` in addition to the implicit 240x400 screen target:

- core owns up to four handles, one active handle (`0` is the implicit back
  buffer), a 4 MiB aggregate budget and native lifetime;
- COM implements `CreateRenderTarget`, `SetRenderTarget` (including `NULL`
  restore), `GetRenderTarget`, surface description/container and read-only
  `LockRect`;
- PICA allocates a power-of-two 8..1024 `GPU_RGBA8` texture in VRAM and wraps it
  with `C3D_RenderTargetCreateFromTex(..., GPU_RB_DEPTH24_STENCIL8)`, so every
  explicit target (and the retained lockable back buffer) has a real depth
  attachment;
- `BeginScene` binds the selected target and applies the queued colour clear
  only when the guest actually called `Clear`; mid-scene `SetRenderTarget`
  rebinds immediately;
- a queued Clear owns its target. Switching away before BeginScene is rejected
  explicitly instead of clearing the wrong surface;
- readback uses `C3D_SyncTextureCopy` into linear memory followed by
  `d9_unswizzle_rgba8`. PICA `GPU_RGBA8` words are `0xAABBGGRR`; the inverse
  converts to top-first D3D `B,G,R,A` and forces alpha for `X8R8G8B8`;
- target release is forbidden while a scene is active. Deleting the selected
  target restores core target zero; the next frame binds the screen target.

Only `D3DMULTISAMPLE_NONE`, quality zero and no shared handle are accepted.
`D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` is a real feature: core creates a retained
512x256 RGBA8 target for the 400x240 mode, `BeginScene` binds it, `Present` blits
it to the screen, `LockRect`/`UnlockRect` read/write it through
`C3D_SyncTextureCopy`/`C3D_TexLoadImage`, and `StretchRect` can now sample it.
Explicit `D24S8`/`D24X8` depth-stencil surfaces are supported as descriptors of
the active target's single PICA `DEPTH24_STENCIL8` attachment
(`CreateDepthStencilSurface`, `SetDepthStencilSurface`, `Clear(Z|STENCIL)`):
pow2 8..1024 or the exact 400x240 back-buffer size, bound only when the size
matches the active target (the 400x240 back buffer or a render-target texture's
backing target). No separate VRAM is allocated for a depth descriptor.
A lockable target requires both a readback and a
write-back callback; the null backend keeps real BGRA pixels (including a
private 400x240 presented image) and implements both, so the host tests verify
the pixels instead of only the call stream. Texture render targets, MRT,
resolve, `ColorFill` and sampling a non-lockable implicit back buffer remain
unsupported. `StretchRect` is implemented for explicit-target and retained-back
buffer source/destination copies through the optional backend `target_blit`;
format conversion is rejected. The existing screen-oriented clip transform is
reused, so arbitrary offscreen orientation/scale is not claimed. The pixel proof
uses a square target and an oversized fullscreen triangle, where rotation cannot
change coverage.

`tests/isolated/uicopy/rt_switch.c` drives the real guest COM -> core -> PICA path
with two independent 64x64 targets. It proves solid red/green clears, independent
readback, `ui.copy` drawing only into target A, unchanged target B, explicit
identity queries and `SetRenderTarget(NULL)` restoration. Azahar 2126.2 recorded
38 checks / 0 errors and pixel FNV-1a32 `54d9adc5`; evidence is
`build_azahar/20261125-211721-rt_switch_arm`. This is synthetic pixel evidence,
not a Skyrim frame.

### 5.1 Closed in the backend/vertex pass

**`MIPFILTER_NONE` no longer collapses the mip range.**
`d9_citro_texture_config()` computes `max = min(resident_mips-1, levels-1)` and
`min = min(MAXMIPLEVEL, max)` for every mip filter.  NONE keeps that range and
maps only the mip filter to `GPU_NEAREST`; point and linear retain their
respective nearest/linear mip selection.  MAGFILTER, MINFILTER and wrapping are
unchanged.  The previous `if(!state->mip_filter)max=min` is removed.

**Fixed-function colour declarations are rejected instead of silently dropped.**
`D9_USAGE_COLOR` is the D3D9 usage value 10.  `d9_decl_init()` rejects it for
every declaration type and usage index because core's fixed-function fetch path
only carries position and UV.  This also makes COM `CreateVertexDeclaration`
return `D3DERR_INVALIDCALL`.  Packed `D9_DECL_COLOR` decoding remains available
to callers of `d9_decl_decode()`, but a colour-bearing declaration is not
accepted until a colour varying is implemented.  Position + texcoord0 still
passes and draws through the existing multi-stream path.

### 5.2 Viewport: proven gap and exact core/COM hand-off

`SetViewport` currently validates and stores only `g.viewport[6]` in
`source/d3d9_com.c`.  `D9DrawState` has no viewport, and the citro3d backend never
calls `C3D_SetViewport()` or `C3D_DepthMap()`.  `C3D_FrameDrawOn()` resets the
viewport to the full 240x400 render target, so a non-default D3D9 viewport and
its MinZ/MaxZ range are accepted but ignored.

The following changes are **not applied in this pass** because core and COM are
owned elsewhere.

1. Add a backend-neutral viewport value to `source/d3d9_core.h`, carry a copy in
   each draw state, and declare a setter:

   ```c
   typedef struct {
       uint32_t x,y,width,height;
       float min_z,max_z;
   } D9Viewport;

   /* Inside D9DrawState: */
   D9Viewport viewport;

   int d9_set_viewport(D9Device *d,const D9Viewport *viewport);
   ```

2. In `source/d3d9_core.c`, store `D9Viewport viewport` in `D9Device`, initialize
   it to `{0,0,400,240,0,1}`, validate it, and attach it to the per-draw copy:

   ```c
   int d9_set_viewport(D9Device *d,const D9Viewport *v){
       if(!d||!v||!v->width||!v->height||
          (uint64_t)v->x+v->width>400u||
          (uint64_t)v->y+v->height>240u||
          !isfinite(v->min_z)||!isfinite(v->max_z)||
          v->min_z<0.0f||v->max_z>1.0f||v->min_z>v->max_z)return D9_INVALID;
       d->viewport=*v;
       return D9_OK;
   }

   /* In draw(), after the existing per-draw state copy: */
   state.viewport=d->viewport;
   ```

   The 400x240 bound was exact for the implicit back buffer. Explicit 8..1024
   power-of-two targets now exist. COM resets the viewport to the full active
   target on `SetRenderTarget`, but this viewport hand-off to core/PICA is still
   not applied: only a full-target viewport is accepted and PICA still uses the
   full target dimensions. A non-default viewport and offscreen orientation
   therefore remain unproven.

3. In `source/d3d9_com.c`, convert the validated six guest words to `D9Viewport`
   and forward them before storing the guest-visible values:

   ```c
   D9Viewport viewport={v[0],v[1],v[2],v[3],minz,maxz};
   if(d9_set_viewport(g.device,&viewport))RET(INVALID,2);
   memcpy(g.viewport,v,sizeof g.viewport);
   RET(OK,2);
   ```

   `default_viewport()` must also call `d9_set_viewport()` with
   `{0,0,g.pp[0],g.pp[1],0,1}` before serializing those defaults.  It is called
   after CreateDevice, Reset and SetRenderTarget, which are exactly the points
   where D3D9 restores the full-target viewport.  Keep COM's existing bounds and
   finite/range checks; the core setter is the authoritative state hand-off.

4. Once that state exists, apply it in `source/d3d9_citro3d.c` before each draw.
   The current clip transform rotates the 400x240 D3D9 display clockwise into
   the 240x400 top-screen target, so the viewport rectangle must rotate and
   swap its dimensions:

   ```c
   static void apply_viewport(const D9Citro *c,const D9DrawState *s){
       const D9Viewport *v=&s->viewport;
       unsigned target_w=c->target->frameBuf.width;
       unsigned target_h=c->target->frameBuf.height;
       uint32_t right=v->x+v->width,bottom=v->y+v->height;
       unsigned x=(unsigned)((uint64_t)target_w*(240u-bottom)/240u);
       unsigned y=(unsigned)((uint64_t)target_h*(400u-right)/400u);
       unsigned w=(unsigned)((uint64_t)target_w*v->height/240u);
       unsigned h=(unsigned)((uint64_t)target_h*v->width/400u);
       if(!w)w=1;
       if(!h)h=1;
       C3D_SetViewport(x,y,w,h);

       float range=v->max_z-v->min_z;
       if(range>0.0f)
           C3D_DepthMap(true,-1.0f/range,(v->max_z-1.0f)/range);
       else
           C3D_DepthMap(true,0.0f,1.0f-v->min_z);
   }
   ```

   Call this from `apply_state()` (which must then receive `D9Citro *c`) or from
   `draw()` immediately before `apply_state()`.  No scissor is needed: the PICA
   viewport performs the clip.  Integer floor mapping is deterministic; the
   one-pixel minimum prevents a legal one-pixel D3D9 viewport from collapsing.

   The depth formula follows the existing shader conversion
   `z_clip = z_d3d - w` and reverse-depth convention.  PICA computes
   `depth = (z_clip/w) * scale + offset`; mapping D3D9 `[MinZ,MaxZ]` to the
   existing reversed range therefore requires `scale = -1/(MaxZ-MinZ)` and
   `offset = (MaxZ-1)/(MaxZ-MinZ)`.  A degenerate `MinZ == MaxZ` viewport maps
   every fragment to `1-MinZ`.  The default `[0,1]` viewport preserves the
   current clear-at-zero/reversed-depth behavior.

   The transform was cross-checked against citro3d's
   [`C3D_SetViewport`](https://github.com/devkitPro/citro3d/blob/master/source/base.c)
   and [`C3D_FrameDrawOn`](https://github.com/devkitPro/citro3d/blob/master/source/renderqueue.c),
   plus Azahar's PICA software rasterizer formula
   [`screen = (ndc + 1) * halfsize + offset` and `depth = z/w * scale + offset`](https://github.com/azahar-emu/azahar/blob/master/src/video_core/renderer_software/sw_rasterizer.cpp).

Viewport regression coverage must assert the default and a non-full viewport in
a recording `D9DrawState`, COM `GetViewport` after Set/Reset/SetRenderTarget, and
the rotated rectangle plus depth-map parameters in an ARM state probe. Full
viewport carry-through remains open; the current safe behavior explicitly rejects
non-default viewports and preserves the existing viewport on SetRenderTarget.

### 5.3 Closed in the core/COM pass

- Textured draws require declaration texcoord0 instead of substituting `(0,0)`.
- VS constants are restricted to the supported `c0..c3` range.
- Initial state now matches D3D9 defaults: WRAP, POINT min/mag, CULL_CCW and
  alpha compare LESS.
- Rectangle and multiple queued Clear calls are explicitly rejected rather than
  silently discarded.
- Non-default viewports are explicitly rejected; SetRenderTarget no longer resets
  the viewport.

### 5.4 Focused evidence for this pass

Host, compiled directly with clang (the out-of-scope `run_host_tests.ps1` was not
run):

- `vertex`: PASS — declaration formats, explicit packed/float/secondary colour
  rejection, position+UV draw path, mip-range/filter assertions, multiple
  streams, INDEX32, strip/fan winding and bounds.
- `graphics`: PASS — existing texture, transform, draw and resource-limit tests.
- Both also PASS with `-fsanitize=address,undefined -fno-omit-frame-pointer`.

ARM11:

- `source/d3d9_vertex.c` and `source/d3d9_citro3d.c` compile with devkitARM and
  `-Werror -Wno-misleading-indentation` (the suppression covers existing
  compressed-statement warnings in these files).
- `tests/isolated/vertex_state/` links against citro3d/libctru and runs in
  Azahar as a state-only probe:

  ```text
  vertex_citro3d_arm: PASS (15 checks, errors=0; no draw)
  AZAHAR RESULT: PASS
  ```

  Evidence: `build_azahar/20261124-194543-vertex_citro3d_arm_final/`.

This is state-programming and host-logic evidence only.  No draw, pixel result,
frame, or visual-equivalence claim is made.  Remaining integration dependencies
are the viewport core/COM changes above and their recording/ARM regressions.

## 6. Deliberate non-goals

- No external pixel-shader constant upload path.
- No second/third texture unit or sampler.
- No vertex-colour or fragment-varying routing.
- No VPOS/VFACE support.
- No MRT, depth-output, cube/volume sampler, or non-TEV fragment operation.
- No claim that the wider translator subset is renderable by this backend.
- No claim of a real Skyrim frame or pixel-equivalent PICA rendering.
