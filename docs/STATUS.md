# STATUS — Skyrim_N3DS

Updated: 2026-11-24
Version: v0.12 (working tree; no release tag)

## 1. Environment actually used

| Component | Value |
|---|---|
| OS | Windows, PowerShell 5.1 + devkitPro MSYS2 |
| devkitARM | 15.2.0 (`arm-none-eabi-gcc.exe (devkitARM) 15.2.0`) |
| libctru / Citro3D | installed via devkitPro 3DSDev (`/opt/devkitpro/libctru`); installer records no version string |
| Azahar | 2126.2 (`C:\Program Files\Azahar\azahar.exe`) |
| Azahar profile | `System_IsNew3ds=true`, virtual SD enabled, GDB stub off, LLE applets on |
| Python (tools) | 3.13.3 venv `.venv`, capstone 5.0.9, pefile 2024.8.26 |
| Host C compiler | clang 23.1.5 + MSVC 14.51 (x86_64 host tests) |
| Skyrim 2011 | `E:\SteamLibrary\steamapps\common\Skyrim` (TESV.exe 18,024,240 B, SHA256 `69362f77…efc72`, Steam-protected) |
| Unprotected TESV | `E:\skyr\TESV.exe`, 17,462,064 B, SHA256 `d97eb0cc…f5e605`, SHA-1 `17ec1ef6…9182`, entry `0xf568a1` in `.text` |
| Same-build proof | All sections except `.text`/`.bind` byte-identical to the Steam copy (`tools/compare_pe.py`) |
| Skyrim_Android | `C:\Users\vasya\Documents\Skyrim_Android`, commit `0255cfa86d5f1973d3dc6b2cb599f864b9237b22` |
| 3DS hardware | Physical `hwhybridprobe` PASS: 178 MiB, `0x00403780`, IR executed. Latest `hwbulk` CIA (968,640 B, SHA256 `1d88392e…fd6a`) matches host `0x0064be80` in Azahar; physical retest pending. |
| System disk | C: ≈138 GB free; E: ≈55 GB free at the latest check. Host intermediates use `E:\Skyrim_N3DS_build`; isolated ARM builds use distinct targets. |
| Application memory | `.3dsx`: `MEMREGION_APPLICATION` = 96 MiB observed; CIA with `SystemModeExt: 178MB` (libctru `SYSMODE_DEV1`, 178 MiB on N3DS) — runtime confirmation pending |
| Intermediate storage | `F:` is the console SD card. It now contains only the final test inputs/instructions under `F:\Skyrim` and final CIAs under `F:\cias`; discovery/build intermediates remain on `E:`/`C:`. Never copy host run directories, generated C, object files or logs to the card. |

## 2. Build artifacts (2026-11-24)

| Target | `.text` | `.data` | `.bss` | `.3dsx` |
|---|---:|---:|---:|---:|
| Skyrim_N3DS (diagnostic) | 137,880 | 7,176 | 14,900 | see `dist/` |
| Skyrim_N3DS_lift | 172,984 | 7,176 | 26,188 | see `dist/` |
| Skyrim_N3DS_boot | 174,612 | 7,176 | 30,044 | see `dist/` |
| Skyrim_N3DS_pilot (Scaling, current `dist/`) | 160,076 | 7,176 | 25,404 | 168,876 B |
| Skyrim_N3DS_pilot (Multiply, measured) | 170,972 | 7,176 | 25,404 | 179,776 B |
| Skyrim_N3DS_tesv (70-function slice) | 366,804 | 7,304 | 113,228 | 377,016 B |
| Skyrim_N3DS_tesv.cia (local, 178 MB exheader) | — | — | — | deep slice 25 360 funcs, 29 075 904 B (sha256 `6c5153ad…e89e`) |
| Skyrim_N3DS_tesv 9k `.3dsx` (all modules) | 26 226 952 | 7 312 | 979 580 | 26 367 848 B, Azahar parity |
| Skyrim_N3DS_tesv deep `.3dsx` (new codegen) | 73 148 448 | 7 312 | 1 101 252 | 73 577 220 B (sha256 `23b97391…0e53`), host only |
| Skyrim_N3DS_tesv_irprobe (hybrid diagnostic) | 1 439 400 | 7 328 | 941 844 | 1 455 092 B; CIA 826,304 B on `F:` |
| Skyrim_N3DS_tesv_hwtest (deep AOT, lazy VA) | 72 594 152 | 7 328 | 1 155 868 | 72 929 016 B; CIA 28,678,592 B on `F:` |
| Skyrim_N3DS_tesv_hwhybridprobe (growth13 hybrid) | 1 468 632 | 7 344 | 946 220 | 1,484,704 B; CIA 845,248 B; physical PASS `0x00403780` |
| Skyrim_N3DS_tesv_hwnext2 (growth19 hybrid) | 1 549 600 | 7 344 | 946 236 | 1,565,680 B; CIA 905,664 B on `F:`; Azahar `0x00ff1dd0`, IR 14,350 blocks |
| Skyrim_N3DS_tesv_hwbulk (growth24 hybrid) | 1 639 000 | 7 344 | 946 236 | 1,655,080 B; CIA 968,640 B on `F:`; Azahar `0x0064be80`, IR 32,199 blocks |
| Skyrim_N3DS_graphics | 136,544 | 7,192 | 19,268 | see `dist/` |
| Skyrim_N3DS_graphics_com | 232,372 | 7,192 | 43,772 | see `dist/` |

