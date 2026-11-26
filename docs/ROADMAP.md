# ROADMAP — Skyrim 2011 → New Nintendo 3DS

Legend: `[x]` verified in this workspace, `[~]` partially implemented or
emulator-only, `[ ]` not started. A milestone gate is only met when the stated
evidence exists; nothing here implies hardware success.

## M0 — Reproducibility and honest memory measurement

- [x] All baseline host tests build and pass from one runner
      (`tools/run_host_tests.ps1`): host, lift, boot, graphics, vertex,
      bc_shader, com, pilot_real, hotpatch resolver.
- [x] ASan build/run of the 7 C tests (`-Sanitize`).
- [~] All ARM targets build and launch in Azahar (diagnostic, lift, boot,
      graphics, graphics_com, pilot). Logs/screenshots in `build_azahar/`.
- [x] Game audit with hashes, image sizes, executable bytes, imports, exports.
- [~] Memory table with measured ARM `.text/.bss`, guest heap and pilot
      footprint. Real-device OS reserve still unknown; Azahar's reported
      "free 4068 MiB" is emulator host memory and must not be used.
- [ ] Dedicated Azahar profile with extra RAM disabled and documented
      screenshots for every run (current profile is the user's, New3DS=true).

**Gate:** baseline tests + demos pass in Azahar; measurements/blockers table
with sources. *Almost met; hardware budget unknown.*

## M1 — Real D3DX pilot

- [x] `prepare_pilot.py` runs against the real 32-bit `D3DX9_42.dll`.
- [x] Discover closed direct-call slice, relocations, generated function table,
      guest ABI, x87 handling, return convention.
- [x] MSVC lazy-ILT thunk detected, verified byte-for-byte, real body resolved,
      bypass recorded in manifest and logs; unit tests cover malformed variants.
- [x] Host differential check: 3/3 for all nine lazy-ILT exports
      (`MatrixMultiply`, `MatrixTranspose`, `MatrixMultiplyTranspose`,
      `PlaneTransform`, `Vec3TransformCoord`, `Vec3TransformNormal`,
      `Vec3Normalize`, `PlaneNormalize`, `MatrixInverse`), plus the internal Vec3
      helper and the `_CIsqrt`/`_finite` host shims; canaries, the out_len tail,
      all 16 matrix floats and the determinant are checked.
- [x] ARM TESV build with the 12-entry table (`d3dx12`/`d3dx13`); hashes are in
      the artifact manifest. The only Azahar PASS is still the older
      `D3DXMatrixMultiply` run; the full table has not been launched in Azahar.
- [ ] Real New 3DS control run.
- [ ] Azahar run of the nine-body table (desktop unavailable during this work).

**Gate:** original user DLL correct on host and PASS (3/3) in Azahar. *Met for
Multiply/Transpose in Azahar; hardware pending.*

## M2 — Scaling static recompilation

- [x] Measured expansion on a real function: 217 x86 insns → 12,348 B ARM
      (`-O2`), 12,572 B (`-Os`). This is the current blocking number.
- [x] Unprotected same-build executable supplied by the owner
      (`E:\skyr\TESV.exe`, verified by section comparison; no DRM bypass).
- [x] Full TESV discovery: 90,900 functions, 94.7% of text covered; results in
      `E:\Skyrim_N3DS_build\analysis\tesv\functions.json`.
- [x] Direct-call closure from entry measured: 13,390 functions, 955k insns,
      ~51.8 MiB projected ARM `.text`; 159 imports, 12,472 dynamic sites.
- [x] Budget modes accounted: 96 MiB `.3dsx` region vs 178 MiB CIA
      `SystemModeExt` (N3DS DEV1). Startup slice (~80 MiB before GPU) is
      plausible at 178 MiB, not at 96 MiB. Full-game lift remains impossible.
- [x] Local CIA packaging (`tools/build_cia.ps1`, `tools/cia/template.rsf`).
- [ ] Confirm 178 MiB at runtime/device and measure the GPU/texture budget.
- [x] `tools/slice_pe.py` emits bounded slices/reports for lift.py and locally
      decodes direct targets missing from the discovery manifest
      (`tests/slice_pe_test.py`).
- [x] Real PE mapping + IAT walk (`tests/tesv_probe.c`): 266 imports, 187
      unresolved, first misses in exact order.
- [x] First bounded startup slice lifted and executed (70 functions + 2 TLS
      callbacks) on host and in Azahar. Dynamic chain fixed so far:
      HeapCreate → indirect `0xf76345` → indirect `0xf5d5c3` → EncodePointer;
      current fault `0xf6916b`. Diagnostic only; not a boot.
- [ ] Automate fault-driven slice growth and reach a stable CRT/init stage.
- [ ] Cheaper lowering of guest memory/flags to reduce bytes per instruction.
- [ ] Direct/indirect calls, jump tables, tail calls, import thunks, callbacks
      at scale, with diagnosable refusal for unknown targets.
- [ ] x86 semantics test matrix expanded (flags, x87, SSE, string ops, atomics).
- [ ] Full-code size study and placement plan (sections, GC, overlays only if
      the platform actually allows it).

**Gate:** a meaningful TESV init slice recompiled with correct ABI in Azahar,
plus a measured placement plan. *Not met.*

## M3 — PE/Win32 bootstrap and TESV start

- [~] PE mapping, relocations, IAT/EAT walking, TLS templates, PEB/TEB,
      command line/environment, DllMain ordering, ExitProcess (self-authored
      fixtures only).
- [~] Real TESV module-load sequence, delay imports, forwarders, TLS callbacks:
      TESV + d3dx9_42 + binkw32 + X3DAudio1_7 + steam_api are mapped (the last
      four data-only), TESV's own TLS callbacks run, and unknown imports bind to
      a named fault trap (250 names observed for the current module set).
- [~] Startup-critical Win32: files/dirs, INI profile read/write, ANSI/Unicode,
      errors, handles, critical sections, waits/time, module lookup, file
      metadata/timestamps, environment; the remaining gaps are named explicitly.
- [x] Cooperative guest-thread model and scheduling (host-tested; ARMv6K backend
      still requires hardware verification).
- [~] Minimal proven SEH subset: the exact `SetThreadName` exception continues
      execution; general handler-chain dispatch and `RtlUnwind` still refuse.
- [~] Renderer gate: D3D9 enumeration, HAL caps, windowed checks and the COM
      device bridge exist. TESV now reaches renderer selection; the next blocker
      is honest `D3DUSAGE_RENDERTARGET` surface support before `CreateDevice`.

**Gate:** real TESV startup progresses through DLL/CRT and reaches engine
initialization inside generated code. *Met on the host and in Azahar: the lifted
CRT and the initializer table at `0x1076aa0` run, and the engine's memory check
at `0xa48a5d` is now the first refusal ("Not enough memory to run application.",
needs ≥512 MiB of page file). The next gate behind it is the D3D9/SM3 renderer
check.*

## M4 — Filesystem and game resources

- [ ] Map actual TESV access to `Data/`, BSA, ESM/ESP, INI, strings, meshes,
      textures, audio; case-insensitive path mapping inside a sandbox root.
- [ ] Streaming reads, bounded LRU caches, byte budgets, corruption handling.
- [ ] Reproducible resource tooling with manifests; no source-file mutation.

**Gate:** TESV opens real configuration and game data and completes a resource
read stage without fake successes. *Not met.*

### Checkpoint toward the main menu (2026-11-24)

- [x] Executes the original TESV code (lifted, host + Azahar parity).
- [x] Physical New 3DS diagnostic launch (`irprobe`, user-reported normal).
- [x] Full physical hybrid TESV (`hwhybridprobe`): 178 MiB region, IR executed,
      fault `0x00403780`, no loader/libctru crash.
- [x] Latest hybrid `hwbulk` reaches `0x0064be80` in Azahar (host parity), with
      32,199 decoded IR blocks; physical retest pending.
- [ ] Deep AOT CIA physical launch: compressed and no-compression variants hit
      the same system-loader trap; hybrid is the working hardware path.
- [x] Passes the minimum-system check in the probe-only diagnostic mode.
- [x] Substantial ARM code reduction (25.35 B/insn, −11.6 % measured).
- [x] Fix the allocator initialisation. Legal re-commit and 4 KiB commit
      alignment now work; lazy VA keeps a 95 MiB commit promise with only
      68–3,608 KiB physically backed across the observed startup stages.
      Next confirmed frontier: the input/controls manager needs
      `Interface/Controls/PC/ControlMap.txt` from `Skyrim - Interface.bsa`.
      The file is not loose, the load early-exits, and the later clear
      dereferences `[obj+0x34] == NULL` (fault at address 8). BSA reading or a
      loose-file extraction is the next gate. The SD (`F:`) was detached, so the
      game data was not reachable at the time.
- [x] Full hybrid TESV in Azahar (diagnostic probe): 32 AOT + 25,328 IR;
      host and ARM reach `0x00b2a970`, ARM IR cache records 859 blocks.
- [~] Hybrid AOT + IR tier. v2 implements x87 and the measured SSE/SSE2 set;
      **25,350/25,360** real functions decode, with only 10 functions / 0.24 MiB
      projected AOT forced by block-terminator edges. Real-profile 20% AOT + 80%
      IR projects to **32.3 MiB**; ARM interpreter text is 55,338 B. Equivalence
      and partition tests pass; Azahar measures IR warm at 5.24× AOT. Full hybrid
      TESV is linked (32 AOT + 25,328 IR; `.text` 1.44 MB, `.3dsx` 1.45 MB).
      Host and ARM probe runs both reach `0x00b2a970`; the ARM IR cache records
      859 blocks / 94,584 B / 18,468 hits. Hot-set selection supports measured-time
      ranking; the current real profile remains call-ranked.
- [x] Fit the deep slice in New 3DS memory. Current AOT 69.8 MiB + lazy VA
      ~32 MiB + 8 MiB linear + ~1 MiB rodata ≈ **111 MiB plus touched**, inside
      178 MiB. This does not require IR; IR is for code beyond startup.
- [ ] Indexed SD-backed IR container for all discovered functions with lazy
      load, bounded LRU and AOT precedence; removes recompilation on each
      unknown-function fault. Differential tests and iteration-time measurement
      required before integration.
- [~] D3D9 initialisation: enumeration and the exact LDR render-target surface
      gate are implemented; TESV now reaches the adapter caps check and stops at
      missing `HWTRANSFORMANDLIGHT`. SM3 remains the later architectural blocker.
- [x] Synthetic end-to-end `ui.copy` PICA pixel proof: COM → core → TEV →
      citro3d → Present, 128×128 readback verified. Not a Skyrim frame.
- [x] Synthetic render-target switching pixel proof: two independent 64×64 VRAM
      targets, clear/readback, `ui.copy` isolation and back-buffer restore; Azahar
      38 checks / 0 errors, FNV `54d9adc5`. Not a Skyrim frame.
- [x] First-menu shader closure identified and validated on retail data:
      `p1200005` lit logo PS, menu copy, fixed fog/UI, 390 possible generic VS
      declarations; 15 validator tests pass. This is a bounded closure, not a
      global SM3 claim. See `docs/MENU_SHADER_CLOSURE.md`.
- [x] Main-menu resource closure measured: `StartMenu.swf` (Update.bsa override),
      fonts, translations, logo/fog NIFs and DDS; BSA/SWF census tests pass. First
      UI-side blocker is the GFx dynamic vertex-buffer/FVF path and
      `SetTextureStageState`. See `docs/MENU_ASSET_CLOSURE.md`.
- [x] Bounded C BSA v104/v105 reader and a runtime `Interface/StartMenu.swf`
      resolver with the measured Interface-then-Update order, SWF header checks,
      guest-memory budget and named refusals; synthetic and real read-only tests
      pass. This is asset loading, not GFx draw. See `docs/MENU_BSA_RUNTIME.md`.
- [x] Bounded SWF container/tag summary for the resolved `StartMenu.swf`:
      exact CWS zlib stream, frame header, short/long tags, recursive
      `DefineSprite` walk, counts and bounded ExportAssets/ImportAssets/
      DefineFontName names; real movie reports 1,186 tags and 52 names. No
      display list, AVM or draw. See `docs/MENU_SWF.md`.
- [x] Bounded SWF display-state snapshots and DefineFont3 metrics:
      PlaceObject2/3, RemoveObject2, per-ShowFrame depth-ordered objects,
      71 sprite summaries, one selected sprite timeline, and the 243-glyph
      code/advance/bounds/kerning tables; malformed/AVM2/DefineFont4 paths refuse
      by name. Still no renderable display list, AVM execution or draw.
      See `docs/MENU_SWF.md`.
- [x] Bounded SWF shape/glyph geometry: DefineShape/2/3/4 fill/line styles,
      StyleChange/StraightEdge/CurvedEdge records, 27 shapes / 151 paths /
      1,712 segments / 2,362 points, selected-sprite shape bounds, and 243
      DefineFont3 glyph outlines (241 non-empty, 5,292 segments). No
      tessellation, rasterisation or atlas. See `docs/MENU_SWF.md`.
- [x] Bounded SWF path tessellation: even-odd and non-zero fill rules, adaptive
      quadratic-curve flattening, style-tagged triangles plus separate stroke
      segments, caller-owned buffers with named overflow refusals. Real
      StartMenu: 27/27 shapes, 71,605 vertices, 35,759 triangles, 11 lines,
      3,118 curve segments. CPU geometry only; no raster, atlas or draw.
      See `docs/MENU_SWF.md`.
- [ ] Skyrim main menu.

## M5 — Real D3D9 → PICA200 renderer

- [~] COM ABI, refcounts, deferred release, device/resource lifecycle.
- [~] Vertex buffers/streams/declarations/FVF, INDEX16/32, strips/fans on CPU.
      `DrawPrimitiveUP`/`DrawIndexedPrimitiveUP` are implemented for the same
      FVF/declaration subset without changing bound state; COM slots 83/84 and
      host/ASan tests pass. `DRAWPRIMITIVES2EX` and HW T&L remain unclaimed.
- [~] BGRA + BC1/2/3 textures, mips, partial rect locks, sampler/render states.
      Bounded DDS/BC → ETC1/ETC1A4 conversion and SKTX container are implemented;
      18,580-DDS projection is 1,544 MiB alpha-dropped / 2,090 MiB mixed auto.
      Guest/GPU ownership, LRU, pinning and deterministic full-state accounting
      pass 91 residency checks; GPU-only ETC1/SKTX registration and rehydrate
      tokens add 286 checks. Pool-aware core/COM wiring and direct SKTX upload
      add 108 runtime checks; MANAGED eviction/rehydrate, Reset preservation,
      DISCARD and automatic guest drop remain pending. TESV's D3DX9_42 texture
      entry points now bind to host thunks: in-memory and sandboxed file-path
      DDS image-info/create (`D3DXCreateTextureFromFileExA` accepts the measured
      TESV tuple, including DXT→A8) and surface load are implemented;
      `D3DXCompileShader` refuses by name, while cube/volume creation remain
      named `E_NOTIMPL`.
- [~] A specialized ps_3_0 → PICA200 TEV translator for the measured LDR
      subset (`nop/mov/add/sub/mad/mul/dp3/lrp/texld/dcl/def`, 6 stages, 3 texture
      units); 19 real PS chunks translate after rejecting non-LDR literals,
      including all shipped simple copies.
      Guest `CreatePixelShader` integration is tested; every other opcode/feature
      is rejected by name. A structural menu-light matcher plus five-stage TEV
      prototype covers 6 additional retail logo signatures; automatic selection
      still requires the material/normal/tangent preconditions. The bounded
      first-menu runtime selector now accepts only the exact `p1200005` SHA-256
      and the 184-byte menu copy (plus explicit fixed fog/tint contracts),
      refuses everything else by name, and keeps VS/PS 2.0 with zero SM3 slots.
      `vs_3_0`, the material-precondition source and two PICA texture units are
      the next layers. The bounded
      first-menu runtime selector is now connected: exact-hash
      `menu.logo.ps`/`menu.composite.ps` plans, a bounded structural
      `menu.generic.vs` family gated by declaration/FVF, fixed fog/tint
      contract plans, and observable material bits, with named
      precondition/backend draw refusals (`docs/MENU_SHADER_RUNTIME.md`).
- [x] Offline menu preprocessing tools validate both normal-decode signatures,
      tangent/material constants and Scaleform glyph quads; real `Logo02_n.dds`
      normalization passes. Runtime loader integration remains pending.
- [~] Library of specialized graphics pipelines (interface/text, sky,
      landscape, static geometry, characters, vegetation, water, particles).
      Data-driven registry and structural matcher cover 17 interface/text and 6
      static-geometry PS chunks; the other families are explicitly planned with
      quality, pass, memory and fallback contracts. COM/GPU integration is pending.
- [~] Inventory of real Skyrim shader hashes → PICA programs or specialized paths.
      **Measured (2026-11-24)**: 10,475 compiled chunks, all vs_3_0/ps_3_0, no
      SM2; 6,659 distinct bytecode blobs / 6,655 instruction streams across 122
      package+technique keys. The main-menu subset is ranked (copy/blit,
      840–852-byte base-lit logo, fog/tint); universal translation is neither
      required nor feasible for the first frame.
      The engine loads shader packages by name pattern
      (`shadersfx\%s\%03d\V0%s.fxp` / `P0%s.fxp`, `shadersfx\Shaders%03d.fxp`,
      `shadersfx\Imagespace%03d.fxp`), so a pre-translation cache can be keyed
      by exactly `(name, index, stage)` without touching game code.
- [~] One explicit LDR render-target surface is implemented across core/COM/PICA:
  VRAM colour target with its own depth attachment, clear/bind/restore, read-only
  LockRect and bounded lifetime. Azahar switching pixel proof passes.
  `GetViewport` is corrected to slot 48. `D3DUSAGE_RENDERTARGET` textures
  (pow2 8..1024 X8R8G8B8/A8R8G8B8, one level, pool DEFAULT) are real
  target-backed textures: `GetSurfaceLevel(0)` returns the render target and
  `SetTexture` samples the same colour buffer. Depth-stencil descriptors now
  cover pow2 8..1024 (plus the exact 400x240 back-buffer size), are bound only
  to a target of the same size, and a target switch under an incompatible bound
  depth is a named refusal; depth/stencil-only clears no longer clear colour.
  Float/HDR render targets, mip chains, viewport orientation, MSAA depth,
  depth textures, separate depth VRAM, scissor, state blocks, fog, multi-stage
  samplers, skinning, terrain and particles remain unsupported.
- [ ] Golden-image tests with tolerances; check emulator vs hardware differences.

**Gate:** real Skyrim main menu and then world geometry drawn by TESV, not a
demo cube. *Not met.*

### Path to the first frame (dependency order, measured)

| # | Barrier | State |
|---|---|---|
| 1 | Guest reaches WinMain | **reached**: WinMain `0x69d1d0` is executing. It opens `HKLM\Software\Bethesda Softworks\Skyrim` at `0x0069d625`, which faults on the unimplemented `ADVAPI32.dll!RegOpenKeyExA`. A bounded in-memory registry shim is in progress |
| 2 | `VirtualAlloc` reserve/commit separation | **done**: legal re-commit, 4 KiB commit rounding, lazy non-contiguous backing; 95 MiB commit promise costs 68–136 KiB at this stage |
| 3 | `InitPaths` + INI | sandbox INIs prepared (`Documents\My Games\Skyrim\SkyrimPrefs.ini` with 400x240, `LocalAppData\Skyrim\Skyrim.ini`) |
| 4 | Renderer gate `0xc6dbf0` | **reached in TESV**: enumeration completes, LDR RT surface formats `0x15/0x16` now pass, then `0x00c6d8c6` rejects HAL because `DevCaps & 0x00010000` (`HWTRANSFORMANDLIGHT`) is clear. No capability bit is faked. A measurement-only probe (`TESV_D3D9_CAPS_PROBE=1`, not shippable) shows what follows the gate: `CreateDevice` is called with `400x240 X8R8G8B8`, `DISCARD`, no MSAA, windowed, interval ONE and `Flags=0x1` (`LOCKABLE_BACKBUFFER`). That flag and the explicit depth-stencil surface are now real features, so with the probe the device can be created; the remaining question is the engine's post-device use of fixed-function lighting (`docs/HWTL_DECISION.md`, `docs/HWTL_IMPLEMENTATION.md`). |
| 5 | Shader model check | honest caps still report SM2; after T&L, TESV's later `0xc6e22d` check requires SM3. Generic TEV covers 19 real PS chunks; the menu-light prototype covers 6 logo signatures. The bounded menu shader closure (`p1200005` + menu copy) now has a runtime contract that never widens the global caps. |
| 6 | Device creation + first frame | **implemented and host-tested**: the measured TESV `CreateDevice` set is accepted, including `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` (retained 512x256 target, real Present blit and LockRect read/write) and explicit `D24S8/D24X8` depth-stencil surfaces with hardware clear/attach. Host evidence is pixel-complete through the null backend (`tests/back_buffer_lockable_smoke.c`); PICA/Azahar pixel proof is the next step (`docs/BACK_BUFFER_LOCKABLE.md`). |


Measured post-renderer state (2026-11-26): with the measurement-only SM3 probe the gate
passes and renderer initialisation continues. The first substantial call is
`CreateTexture(D3DUSAGE_RENDERTARGET)`; LDR targets are now implemented (pow2 8..1024
X8R8G8B8/A8R8G8B8, one level, pool DEFAULT) as real target-backed textures, while
float/HDR (`0x71/0x72`) stay named refusals. The next census records the first
depth-stencil/shader/draw call after this layer. No frame is claimed.
Steps 1–4 are engineering. Step 5 is the architectural question: PICA200 has no
programmable fragment shader, so SM3 pixel shaders can only run through a
specialised translation of the exact shader set Skyrim uses, or not at all.


## M6 — Input, audio, window, UI, saves

- [~] Win32 window/message/input mapping; Circle Pad/C-Stick/buttons; touch
      shortcuts on the lower screen; dead zones and focus loss.
- [~] Audio backend: real `waveOut*` shim wired to OpenDSP with ndsp fallback in
      the TESV 3DS runner, stereo X3DAudio subset, timestamps/resampling/underrun
      accounting. The remaining gate is reaching real game audio calls.
- [ ] Bink/video and middleware policy, callbacks preserved.
- [ ] Atomic saves in a dedicated SD directory, reload verification, clean exit.

**Gate:** playable input/menu, verified save→reload, audio stated or implemented.
*Not met.*

## M7 — World, simulation, performance, stability

- [ ] Physics/AI/Papyrus/animation/collision/quests/loading on real scenarios.
- [ ] Frame/update/physics/AI/render/audio/IO instrumentation; heap and stack
      high-water marks.
- [ ] 400×240 target, mono rendering, LOD/view-distance/shadow reduction as
      configuration, not semantic breakage.
- [ ] 30-minute smoke route and device validation.

**Gate:** vertical gameplay slice works in Azahar and on a real New 3DS with
published measured limits. *Not met.*

## M8 — Release reproducibility and documentation

- [~] Clean-build scripts, artifacts with hashes/manifests, host test runner.
- [ ] Second-machine clean build, supported game hashes, FAQ, log collection.
- [ ] License/dependency review; no proprietary binaries or generated game code
      distributed. `.3dsx` produced locally by the owner only.

**Gate:** reproducible release candidate on hardware with an accurate support
statement. *Not met.*
