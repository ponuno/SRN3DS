# COMPATIBILITY — what is actually supported

This file separates implemented behaviour from placeholders. "Registered" does
not mean "semantically complete". Everything not listed returns an error or
raises a diagnostic trap; it is not silently treated as success.

## 1. Verified game files (local, not distributed)

| File | Image base | Image bytes | Executable bytes | SHA256 (prefix) |
|---|---:|---:|---:|---|
| TESV.exe | 0x00400000 | 27,336,704 | 13,579,264 | `69362f77bc56bf17` |

TESV.exe SHA-1 is `f39aecebf6e332616c4c518617073f6e8250e0f8`, matching the
1.9.32.0.8 build supported by upstream. The Steam copy is protected (`.text`
entropy 8.00, entry in `.bind`). A second, unprotected copy is available at
`E:\skyr\TESV.exe` (SHA-1 `17ec1ef6d72781359860f103f600711dfc219182`,
SHA256 `d97eb0cc0ceef0923d2234d42b382c0d033853356528b271bc043542b1f5e605`,
17,462,064 B, entry `0xf568a1` in `.text`). `tools/compare_pe.py` proves it is
the same build: `.data`, `.rdata`, `.reloc`, `.rsrc`, `.tls` are byte-identical;
only `.text` (decrypted) and the removed `.bind` differ. `tools/inspect_pe.py`
confirms the unprotected copy has normal section entropy. No DRM was bypassed
by this project; the file was supplied by the owner.
| binkw32.dll | 0x10000000 | 290,816 | 139,264 | `13a57401c477be42` |
| steam_api.dll | 0x3b400000 | 163,840 | 91,648 | `025a1fd2b3e81a1a` |
| D3DX9_42.dll (system, 32-bit) | 0x10000000 | 1,890,000+ | — | `f8b9cfab7fffbc8f` |
| X3DAudio1_7.dll | — | not found next to game | — | — |

`skyrim_audit.json` leaves out delay imports, dynamic `GetProcAddress` targets,
generated ARM code, resources and runtime heap.

## 2. Static import coverage (TESV + binkw32 + steam_api)

| DLL | Unresolved unique names |
|---|---:|
| kernel32.dll | 75 |
| user32.dll | 50 (window/message APIs, none implemented) |
| wsock32.dll | 21 (ordinal imports) |
| d3dx9_42.dll | 18 (pilot covers 4 matrix functions) |
| gdi32.dll | 15 |
| winmm.dll | 12 |
| advapi32.dll | 6 (registry) |
| ole32.dll | 6 |
| shell32.dll | 2 |
| xinput1_3.dll | 3 (ordinals) |
| x3daudio1_7.dll | 2 |
| dinput8.dll | 1 |
| d3d9.dll | 1 (`D3DPERF_SetOptions`; `Direct3DCreate9` is dynamic) |

Total unique unresolved: 205 (v6 audit). 244 unresolved import entries across the
three modules; 189 entries already mapped to shims. Local exports not yet
lifted: 14. `references` counts importing modules, not call sites.

### Target formats and memory mode

| Format | Application memory | Notes |
|---|---:|---|
| `.3dsx` (Homebrew Launcher / Azahar) | 96 MiB region observed | development and CI path |
| CIA, `SystemModeExt: 178MB` (N3DS) | 178 MiB per libctru `SYSMODE_DEV1` | `tools/build_cia.ps1`; runtime confirmation pending |
| CIA, 124 MB / production | 124 MiB | not requested |

The CIA build requests 178 MiB, 804 MHz and L2 cache in
`tools/cia/template.rsf`. It is a local-build artifact derived from the owner's
files and must not be distributed. A `.3dsx` cannot request the 178 MiB mode.

## 3. Win32 / CRT subset (registered, fixture-tested)