`tools/build.ps1` archives `.elf`, `.3dsx`, `build.log` and
`build_manifest.json` (SHA256, toolchain, exact make command) under
`dist/<target>/`. It cleans by default; `-NoClean` resumes an existing isolated
object directory after a long generated-code compile times out.

## 3. Result matrix (2026-11-24)

Legend: HOST PASS = native x86_64 build/test; ARM BUILD PASS = devkitARM link;
AZAHAR PASS = launched in desktop Azahar and observed.

| Check | HOST | ARM | AZAHAR | Evidence |
|---|---|---|---|---|
| PE mapping / reloc / heap / call ABI (`host`) | PASS | n/a | n/a | `E:\Skyrim_N3DS_build\host\logs` |
| Lift ABI + Win32 shims (`lift`) | PASS | PASS | PASS (`Lift ABI: PASS`) | `build_azahar/*Skyrim_N3DS_lift` |
| PEB/TEB/TLS/IAT/DLL attach/entry (`boot`) | PASS | PASS | PASS | `build_azahar/20261124-012731-Skyrim_N3DS_boot_v012` |
| Graphics core D3D9→PICA | PASS | PASS | PASS, 59.8 FPS cube | `build_azahar/20261124-012753-Skyrim_N3DS_graphics_v012/screen.png` |
| D3D9 COM + BC + SM2 + declarations | PASS | PASS | PASS, all on-screen checks PASS | `build_azahar/20261124-012818-Skyrim_N3DS_graphics_com_v012/screen.png` |
| Host tests under ASan | PASS (7 C tests) | n/a | n/a | `E:\Skyrim_N3DS_build\host\logs` |
| Real D3DX pilot (Multiply) | PASS (3/3) | PASS | PASS (3/3) | `build_azahar/20261124-011746-Skyrim_N3DS_pilot_v012` |
| Real D3DX pilot (Transpose) | PASS (3/3) | PASS | not re-run | `build_host` |
| Real D3DX pilot (Scaling) | PASS (3/3) | PASS | PASS (3/3) | `build_azahar/20261124-012459-Skyrim_N3DS_pilot_scaling` |
| TESV PE map + IAT walk (`tesv_probe`) | PASS: 266 imports; diagnostic traps reduced 186 → 154 after loaded-export resolution fix | n/a | n/a | `E:\Skyrim_N3DS_build\host\logs`, `tests/pe_imports_smoke.c` |
| Win32 module smokes (sync/user/locale/shell) | PASS | n/a | n/a | `E:\Skyrim_N3DS_build\host\logs` |
| TESV startup slice (70 funcs, diagnostic) | HOST DIAG: fault at `0x00f6916b` | PASS | AZAHAR DIAG: same fault | `build_azahar/20261124-022926-Skyrim_N3DS_tesv_v012c` |
| TESV startup slice (21 400 funcs, diagnostic) | HOST DIAG: fault at `0x0103b5a2` after 8 growth iterations | build in progress | not re-run (heaps re-pinned to 34+8 MiB) | `E:\Skyrim_N3DS_build\analysis\growth8\growth_journal.json` |
| ARM `.text` measurement (9 000 funcs, `-O2`) | 26 111 200 B / 924 440 insns = 28.2 B/insn | PASS | n/a | `dist/Skyrim_N3DS_tesv/build_manifest.json` |
| TESV 9 000-func `.3dsx` in Azahar (24.9 MiB text, heaps 34+8) | n/a | 26 112 064 B text | **RAN**: `app_region=96 MiB`, guest used 31.4 MiB, then `ExitProcess(255)` | `build_azahar/20261124-051852-tesv_9k_heaps2/screen.png` |
| TESV 9 000-func all-modules Azahar parity | n/a | 26 226 952 B text | **DIAG**: same `0x00f53e0f` as host | `build_azahar/20261124-073043-tesv_9k_allmodules` |
| TESV 9k lazy-VA + audio ARM/Azahar | n/a | 26 510 768 B text; `.3dsx` 26 652 192 B | **DIAG**: same `0x00f53e0f`; `app_region=96 MiB`, guest 30.6 MiB. Audio fallback/state fixes pass ARM regression; one external Azahar/libctru first-init `Component not loaded!` warning remains | `build_azahar/20261124-161811-tesv_9k_lazyva_audio` |
| Lazy VA + file-page provider | PASS: reserve/commit/recommit/query plus first-touch/dirty/eviction/reload/I/O-failure accounting | PASS (9k isolated) | PASS: 9k parity with lazy VA | `tests/win32_smoke.c`, `build_azahar/20261124-160700-tesv_9k_lazyva_audio` |
| ARM11 time-profile methodology | PASS: 15-function AOT/IR state verification, 40,448 hits, archive hash checks | PASS: isolated Azahar benchmark | PASS: synthetic emulator timing only | `docs/ARM_PROFILING.md`, `tests/isolated/armtime/results/20261124-172408-armtime` |
| AOT ↔ IR equivalence and v2 coverage | PASS: x87 + measured SSE/SSE2; 25,350/25,360 functions decode; partition/CMC/time-ranking tests pass. Full hybrid host probe reaches `0x00b2a970` with 68 KiB VA backed | PASS: full hybrid link, 32 AOT + 25,328 IR; `.text` 1,438,312 B, `.3dsx` 1,454,000 B; probe build `.text` 1,439,400 B | **PASS (guest diagnostic)**: probe run reaches `0x00b2a970`; IR cache 859 blocks / 94,584 B / 18,468 hits. Harness overall FAIL only because a concurrent armtime log shared `Skyrim_N3DS.log` | `tests/ir_*`, `dist/Skyrim_N3DS_tesv_irprobe`, `build_azahar/20261124-171506-irprobe` |
| D3D9 null device/surface path | PASS: CreateDevice/Reset/back buffer/RT/viewport/Clear state tests; opt-in COM trace covers enumeration → Present | PASS (`com` isolated ARM) | n/a | `TESV_D3D9_TRACE=1`, `tests/null_backend_smoke.c` |
| TEV-aware PICA backend stage | PASS: 134 host checks + ASan/UBSan; named rejection for unsupported TEV features; MIPFILTER_NONE and vertex-colour contract fixes | PASS: `-Werror` compile/link | PASS: 66-check TEV + 15-check vertex/citro3d state probes, no draw/pixel claim | `docs/PICA_BACKEND.md`, `build_azahar/20261124-194543-vertex_citro3d_arm_final` |
| TEV-aware PICA runtime integration | PASS: full host suite 36 + 1 expected TESV diagnostic; COM pointer/lifetime/rejection tests | PASS: graphics/COM/TESV `picaprobe` builds | DIAG: TESV probe `0x00403780`; synthetic `ui.copy` pixel proof PASS separately | `dist/Skyrim_N3DS_tesv_picaprobe`, `tests/isolated/uicopy`, `docs/PICA_BACKEND.md` |
| Renderer requirement probes | PASS: LDR paths accepted; HDR/RESZ/depth unsupported; sRGB/ATOC/SM3 explicit fallbacks; `-Werror` | n/a | n/a | `docs/RENDERER_REQUIREMENTS.md`, `tests/renderer_requirements_smoke.c` |
| End-to-end `ui.copy` PICA draw | PASS: 52 mock checks + ASan/UBSan; existing `tev_com` PASS | PASS: isolated ARM build | **PASS (synthetic pixel proof)**: 128×128 readback, 64 texels × 256 pixels, FNV `e34b4dc5` | `tests/isolated/uicopy/`, `build_azahar/20261124-203413-uicopy_arm_final` |
| First-draw capture module | PASS: host + ASan/UBSan + ARM `-Werror`; wired into COM draw hooks with bounded snapshot and no-overwrite files | PASS: compile/link in pending integration | n/a | `docs/FIRST_DRAW_CAPTURE.md`, `tests/d3d9_capture_smoke.c` |
| Specialized pipeline registry | PASS: 124 C structural/plan checks + 19 Python tests; 17 interface/text and 6 static-geometry PS chunks structurally selected | n/a | n/a | `docs/PIPELINES.md`, `tools/pipelines/coverage_base.json` |
| Menu shader preprocessing | PASS: 29 synthetic tests; real `Logo02_n.dds` normalized for both retail signatures, 2,796,332 B output, ~4.96 MiB tool peak | n/a | n/a | `docs/MENU_PREPROCESS.md`, `tools/menu_preprocess/` |
| Specialized SM3 → TEV translator | PASS: 237 checks, 28,000 vectors, 19 real PS chunks accepted after literal-range hardening; guest CreatePixelShader blit/tint integration PASS; 6 menu-light signatures have a separate 5-stage prototype | n/a | n/a | `tests/tev_*`, `tests/menu_light_smoke.c`, `docs/SHADERS.md` |
| ETC1 conversion + texture residency | PASS: 18,580-DDS projection, bounded conversion, SKTX parser, 91 residency + 286 GPU-only upload + 108 runtime checks, 24 container checks | PASS: isolated ARM/Azahar 11 checks; graphics/COM/TESV `texres*` builds | PASS: `texetc1`, no pixel readback; direct SKTX backend compiles on ARM | `docs/TEXTURES.md`, `docs/TEXTURE_RESIDENCY_INTEGRATION.md`, `dist/Skyrim_N3DS_graphics_texresgfx`, `dist/Skyrim_N3DS_tesv_texres` |
| TESV startup after lazy VA | **WinMain `0x69d1d0` executing**. It opens `HKLM\Software\Bethesda Softworks\Skyrim` at `0x0069d625` and faults on `ADVAPI32.dll!RegOpenKeyExA`; registry shim in progress | PASS: deep `hwtest`, `hwhybridprobe`, `hwnext2`, `hwbulk` artifacts; container ARM link pending | **PASS**: physical hybrid at `0x00403780`; `hwbulk` Azahar reaches `0x0064be80` | registry shim, then re-run host probe |
| New 3DS hardware | — | — | **PASS (diagnostic)** | `hwhybridprobe`: 178 MiB region, guest 35,653,056 B, `0x00403780`, IR 956 blocks / 23,193 hits, no `svcBreak`; `build_hwtest/physical_20261124/` |

