# Device bring-up notes (Azahar 2126.2, New 3DS)

This note records what the port actually does when launched as a `.3dsx` in
Azahar with the real game data on `sdmc:/Skyrim`. It is not a playable build
and no frame or pixel result is claimed.

## CIA build

`tools/build_cia.ps1` packages a finished TESV-mode ELF into an installable
CIA for New 3DS. It is a packaging step only: the ELF is built by
`tools/build.ps1 -Mode tesv`, and no game logic, shader or menu source is
touched.

### Requirements

- devkitPro with `makerom` and `bannertool` in `%DEVKITPRO%\tools\bin`
  (verified with makerom 0.19.0). The script falls back to
  `C:\devkitPro\tools\bin` and then `PATH`; override with `-Tools <dir>`.
- Windows PowerShell 5.1 (`System.Drawing` is used for the generated icon and
  banner).
- `staged/` (regenerate with `python tools/stage_upstream.py ../Skyrim_Android staged`)
  and a lifted TESV generator directory containing `tesv_table.c`.
- The ELF at `dist/Skyrim_N3DS_tesv/Skyrim_N3DS_tesv.elf`.

### Exact commands

```powershell
cd <repo>
python tools/stage_upstream.py ../Skyrim_Android staged
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 `
    -Mode tesv -TesvGen E:\Skyrim_N3DS_build\live_arm_check2\gen -Jobs 8
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_cia.ps1
```

`build_cia.ps1` accepts `-Elf`, `-Out`, `-Rsf`, `-WorkDir` and `-Tools`; all
have defaults, so the bare command above reproduces the artifact.

### Artifact and checks

- CIA: `build_cia/Skyrim_N3DS_tesv.cia`
- Manifest: `build_cia/cia_manifest.json` (sizes, SHA256 for ELF/RSF/CIA,
  title ID, tool paths)
- Verification report: `build_cia/cia_info.txt`
- Title ID: `000400000F0D5900` (`TitleInfo/UniqueId: 0xF0D59` in
  `tools/cia/template.rsf`)
- Exheader: `SystemMode 96MB`, `SystemModeExt 178MB`, `CpuSpeed 804MHz`,
  `EnableL2Cache true`, `CanAccessCore2 true` — New 3DS only.

makerom 0.19 has no `-i` inspector, so the script parses the CIA itself: it
checks the `0x2020` header and section sizes, locates the embedded `NCCH`
block, and requires the NCCH partition ID to equal the RSF-derived title ID.
It also requires the `SMDH` meta block (icon/banner) to be present. The build
is made with `-target t` (test keys, production certs) and `-exefslogo`.

### Installing

- **Azahar**: `File > Install CIA...` and pick
  `build_cia/Skyrim_N3DS_tesv.cia`. Azahar's CLI has no headless install, so
  this must be done in the GUI. Only the structure was verified here; no CIA
  boot in Azahar is claimed.
- **Hardware (New 3DS with CFW, e.g. Luma3DS)**: copy the `.cia` to
  `sdmc:/cia/`, open FBI (or GodMode9's CIA installer) and install it. Luma
  patches signature checks, so the test-signed CIA installs without a
  Nintendo signature. Old 3DS cannot run it because of `SystemModeExt 178MB`
  and `CpuSpeed 804MHz`.
- Game data is still expected at `sdmc:/Skyrim` (IR container at
  `sdmc:/Skyrim/ir/tesv.irc`), exactly as for the `.3dsx` runs.

Do not distribute the CIA: it contains generated code derived from locally
owned game files. No frame, pixel or playability result is claimed for the
CIA build; `DevCaps` is unchanged.

## Boot path reached on the device

1. CIA/3DSX starts, PICA200 backend opens, D3DX shims register, the IR
   container loads from `sdmc:/Skyrim/ir/tesv.irc`.
2. Guest memory initialises. The first device run failed because the guest
   slab table was capped at 8 (`guest_slab_commit` could not find the 9th
   reservation). Raising `GUEST_MAX_SLABS` to 32 and the newlib heap to
   96 MiB (guest budget 92 MiB) removed that failure.
3. The engine opens `Skyrim - Interface.bsa`, `Animations.bsa`, `Meshes.bsa`
   and the other archives. Paths arrive both as `C:\Skyrim\...` and as
   `sdmc:/Skyrim\...` with mixed separators and trailing `\`; the sandbox
   mapper now accepts all of those forms (and strips the duplicated
   `Skyrim` component).
4. Renderer initialisation then stops on the documented honest gates:
   - `Hardware T&L required but not supported by Adapter.`
   - after the T&L gate: `ShaderModel3 required but not supported by your
     video card.`
   Both are intentional: `DevCaps` stays `0x00082000` and SM3 execution does
   not exist.

## Measurement-only probe switch

`source/tesv_slice_main.c` checks for `sdmc:/Skyrim/tesv_caps_probe.flag`.
When the file exists, it sets:

```text
TESV_D3D9_CAPS_PROBE=1        # advertise HW T&L (fictional)
TESV_D3D9_SM3_GATE_PROBE=1    # pass the 0x00c6e23b SM3 gate
TESV_D3D9_SHADER_SKIP_PROBE=1 # code-less shader objects
```

Every probe logs that it is measurement-only and not shippable. With the flag
present the device passes the T&L and SM3 checks, creates the shader objects
and then faults at `Guest fault at 00000030: unmapped guest memory`.

### The `0x30` fault is a VRAM allocation failure, not a NULL shader

The last device log before the fault is:

```text
[d3d9-citro] CreateRenderTarget: 256x256 VRAM texture allocation failed
[d3d9] CreateTexture: CreateRenderTarget: backend could not allocate a 256x256 native target
[guest] unmapped addr=00000030 bytes=4 last_call=00c8fc90 irc_fn=00fb5e60 ...
[guest] esp=1007fbac stack=541c26c0 00c7f10f ...
```

* `0x00c8fc90` is the `NiRenderTargetGroup` buffer getter
  (`mov esi,ecx; mov ecx,[esi+0x30]`). The faulting call is the one from
  `0x00c7f10a`, whose return address `0x00c7f10f` is in the stack dump.
* Its `this` is `[esi+0x7c]`, the second group created by the renderer-init
  helper `0x00c92330`. That helper returned NULL because the 256x256
  `CreateTexture` (render-target texture) failed. The engine does not null
  check this second group and calls the getter with `ecx=0`, which faults on
  `[0+0x30]`.
* `irc_fn=00fb5e60` is only the preceding six-byte constant getter
  (`mov eax,0x01bbfbac; ret`); it is the last IR function entered, not the
  faulting site.

The host census takes the same route past this point because the null backend
has no VRAM limit. On PICA every explicit target used to allocate colour
**plus** a full-size `DEPTH24_STENCIL8` attachment. With the 256-per-axis
native cap that is 512 KiB per target; the measured shadow/reflection set,
the retained 512x256 back buffer, the 400x240 screen target and the libctru
framebuffer banks exceed the 6 MiB PICA VRAM.

### Lazy depth for explicit targets

`D9Backend` gained the append-only `target_has_depth` / `target_depth_attach`
pair. PICA now creates explicit colour targets **colour-only**
(`C3D_RenderTargetCreateFromTex(..., -1)`) and attaches `DEPTH24_STENCIL8` only
when a matching `SetDepthStencilSurface` is actually bound, outside
`BeginScene`/`EndScene`. The retained back buffer still gets depth eagerly.
A failed attach is a named `D9_MEMORY` refusal
(`depth-stencil attachment allocation failed for the %ux%u target (VRAM)`) and
leaves the previous binding intact; no NULL is masked. The backend logs
`target WxH colour-only; vram free N KiB` and the same free value on attach or
failure, so the next run shows the real VRAM headroom. `tests/pica_lazy_depth_smoke.c`
covers colour-only creation, one on-demand attach, a VRAM-full named refusal,
no re-attach, `Clear(Z)` and teardown. Host `pica_depth_attachment`,
`render_target_budget` and `depth_target_matching` still pass.

The measurement build `E:\azahar_run\Skyrim_N3DS_tesv_lazy_depth.3dsx` carries
this change; the next Azahar run tells whether the 256x256 group now builds and
which layer follows.

`stderr` is mirrored to `sdmc:/Skyrim_N3DS_stderr.log` for these runs; the
final `sdmc:/Skyrim_N3DS_tesv.log` carries `rc`, exit code/from and the
unresolved-import list.

## Host census state (same route, host slice)

The host census now reaches `SetRenderTarget` and `SetDepthStencilSurface`.
The engine binds a 512x1024 depth while the implicit back buffer is 400x240;
the bridge returns `D3DERR_INVALIDCALL` with both sizes. The engine then
recurses through `NiDX9Renderer::BeginUsingRenderTargetGroup` until the 64 KiB
worker stack overflows at `0x1d1ffffc`. The remaining work is the pair itself
(a real 512x1024 colour target/group registration), not the error code.

## Three-profile memory route (96/8, 104/16, 120/24)

Build one isolated artifact per profile; do not reuse a `build/` directory
between them:

```powershell
foreach ($p in @("96x8","104x16","120x24")) {
  powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 `
      -Mode tesv -TesvGen <gen> -MemProfile $p -Isolated "mem$p" -Jobs 8
}
```

Run the same route on each artifact, on the same SD card and settings:
cold start → menu → first exterior cell → neighbour cell → return. Before
leaving the route, let it render past at least one 300-frame snapshot (or set
`N3DS_MEM_EVERY=60` for a denser trace) and then exit with START so the `exit`
line is written.

Collect `sdmc:/Skyrim_N3DS_mem.log` after each run. The log starts with
`mem tag=boot ... heap_mib=` so a file cannot be attributed to the wrong
profile. Check, per run:

1. both heaps initialised (`[mem] profile ...` banner, no svcBreak);
2. `start` and `exit` lines exist, plus at least one `frame` or `cell` line;
3. peak acceptance: `newlib_free >= 4 MiB` and
   `linear_free/linear_min_free >= 2 MiB` at the worst snapshot;
4. no `mem tag=alloc_fail`; if one exists, its `fail_kind`/`fail_bytes` names
   the rejected request.

Host-side pre-check (profile arithmetic, parser, fake allocator, line format):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/check_mem_profile.ps1
```

This is measurement only: it does not claim a frame, a working set or a
shippable heap. 96/8 stays the default until 120/24 passes the route above on
hardware.

## Not implemented / not claimed

- No HW T&L and no SM3 execution; both gates are honest refusals.
- No 512x1024 colour render target/group for the depth pair.
- No frame, no pixel comparison, no hardware run.

## Measuring the 512x1024 depth pair

For the host census, add `TESV_D3D9_DEPTH_PAIR_PROBE=1` to the existing
measurement environment. In a TESV build containing this change, the
independent SD switch is `sdmc:/Skyrim/tesv_depth_pair_probe.flag`.
CreateDevice and Reset allocate a full 512x1024 colour target and Present
scales it to the 400x240 display. Allocation can fail on PICA. The pair probe
prints `MEASUREMENT ONLY` and does not change caps or shader execution.
Enable the existing D3D9 trace to record successful depth binds with the
active colour surface and both dimensions. Count guest SetTexture, draw and
Present calls separately from the bridge regression `depth_pair_scaled`.