| Group | Functions |
|---|---|
| Errors | GetLastError, SetLastError |
| Heap | GetProcessHeap, HeapAlloc/Free/Size/ReAlloc, HeapCreate/HeapDestroy (up to 4 additional real guest heaps at 0x30000000+, 64 KiB–8 MiB each; process heap handle 1) |
| Virtual memory | `VirtualAlloc/VirtualFree/VirtualQuery`: real reserve/commit/decommit/release, 64 KiB reservation granularity, 4 KiB commit rounding, idempotent re-commit, and lazy demand-zero backing. The 3DS path uses non-contiguous pages because `MEMOP_RESERVE` is unavailable. `CreateFileMappingA/MapViewOfFile/UnmapViewOfFile` use the same page-provider model; I/O and backing exhaustion have distinct diagnostics. |
| Time | GetTickCount, QueryPerformanceCounter/Frequency, Sleep, GetSystemTimeAsFileTime, GetLocalTime, GetTimeZoneInformation (fixed current offset), CompareFileTime, FileTimeToSystemTime, SystemTimeToFileTime, FileTimeToLocalFileTime, LocalFileTimeToFileTime |
| Process/thread | GetCurrentProcess/Thread (+Id), GetSystemInfo, ExitProcess, TerminateProcess (current process only), IsDebuggerPresent (FALSE), IsProcessorFeaturePresent (all FALSE: conservative) |
| TLS/FLS | TlsAlloc/Free/Get/SetValue (576 slots, TEB-backed); FlsAlloc/Free/Get/SetValue (128 slots, single thread) |
| Modules | GetModuleHandleA/W, GetProcAddress, LoadLibraryA, FreeLibrary, GetModuleFileNameA |
| Strings/misc | lstrlenA, lstrcmpiA, lstrcpyA, lstrcatA, OutputDebugStringA, FormatMessageA (generic fallback text, not the OS message table) |
| Atomics | InterlockedIncrement/Decrement/Exchange/CompareExchange/ExchangeAdd (single guest thread) |
| COM | CoInitialize/CoInitializeEx/CoUninitialize (refcount only), CoCreateInstance returns REGDB_E_CLASSNOTREG |
| Misc | SetHandleCount, CopyFileA, GetUserNameA ("Player") |
| user32 basics | Separate module: RegisterClassA, CreateWindowExA, rects, AdjustWindowRect, SetWindowPos/Text, ShowWindow, FindWindowA (NULL), GetSystemMetrics (400×240, remote-session 0), MessageBoxA (logs), PeekMessageA (empty queue), LoadIconA, cursor/key stubs, window/class longs, GetDC/ReleaseDC, GetStockObject |
| Synchronization | Separate module: Create/OpenMutexA, ReleaseMutex, Create/OpenSemaphoreA, ReleaseSemaphore, Create/OpenEventA, Set/ResetEvent, WaitForSingle/ForMultipleObjects (single thread; INFINITE deadlock traps) |
| Locale | Single en-US/LCID 0x0409: GetUser/SystemDefaultLCID+LANGID, IsValidLocale, GetLocaleInfoA/W (20 values + RETURN_NUMBER), CompareStringA/W (ordinal; documented deviation), GetDateFormatA/GetTimeFormatA (default pictures only) |
| shell32 | SHGetFolderPathA/W (known CSIDLs answer a sandbox-relative directory: LocalAppData/Documents/AppData/ProgramData; unknown CSIDL and an unconfigured root return E_FAIL; no Windows profile is invented), ShellExecuteA (SE_ERR_NOASSOC) |
| registry | Bounded in-memory ADVAPI32 subset: RegOpenKeyExA, RegCreateKeyExA, RegQueryValueExA, RegSetValueExA, RegCloseKey. Case-insensitive fixed tree (64 keys, 16 handles, 8 values/key, 512 B/value). Missing keys and values return ERROR_FILE_NOT_FOUND and are logged once; unsupported value types and access rights are refused. No host registry, no persistence, no preloaded values. |
| winmm / audio | `waveOutOpen/Close/PrepareHeader/UnprepareHeader/Write/Reset/Pause/Restart/SetVolume/GetPosition/GetNumDevs/GetDevCaps` over a real backend (OpenDSP/ndsp on ARM, null sink on host); PCM 8/16-bit mono/stereo, 4–192 kHz with a documented linear resampler to 32 730 Hz, deferred `WHDR_DONE`, correct `MMRESULT`/`WAVERR_*` codes, explicit refusal for float/24-bit/EXTENSIBLE/>2 channels. The TESV 3DS runner installs the backend at startup via `win32_audio_3ds_install()`; no backend means `MMSYSERR_NODRIVER` |
| X3DAudio 1.7 | `X3DAudioInitialize/Calculate` stereo subset: distance attenuation, LPF, cone, Doppler, constant-power matrix; delay/reverb/LFE/HRTF/inner radius return `E_NOTIMPL` with a named reason |
| Threads | CreateThread returns NULL + ERROR_CALL_NOT_IMPLEMENTED (120) after logging start/param; ResumeThread/SetThreadPriority/Affinity/GetThreadPriority work only on the current-thread pseudo-handle |
| Critical sections | InitializeCriticalSection, InitializeCriticalSectionAndSpinCount, DeleteCriticalSection, Enter/LeaveCriticalSection (24-byte guest layout, recursion count, one expected thread; no blocking/waiting) |
| Current directory | GetCurrentDirectoryA, SetCurrentDirectoryA (relative subdirectories only; file opens resolve against it; absolute/drive paths are rejected) |
| Code pages / Unicode | GetACP (1252), GetOEMCP (437 query), IsValidCodePage, GetCPInfo, MultiByteToWideChar, WideCharToMultiByte (CP_ACP fully mapped, UTF-8 supported; OEM non-ASCII conversion rejected) |
| Std handles/console | GetStdHandle, SetStdHandle, GetFileType (char for std, disk for real handles) |
| Memory | LocalAlloc/LocalFree (process heap; LMEM_ZEROINIT) |
| Pointers | EncodePointer/DecodePointer (process XOR key, round-trip only) |
| SEH filters | SetUnhandledExceptionFilter (stores/returns previous), UnhandledExceptionFilter (CONTINUE_SEARCH); exact `SetThreadName` `RaiseException(0x406d1388, {0x1000,name,tid,0})` continues execution; every other raise traps; DebugBreak traps |
| Command line/env | GetCommandLineA/W, GetEnvironmentStringsA/W, FreeEnvironmentStringsA/W, GetStartupInfoA |
| Files | CreateFileA (read/write, dispositions 1–5, share 0–3 accepted but not enforced), ReadFile, WriteFile, CloseHandle, GetFileSize, GetFileSizeEx, SetFilePointer, SetFilePointerEx, SetEndOfFile, FlushFileBuffers |
| File/dir metadata | GetFileAttributesA, GetFileAttributesExA (WIN32_FILE_ATTRIBUTE_DATA), GetFullPathNameA (sandbox-relative), CreateDirectoryA, DeleteFileA |
| Directory enumeration | FindFirstFileA, FindNextFileA, FindClose (320-byte WIN32_FIND_DATAA, `*`/`?`, 8 concurrent enumerations; no 8.3 alias) |
| CRT | malloc/calloc/realloc/free, memcpy/memmove/memset/memcmp, strlen/strcmp/_stricmp/strcpy/_strdup |