The full host suite (`tools/run_host_tests.ps1`) covers: host, lift, boot,
graphics, vertex, bc_shader, com, adapter/device/null backend, sync, user, locale,
shell, pilot_real, tesv_probe, PE imports, IR equivalence/partition,
TEV/menu-light, audio, VA mapping, ETC1/residency/runtime/container tests, plus
Python resolver/slice/shim-base checks. A documented guest fault in `tesv_slice` is
reported as HOST DIAG, not PASS.

Latest texture/state integration run (2026-11-24 20:0x): **38 passed, 1 expected
TESV diagnostic, 0 failed**. This includes pool-aware texture residency,
SYSTEMMEM CPU-only behavior, direct SKTX uploads, exact split accounting,
release retry, D3D9 defaults, missing-UV rejection, c0..c3 constant bounds, and
explicit viewport/Clear rejections. Focused graphics/COM/texture tests also
pass under ASan/UBSan. The TESV diagnostic remains the guest
`ExitProcess(0)` / “Not enough memory to run application.” frontier, not a test
failure.

### Hardware incident (2026-11-24)

The first CIA run crashed before `main`. Luma3DS dump
`build_crash/crash_dump_00000070.dmp` decodes as:

| Field | Value |
|---|---|
| process / title | `Skyrim N3DS`, `000400000F0D5900` (our CIA; matches the documented ID) |
| type | 2 — prefetch abort (Luma special-cases `svcBreak` panic) |
| PC | `0x00132df4` → `svcBreak` |
| LR | `0x00134b8c` → `__system_allocateHeaps` (`allocateHeaps.c:66`) |
| SP / CPSR | `0x0fffffc0` / `0xa0000010` (User/ARM) |

Cause: in the 178 MiB `SystemModeExt` mode, libctru's default heap split asks
`svcControlMemory` for an application heap larger than the heap area and calls
`svcBreak` before `main`. Fix: `source/tesv_slice_main.c` now pins
`__ctru_heap_size = 56 MiB` and `__ctru_linear_heap_size = 16 MiB`, which fits
both the 96 MiB `.3dsx` region and the 178 MiB CIA mode. In Azahar `.3dsx`
after the fix: `app_region=96 MiB`, heaps committed, guest used 28.1 MiB and
the slice faulted at `0x00f6916b` instead of panicking. The current pins are
64 MiB app + 8 MiB linear with a 60 MiB guest budget; physical validation is
recorded below.

### Hardware loader incident `crash_dump_00000071.dmp`

The user reports `irprobe` ran normally, while the compressed deep `hwtest` CIA
crashed at startup. Dump SHA256:
`90ea8c1cc25307f88827fbd37fd2919a44493558b6e733d1f96380440d95b335`.

| Field | Value |
|---|---|
| process / title | `loader` / `0000000000000000` |
| type | 1 — undefined instruction |
| PC / LR | `0x14001900` / `0x14001e94` |
| SP / CPSR | `0x0ffffe08` / `0xa0000010` (User/ARM) |
| R0 | `0xE0E01BF5` |