Not implemented: thread creation and scheduling, waitable objects
(mutex/semaphore/event), WaitForSingleObject blocking semantics, recursive
directory creation, directory removal, Unicode file paths (`CreateFileW`),
sockets, window/message/input, general real SEH/unwind (beyond the exact
`SetThreadName` continue pattern and `setjmp` support used by lifted code),
`__security_cookie` initialization. Share modes are accepted but not enforced between handles;
`cAlternateFileName` is left empty. Critical-section waits are only meaningful
for the single expected guest thread. Code-page conversion for CP_OEMCP
non-ASCII bytes returns an error rather than guessing.

## 4. D3D9 interface subset

| Interface | Methods |
|---|---|
| All objects | QueryInterface, AddRef, Release |
| IDirect3D9 | GetAdapterCount, GetAdapterIdentifier, GetAdapterModeCount, EnumAdapterModes, GetAdapterDisplayMode, CheckDeviceType, CheckDeviceFormat, CheckDeviceMultiSampleType, CheckDepthStencilMatch, CheckDeviceFormatConversion, RegisterSoftwareDevice, GetAdapterMonitor, CreateDevice |
| IDirect3DDevice9 | GetDeviceCaps, TestCooperativeLevel, GetDirect3D, Reset, GetDisplayMode, GetCreationParameters, GetBackBuffer, CreateVertexBuffer/IndexBuffer/Texture/VertexDeclaration, CreateOffscreenPlainSurface, Set/GetRenderTarget, SetViewport, Set/GetScissorRect (state only, no scissor stage), Set/GetGammaRamp (state only, no output gamma), Set/GetNPatchMode (state only; patch draws unsupported), Set/GetClipPlane (6 float4 planes stored; `D3DRS_CLIPPLANEENABLE != 0` makes every draw a named refusal because PICA has no user clip-plane stage), Set/GetClipStatus (legacy CPU state; no draw effect), Set/GetSoftwareVertexProcessing (FALSE accepted, TRUE named; Get always 0), StretchRect (explicit-target copies; back-buffer source unsupported), CreateVertexShader/PixelShader, Set*Shader, Set*ShaderConstantF, CreateQuery (EVENT only), BeginScene, EndScene, Clear, Present, SetTransform, Set/GetFVF, Set/GetStreamSource, Set/GetStreamSourceFreq, SetIndices, SetTexture, SetTextureStageState (stage 0), DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP, DrawIndexedPrimitiveUP, Set/GetRenderState, Set/GetSamplerState, Set/GetVertexDeclaration |
| Vertex/Index buffer | GetDevice, Lock/Unlock, GetType, GetDesc |
| Texture | GetDevice, GetType, GetLevelCount, GetLevelDesc, LockRect/UnlockRect (BGRA + BC, mips, partial rects). Pool-aware core/COM residency wiring is implemented: DEFAULT/MANAGED retain guest restoration sources, SYSTEMMEM is CPU-only, and direct SKTX/ETC1 creation is GPU-only. Full MANAGED eviction/rehydrate, Reset preservation, DISCARD and automatic guest drop remain pending. |
| VertexDeclaration | GetDevice, GetDeclaration, QueryInterface/AddRef/Release |
| VertexShader/PixelShader | GetDevice, GetFunction, AddRef/Release |
| IDirect3DQuery9 | GetDevice, GetType, GetDataSize, Issue, GetData. Only `D3DQUERYTYPE_EVENT` exists; it reports a 4-byte BOOL that is TRUE after Issue (synchronous backend). Occlusion, VCACHE, resource-manager, timestamp and all timing queries are named `D3DERR_NOTAVAILABLE`. |
| Shader constants | VS float c0..c8 (legacy menu bank; advertised `MaxVertexShaderConst` stays 4), PS float c0..c7 (PICA TEV external constants); VS/PS I use 8 int4 vectors and B uses 8 BOOLs. I/B round-trip but no accepted VS/PS opcode consumes them. |
| Shaders | Two exact SM2 templates plus a specialized ps_3_0 → PICA200 TEV translator for `nop/mov/add/sub/mad/mul/dp3/lrp/texld/dcl/def` (6 stages, 3 texture units). Generic real-data translation accepts **19 of 9,621 shipped PS chunks** after rejecting non-finite/out-of-[0,1] literals rather than clamping them. Guest `CreatePixelShader` integration is tested (shipped blit/tint become TEV; unsupported `cmp` fails with a named reason). A separate structural menu-light matcher covers **6 additional retail logo signatures** with a five-stage TEV prototype, but requires explicit normal/tangent/material preconditions and is not silently auto-selected. **All 10,475 shipped chunks are SM3; there is no universal SM3 translator.** |