This is before the TESV process starts, not the earlier libctru heap panic. The
code around the trap is an LZSS-like byte-copy loop ending in `udf #0`, so a
compressed-ExeFS loader/decompressor failure was the leading hypothesis. The
no-compression control produced `crash_dump_00000072.dmp` with identical PC/LR/R0,
so **compression is rejected as the cause**. The remaining suspects are large
ExeFS/code mapping/install limits or CIA layout; neither is proven. The small
`hwhybridprobe` CIA (845,248 B, SHA256 `f108f0e7…e058`) is the recommended next
physical test and already reaches `0x00403780` in Azahar.

Physical result: `hwhybridprobe` **passed** on New 3DS. The SD log reports
`app_region=178 MiB`, `guest_used=35,653,056`, fault `0x00403780`, 22 executed
functions / 673 calls, and IR cache `blocks=956 bytes=107568 hits=23193
misses=956 evicted=0`. `tools/check_hw_log.py --expected-fault 00403780
--require-ir` passes every check. Evidence is archived in
`build_hwtest/physical_20261124/`; log SHA256 begins `0939c262…`, profile SHA256
begins `7b41b832…`. This confirms the hybrid hardware path and strongly implicates
large-code loading for the AOT loader failures, though the exact loader limit is
not yet isolated.

## 4. Real D3DX pilot

Input DLL: `C:\Windows\SysWOW64\D3DX9_42.dll` (32-bit), 1,892,184 B,
SHA256 `f8b9cfab7fffbc8f98e41aa439d72921dc180634a1febca2a9d41a0df35d3472`,
FNV64 `ec45606fd95aaad9`.

The export is **not** a plain function: it is an MSVC lazy incremental-link
thunk (`FF 25 <slot>` + 5×`CC`), whose slot initially points to a guard wrapper
that calls a one-time initializer and then forwards through the patched slot.
The pilot generator now verifies this exact layout, resolves the real body
(`export+0xB`), and records `lazy_ilt_init_bypassed=true` in
`pilot_manifest.json`. The 3DS and host logs state explicitly that the wrapper
and initializer are not executed. The initializer is real DLL code that reaches
CRT/import initialization and is intentionally out of scope for an isolated
math call; the resolved bodies are self-contained (single function, no calls).

| Export | x86 insns | Host | ARM | Azahar |
|---|---:|---|---|---|
| D3DXMatrixTranspose | 53 | PASS 3/3 | PASS (linked) | not run |
| D3DXMatrixMultiply | 217 | PASS 3/3 | PASS | PASS 3/3 (`20261124-011746-…pilot_v012`) |
| D3DXMatrixScaling | 27 | PASS 3/3 | PASS | PASS 3/3 (`20261124-012459-…pilot_scaling`) |
| D3DXMatrixTranslation | 27 | PASS 3/3 | not built | not run |

Multiply exercises the `jmp [slot]` wrapper; Scaling exercises the
`call [slot]; ret 0x10` wrapper with a returned matrix pointer. Both thunk
forms are covered by unit tests and real DLL runs.

Guest memory used by the pilot: 2,480 KiB of the 48 MiB cap.

## 5. TESV static analysis and game audit

Full upstream discovery on the unprotected executable completed:

| Metric | Value |
|---|---|
| Functions discovered | 90,900 |
| Text bytes covered | 12,330,083 of 13,013,786 (94.7%) |
| Gaps | 4,029 sites / 46,802 bytes |
| Jump tables | 1,083 |
| Unresolved indirect jumps | 1,201 |
| Raw result | `E:\Skyrim_N3DS_build\analysis\tesv\functions.json` (18.7 MB) |

Direct-call closure from the real entry (`tools/slice_pe.py`):

| Metric | Value |
|---|---|
| Reachable functions (direct calls) | 13,390 of 90,900 |
| x86 bytes / instructions | 2,904,872 B / 955,134 |
| Distinct imports used | 159 |
| Dynamic indirect sites encountered | 12,472 |
| Projected ARM `.text` at 56.9 B/insn | ≈ 51.8 MiB |

Real PE/IAT probe (`tests/tesv_probe.c`) maps the executable and resolves
imports with the current shims: 266 imports, 187 unresolved, and the first
missing imports in IAT order are `CompareFileTime`, `FindFirstFileA`,
`lstrcpyA`, `lstrcatA`, `FileTimeToSystemTime`, `FileTimeToLocalFileTime`,
`LocalFree`, `FormatMessageA`, `CreateDirectoryA`, `CreateSemaphoreA`,
`DebugBreak`.

The first bounded lifted slice (entry + both TLS callbacks, 70 functions) now
executes real TESV guest code on host and in Azahar. Observed first-fault chain
after each fix (the runner never fakes an import success; unknown imports are
addresses that trap with their name):

1. `KERNEL32.dll!HeapCreate` — implemented as a real second guest heap.
2. `0x00f76345` unlifted (indirect call target) — slice grown.
3. `0x00f5d5c3` unlifted (indirect call target) — slice grown.
4. `KERNEL32.dll!EncodePointer` — implemented with a process XOR key; decode
   round-trips.
5. Current: `0x00f6916b` unlifted; guest footprint 28.1 MiB, 200,000-block
   budget was not exhausted.

This is the exact loop to continue: add the named import or the indirect target,
rebuild, re-run, record the next address. It is not evidence that the game boots.

Static follow-up of the current fault (read-only analysis):

- `0x00f6916b` is a **direct** `call` from `0x00f58ba4` inside function
  `0x00f58b99` — not an indirect dispatch. The discovery map has a hole there,
  so the slice tool must locally decode direct targets missing from
  `functions.json` instead of faulting one address at a time.
- The same function calls `0xf6b766`, `0xf548a5`, `0xf5dc6d`, `0xf6b757`,
  `0xf67893`, `0xf68a5f`, `0xf6b746`, `0xf58b65`, and `0xf5d551`.
- MSVC `_initterm` tables used by startup (`0x1237684`, `0x123768c`) are empty;
  the current faults are normal init code, not C++ static constructors.
- The SafeSEH table (load config +0x40, VA `0x12375a0`, 56 entries) lists six
  real handlers plus 50 EH funclets; none are on the normal path yet.
- TLS callback array at `0x1076ac4` is `[0, 0, 0xf794ab, 0xf5b303, 0, 0]`, in
  both the protected and unprotected copies. Relocations cover only the two
  non-NULL slots. Windows' loader (ntdll/Wine/ReactOS) stops at the first NULL,
  so **zero TLS callbacks run**; this project's loader already matches that and
  must not "fix" it by skipping NULLs. Optional future guard: log a warning
  when leading NULLs hide later pointers, without executing them.

`tools/slice_pe.py` now locally decodes direct targets absent from the
discovery manifest (recursive descent, padding stop, bounded jump-table
heuristic), supports `--extra-root-file`, and has 12 synthetic assertions in
`tests/slice_pe_test.py`. With a 120-function budget it decoded 66 missing
functions and `staged/lift.py` lifted all 120 with zero rejections. The
fault-driven growth loop is running with larger budgets and records every step
in `growth_journal.json`.

`skyrim_audit_unpacked_v3.json` (static regular imports only):

| Metric | Value |
|---|---|
| PE images scanned | TESV.exe 26.14 MB image (13.58 MB executable), binkw32.dll, steam_api.dll |
| Unique unresolved imports | 205 (v6, after FLS/COM/user32 basics) |
| kernel32.dll unresolved | 75 unique |
| Already mapped to shims | 189 import entries |
| Local exports not yet lifted | 14 |
| Not found next to game | `d3dx9_42.dll`, `X3DAudio1_7.dll` (system copy exists for D3DX) |

`skyrim_audit_unpacked_v6.json` supersedes v5/v4/v3/v2/v1; `references` counts importing
modules (max 3), not call sites.

## 6. First remaining blockers (priority order)