Formats/features: A8R8G8B8/X8R8G8B8, DXT1/3/5, power-of-two 8..1024, full and
partial mip chains, vertex formats FLOAT1-4/16_2/16_4, D3DCOLOR, UBYTE4(N),
SHORT/USHORT normalized, UDEC3/DEC3N; FVF `XYZ|TEX1`; INDEX16/INDEX32;
TRIANGLELIST/STRIP/FAN (CPU expansion); one stream set with declarations
(up to 16 streams), one texture stage; depth/alpha/blend/cull/basic sampler
states; PICA reverse-depth mapping.

Adapter surface (truthful, test `adapter`): one adapter, exactly one mode
**400×240×32 `D3DFMT_X8R8G8B8`**, `GetAdapterIdentifier` names the PICA200
bridge, `CheckDeviceType` succeeds only for windowed X8/A8, `CheckDeviceFormat`
only for A8/X8/DXT1/3/5 textures with normal or filter/wrap queries, usage-0 A8/X8
plain surfaces, and exactly `D3DUSAGE_RENDERTARGET` A8/X8 surfaces. Caps report
**VS 2.0 / PS 2.0** (`0xFFFE0200`/`0xFFFF0200`), `MaxVertexShaderConst=4`,
no SM3 slots, no HW T&L, one active render target (implicit or explicit).
`CreateDevice` builds a real
`D9Device` through `d3d9_core` when a backend is registered and otherwise
returns `D3DERR_NOTAVAILABLE` with a log line (enumeration-only session).
Nothing claims SM3. The specialized TEV path is integrated and tested, but caps
remain SM2 until the required menu shader families and material preconditions
are selectable through the real renderer. A device may legitimately report PS
3.0 while rejecting individual unsupported shaders; it must not claim coverage
it has not tested.