1. **Code volume / memory (startup fits; full hybrid link and ARM probe proven).**
   The pure-AOT deep slice is 69.8 MiB of ARM text. Lazy VA reduced the guest
   footprint from 115.6 MiB to ~32 MiB plus touched pages, so startup totals
   roughly **111 MiB and fits 178 MiB**. The old `0x14` allocator fault is fixed.
   IR v2 now decodes **25,350/25,360** real functions, including x87 and the
   measured SSE/SSE2 set. Only 10 functions (0.24 MiB projected AOT) remain
   forced out by block-terminator edges. The real-profile 20% AOT / 80% IR
   projection is **32.3 MiB**; the ARM interpreter is 55,338 B. The full hybrid
   slice is now linked with **32 AOT + 25,328 IR**: actual `.text` is 1,438,312 B
   and `.3dsx` is 1,454,000 B because IR is decoded on demand into a bounded
   cache, not stored as ARM code. Host and ARM probe runs both reach
   `0x00b2a970`; the ARM IR cache records **859 blocks / 94,584 B / 18,468 hits**,
   proving deep hybrid execution. The harness's overall FAIL was a concurrent
   `Skyrim_N3DS.log` collision with the armtime benchmark, not a guest failure.
   Azahar measures IR warm at 5.24× AOT, but that is not hardware throughput.
   MMX and newer SSE subsets still fail explicitly (`docs/IR.md`).
2. **TESV startup has passed the allocator initialisation.** The lifted CRT,
   the initializer table at `0x1076aa0`, the engine's memory manager and its
   legal re-commit path now run. The 512 MiB minimum-config gate is
   `GlobalMemoryStatusEx` at `0x00a48a67` (IAT `0x0106b274`); it is satisfied
   today only by the diagnostic `TESV_MEMPROBE=1` value. Measured with the
   probe: a 42.119 MiB pool is the minimum, and a 178 MiB region behaves
   identically to 160 MiB. Lazy VA measured: **581.25 MiB reserved, 100.25 MiB
   commit promise, 4.63 MiB physically backed**, 0 commit failures. Reporting
   the 512 MiB honestly needs dirty-anonymous-page eviction (SD swap) or a
   documented native patch; `page_flush()` currently handles only file pages.
   See `docs/MEMORY_BUDGET.md`. Adding the `0xb2a970` vtable roots advanced startup. Growth14 reached
   `0x00403270`; growth15 reached `0x00fc4920`, `0x00fc4990` and, after
   raising the diagnostic block budget, `0x00fc8ef0`. Growth16 then advanced
   `0x00fcd500`, `0x00fcd510`, `0x00fcd540` to `0x00fcd770`, adding 512
   roots from a 513-entry table. Growth17 then reached `0x0042ceb0`, `0x004016b0`,
   `0x0057d3c0` and `0x00fd2f00`; the last 1,025-entry table added 512 roots.
   Growth18 then reached `0x00fd8bb0` and `0x00fdeac0`, adding 512 roots at each
   step. Growth19 reached `0x00fe4710`, `0x00fea170`, `0x00fee960` and
   `0x00ff1dd0`, adding 1,453 roots. Growth20 reached `0x00ff26e0`, `0x00ff26f0`,
   `0x00ff2700` and `0x00ff2bf0`. VA backing is 3,636 KiB. Growth21 reached
   `0x00ff2de0`, `0x00ff2e10` and `0x00ff2ee0`; growth22 reached `0x00ff3240`
   and `0x00ff3260`. Bulk growth24 merged 6,454 initializer + 659 callee roots,
   admitted 42,711 functions with 0 cut targets, and moved the frontier to
   `0x0064be80`. Growth25 then reached `0x00a43250`; growth26 stage 1 walked
   `0x00459c60`, `0x00b06960`, `0x00adeb00` and `0x00a94b10`. Roots are now
   11,827 and VA backing 4,672 KiB. SteamAPI shim and first-draw capture are
   integrated, and the lifter now shards across 8 workers. The `aas/daa`
   rejections are false string-data entries, not
   executed opcodes (`docs/LEGACY_OPS.md`). The stable hardware slice
   growth13/gen01 is at `0x00403780`. D3D9 initialisation has not been reached yet.
3. **Import surface.** Diagnostic mode now binds **154** named/ordinal trap
   entries after fixing resolver order: implemented builtin → loaded PE exports
   → diagnostic fallback. All 18 D3DX, 5 Steam API and 9 Bink exports resolve from
   staged DLLs, but resolving an export does not lift its code. A parallel WinMain
   probe stops at `steam_api.dll!SteamAPI_RegisterCallback`; the standalone offline
   cdecl Steam shim is ready and tested, with shared wiring pending. Remaining traps
   are explicit msvcrt/GDI32/USER32/KERNEL32/ADVAPI32/ole32/WSOCK32 gaps. SEH/APC
   and other unimplemented calls still fail by exact `dll!name`/ordinal.
4. **D3D9 coverage — measured against the shipped shaders.** Skyrim ships
   10,475 compiled shader chunks: **854 vs_3_0 and 9,621 ps_3_0, zero SM2**;
   deduplication finds 6,659 distinct bytecode blobs / 6,655 instruction streams.
   The generic TEV translator accepts 19 PS chunks after rejecting invalid
   literal ranges instead of silently clamping them; the real CreatePixelShader
   path is tested. A separate structural menu-light prototype covers 6 retail
   logo signatures but is not auto-selected. Universal SM3 translation is not
   feasible on PICA200, so the first frame uses specialized families/multi-pass
   or a documented demake path. The adapter still reports SM2 caps truthfully.
   See `docs/SHADERS.md`.
5. **Hardware diagnostic validated.** Physical `hwhybridprobe` reached
   `0x00403780` in the 178 MiB application region with nonzero IR-cache counters
   and no loader/libctru crash. Large AOT CIAs still fail in the system loader,
   compressed or not. No physical D3D9/PICA draw or Skyrim frame exists yet.

## 6a. Renderer milestone (2026-11-26)

The measured TESV `CreateDevice` set is now accepted end to end:
`400x240 X8R8G8B8`, one back buffer, `DISCARD`, no MSAA, windowed, interval
ONE, `Flags=0x1` (`D3DPRESENTFLAG_LOCKABLE_BACKBUFFER`) and
`EnableAutoDepthStencil=0`. The lockable flag is backed by a retained 512x256
RGBA8 target with a real `Present` blit, `LockRect` read/write and
`StretchRect` sampling; explicit `D24S8`/`D24X8` depth-stencil surfaces attach
to the PICA depth attachment and `Clear(Z|STENCIL)` reaches hardware. The ARM
`com` and `tesv` targets link the new backend code. Host evidence is pixel-level
through the null backend only; Azahar pixel proof for the retained target,
present orientation and depth clear is still open. Details and the remaining
steps: `docs/BACK_BUFFER_LOCKABLE.md`. The HW T&L caps bit is still **not**
claimed; a CPU T&L prototype (fixed-function state, directional/point lights,
per-vertex lighting into the existing draw path) now exists behind
`TESV_D3D9_TNL_PROTO=1` for host tests and does not change `DevCaps`. Scope,
test evidence and the remaining work for a real claim:
`docs/HWTL_IMPLEMENTATION.md`.

## 7. Next concrete tasks

1. Container landed: runtime tier, bounded lazy cache, packer call-split fix and
   43-test suite. Growth28 runs with `N3DS_IRC_PATH` set, so unknown functions
   execute from the container instead of forcing a rebuild. Only addresses the
   container does not index (e.g. `0x00ce1b30`) still become roots.
2. Finish the CRT/C++ initializer trace to WinMain and D3D9 (`docs/INIT_TRACE.md`).
   Bulk roots took startup out of the initializer region; the frontier is now
   ordinary engine code.
3. Growth is supervised: `tools/growth_until_target.ps1` is running growth28
   with 8 lift workers, 6 clang jobs and the container tier.
4. Validate `hwnext2` on the physical New 3DS and then build a container-enabled
   hybrid CIA; no container ARM link or physical run has been done yet.
5. Guest threads: TESV spawns a worker at `0x00a5ac30`; the single-thread
   runtime reports `blocking wait would deadlock the single guest thread` and
   terminates it. A cooperative scheduler/wait path is required before phases
   that depend on that thread.
6. Close remaining first-frame audit items in `docs/PICA_BACKEND.md` §5, then
   trace adapter enumeration → CreateDevice → Reset → BeginScene/EndScene →
   Present. The synthetic `ui.copy` path already has pixel proof; the next target
   is a backend-compatible primitive through real TESV D3D9 calls.
7. Complete VA page statistics, decommit/cross-page/eviction/OOM guarantees and
   the first file-backed ESM/BSA mapping test.
8. Continue IR coverage for remaining explicit gaps (MMX/newer SSE), add measured
   ARM11 timing, and keep exact AOT/IR partitioning.
8. Finish texture lifetime/rehydrate gaps, runtime menu preprocessing and real
   game audio calls.
9. Fix data-root/function-start validation so string bytes such as `"sec\0"` and
   `"Fog\0"` cannot become false AAS/DAA function entries; do not add legacy
   opcodes merely to hide bogus roots (`docs/LEGACY_OPS.md`).