Not supported: universal SM3 translation, fog, anisotropic filtering, multiple
sampler stages, state blocks, automatic mip generation, instancing frequency >1,
arbitrary FVF/declaration semantics (normals, skinning, colors are validated but
unused in the production path; a test-only CPU T&L prototype can consume NORMAL
behind `TESV_D3D9_TNL_PROTO=1`), ProcessVertices,
multisampling, queries, swap-chain control. Reset,
back-buffer, CPU offscreen surface, one explicit LDR render-target and viewport
methods work through COM. `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` is real: a
retained 512x256 target for the 400x240 mode with a Present blit and
LockRect/UnlockRect read/write. Explicit `D24S8`/`D24X8` depth-stencil descriptors
(pow2 8..1024 or the exact 400x240 back-buffer size) attach to the active
target's PICA depth attachment, are refused when the size does not match, and
`Clear(Z|STENCIL)` reaches it.
The PICA target path has synthetic Azahar pixel proof (two independent 64x64
targets, readback and back-buffer restore); it is not yet a Skyrim frame. The
retained target and depth attachment are host-tested and compiled for ARM;
their PICA pixel proof is pending. Texture RTs/MRT/resolve remain absent.

## 5. Recompiled-x86 subset (upstream lifter + our staging)

Supported and fixture-tested: core integer ops, flags, x87/FPU, the measured
SSE/SSE2 instruction set, string ops including `rep movsd`, single-threaded
LOCK/CAS, indirect call through function table with cache, guest stdcall/cdecl,
guest memory faults, and AOT↔IR equivalence for the covered subset. IR v2
decodes 25,350/25,360 real functions; 10 block-edge functions remain forced AOT.

Not yet proven at scale: MMX, SSE3/SSSE3/SSE4 beyond the implemented set, the
full x87 rounding/denormal matrix, unaligned and cross-region accesses,
exceptions, multi-threaded atomics, callbacks from host into guest beyond the
tested paths, and New 3DS interpreter throughput.

## 6. Honest status of "no fake success"

- Unknown D3D9 COM slot → guest trap naming the method.
- Unknown shader → `D3DERR_NOTAVAILABLE` plus version/instruction/hash log.
- Unknown guest function → `UNLIFTED_FUNCTION`-style fault (no arbitrary jump).
- Unknown memory → `UNMAPPED guest memory` with the guest address.
- Diagnostic slice mode binds unresolved imports to unique trap addresses; the
  first call traps with the exact `dll!name` or `dll!#ordinal`. Resolution order
  is implemented builtin → loaded PE export → diagnostic fallback, so a trap
  cannot mask a real staged DLL export. Binding succeeds, but no call returns a
  fabricated success.
- Lazily initialized DLL export → resolved only through the verified MSVC thunk
  layout; if the pattern differs, generation fails instead of guessing.
- The D3DX lazy initializer is bypassed and this is printed in the log and
  stored in `pilot_manifest.json`; it is not presented as executed.
