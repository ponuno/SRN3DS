# Dynamic D3D9 resolution chain smoke test

## Status: PASS

`tests/d3d9_dynamic_chain_smoke.c` is an isolated end-to-end test of the
runtime D3D9 entry path. It starts at the Win32 shims, not at a statically
bound PE import, and finishes at a real guest `IDirect3DDevice9` scene on the
host null backend.

This is a **chain test only**. It proves dynamic module lookup, guest thunk
dispatch, COM object identity, adapter enumeration, device creation, null
backend call accounting, and teardown. It makes **no Skyrim frame claim** and
produces no pixels.

No production source, `Makefile`, or `tools/run_host_tests.ps1` was changed for
this test.

## Path exercised

```text
LoadLibraryA("d3d9.dll")                      source/win32_shims.c:1199
  -> module_handle -> d3d9 host resolver      source/win32_shims.c:1174
GetProcAddress(handle, "Direct3DCreate9")     source/win32_shims.c:1201
  -> d9_com_resolve                           source/d3d9_com.c:834
  -> guest export thunk (0xfe......)          source/d3d9_com.c:138
  -> IDirect3D9 factory guest object          source/d3d9_com.c:96,140
  -> GetAdapterCount / Identifier / Modes     source/d3d9_com.c:163-201
  -> IDirect3D9::CreateDevice                 source/d3d9_com.c:319
  -> IDirect3DDevice9 guest object            source/d3d9_com.c:335
  -> GetDeviceCaps                            source/d3d9_com.c:285
  -> BeginScene / EndScene / Present          source/d3d9_com.c:572-574
  -> D9NullStats frame counters               source/d3d9_null.c:37-46
```

The test calls the guest shims through `n3ds_lift_invoke`, the same AOT/IR
dispatch used by the existing COM harnesses. `LoadLibraryA` and
`GetProcAddress` are resolved on the host with `win32_resolve` only to obtain
the guest thunk addresses to invoke; the D3D9 symbol itself is resolved inside
the guest shim chain.

## Assertions

### Dynamic lookup and thunks

- `LoadLibraryA("d3d9.dll")` returns a non-zero module handle.
- `GetModuleHandleA("d3d9.dll")` returns the same handle.
- `GetProcAddress(handle, "Direct3DCreate9")` succeeds and equals the
  `d9_com_resolve()` static-import answer exactly.
- The returned address is in the D3D9 guest thunk range
  `[0xfe000000, 0xff000000)`, not a host pointer and not a Win32 shim address.

### Factory identity and lifetime

- The thunk returns the expected guest `IDirect3D9` object:
  address `0x50002000`, vtable `0x50000200`.
- Initial factory refcount is 1.
- A second `Direct3DCreate9` call returns the same object and increments the
  refcount to 2; releasing the extra reference restores 1.
- `QueryInterface(IID_IDirect3D9)` returns the same guest pointer and balances
  its added reference.

### Adapter enumeration

- `GetAdapterCount()` is 1.
- `GetAdapterIdentifier()` returns the exact driver, description, device name,
  and `Skyrim_N3DS_3D9` device identifier; `WHQLLevel` is 0.
- `D3DENUM_WHQL_LEVEL` returns `D3DOK_NOAUTOGEN` (`0x0876086f`).
- `GetAdapterModeCount(0, X8R8G8B8)` is 1.
- `EnumAdapterModes` and `GetAdapterDisplayMode` both report
  400x240 at 60 Hz in `D3DFMT_X8R8G8B8`.
- `CheckDeviceType(HAL, X8R8G8B8, X8R8G8B8, windowed)` succeeds.

### Device identity and lifetime

- `CreateDevice` succeeds with 400x240 X8R8G8B8, one back buffer, discard swap,
  windowed, no depth-stencil, and hardware vertex processing behavior.
- The returned guest `IDirect3DDevice9` is at `0x50002010` with vtable
  `0x50000400`.
- `CreateDevice` retains the factory: factory refcount is 2 and device refcount
  is 1.
- `QueryInterface(IID_IDirect3DDevice9)` returns the same device pointer.
- `GetDirect3D()` returns the same factory pointer and balances its reference.
- `GetBackBuffer(0,0,0)` returns the implicit guest surface at `0x50002020`
  with vtable `0x50001200`; its refcount is 2 (device's implicit reference plus
  the returned reference), then 1 after release.
- `GetCreationParameters` reports adapter 0, `D3DDEVTYPE_HAL`, focus window 0,
  and the exact behavior flags passed to `CreateDevice`.
- `GetDeviceCaps` reports `D3DDEVTYPE_HAL`, adapter ordinal 0,
  vertex shader version `0xfffe0200` (SM2), pixel shader version `0xffff0200`
  (SM2), and 1024 maximum texture width.

### Null backend frame and teardown

- `BeginScene` / `EndScene` / `Present` each return success.
- `D9NullStats` records exactly `begins=1`, `ends=1`, `presents=1`,
  `draws=0`, and `scene=0`.
- A second `EndScene` without a new `BeginScene` and a second `Present` both
  return `D3DERR_INVALIDCALL` (`0x8876086c`).
- Releasing the device destroys the implicit back buffer and drops the
  factory reference from 2 to 1.
- Releasing the factory drops it to 0.
- The guest vtable words of the factory, device, and back buffer are zeroed
  after destruction.

The exact guest addresses and vtables are intentionally asserted. They are the
current deterministic layout of `d3d9_com.c` (`OBJ_BASE=0x50002000`, 16-byte
stride, vtable `0x50000000 + kind*0x200`). If that layout changes, the test
fails loudly rather than accepting a different object silently.

## Host build and run (clang)

Environment used: clang 23.1.5 with the Visual Studio 18 x64 environment.
The source list matches `null_backend_smoke` plus the new test.

```powershell
$out = "$env:TEMP\d3d9_chain_host"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$src = @(
  "source/guest.c","source/guest_heap.c","source/pe_image.c","source/pe_imports.c",
  "source/win32_shims.c","source/win32_sync.c","source/win32_user.c",
  "source/win32_locale.c","source/win32_shell.c","source/win32_audio.c",
  "source/win32_audio_3ds.c","source/win32_thread.c","source/win32_input.c",
  "source/n3ds_lift_runtime.c","source/ir_core.c","source/ir_decode.c",
  "source/ir_exec.c","source/rt_prof.c","staged/fpu.c",
  "source/d3d9_core.c","source/d3d9_texcache.c","source/d3d9_texture_data.c",
  "source/d3d9_bc.c","source/d3d9_vertex.c","source/d3d9_adapter.c",
  "source/d3d9_shader.c","source/d3d9_tev.c","source/d3d9_null.c",
  "source/d3d9_pica_tev.c","source/d3d9_com.c",
  "tests/d3d9_dynamic_chain_smoke.c"
)

$cmd = 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && ' +
       '"C:\Program Files\LLVM\bin\clang.exe" -std=gnu11 -O1 -g -Istaged -Isource ' +
       '-fwrapv -fno-strict-aliasing -Wno-deprecated-declarations -Wno-format ' +
       '-Wl,/STACK:67108864 ' + ($src -join " ") +
       ' -o "' + $out + '\d3d9_dynamic_chain_smoke.exe"'
& cmd.exe /c $cmd
& "$out\d3d9_dynamic_chain_smoke.exe"
```

Result:

```text
D3D9 dynamic chain: LoadLibraryA -> GetProcAddress -> Direct3DCreate9 thunk ->
IDirect3D9 -> adapter enumeration -> CreateDevice -> IDirect3DDevice9
caps/scene -> Present passed
exit code 0
```

The test source also compiles warning-clean on its own with
`-Wall -Wextra -fsyntax-only`.

### Sanitizer run

The same build with `-fsanitize=address,undefined -fno-omit-frame-pointer`
also builds and runs:

```powershell
# same command as above, adding:
#   -fsanitize=address,undefined -fno-omit-frame-pointer
# and the LLVM Windows sanitizer runtime directory on PATH
```

Result:

```text
D3D9 dynamic chain: ... Present passed
ASan/UBSan: no report
exit code 0
```

## Isolated ARM check

The same test compiled for ARM11 with devkitARM 15.2.0 and ran in Azahar. The
test uses the null backend, so this is a reachability/lifetime check of the same
bridge on ARM, not a PICA200 draw and not a game frame.

```powershell
$root = "C:\Users\vasya\Documents\Skyrim_N3DS"
$out  = "$env:TEMP\d3d9_chain_arm"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$gcc  = "C:\devkitPro\devkitARM\bin\arm-none-eabi-gcc.exe"
$ctru = "C:/devkitPro/libctru"
$port = "C:/devkitPro/portlibs/3ds"
$flags = "-g -O2 -std=gnu11 -march=armv6k -mtune=mpcore " +
         "-mfloat-abi=hard -mtp=soft -D__3DS__ -fwrapv " +
         "-fno-strict-aliasing -ffunction-sections -fdata-sections"

# $src is the same list as the host build.
$objs = @()
foreach ($s in $src) {
  $o = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($s) + ".o")
  $objs += $o
  $cmd = '"' + $gcc + '" ' + $flags +
         ' -I"' + $root + '/source" -I"' + $root + '/staged"' +
         ' -I"' + $ctru + '/include" -I"' + $port + '/include"' +
         ' -c "' + (Join-Path $root $s) + '" -o "' + $o + '"'
  & cmd.exe /c $cmd
  if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
}

$olist = ($objs | ForEach-Object { '"' + $_ + '"' }) -join " "
$cmd = '"' + $gcc + '" -specs=3dsx.specs -g ' + $flags +
       ' -Wl,--gc-sections ' + $olist +
       ' -L"' + $ctru + '/lib" -L"' + $port + '/lib"' +
       ' -lopendsp -lctru -lm -o "' + $out + '\d3d9_chain_arm.elf"'
& cmd.exe /c $cmd

& "C:\devkitPro\tools\bin\3dsxtool.exe" `
  "$out\d3d9_chain_arm.elf" "$out\d3d9_chain_arm.3dsx"
```

The binary was copied to the Azahar virtual SD and launched directly. The test
writes its result to `sdmc:/d3d9_dynamic_chain.log`:

```powershell
$az  = "C:\Program Files\Azahar\azahar.exe"
$sd  = Join-Path $env:APPDATA "Azahar\sdmc"
$app = Join-Path $sd "3ds\d3d9_chain"
New-Item -ItemType Directory -Force -Path $app | Out-Null
Copy-Item "$env:TEMP\d3d9_chain_arm\d3d9_chain_arm.3dsx" `
  (Join-Path $app "d3d9_chain_arm.3dsx") -Force
$proc = Start-Process -FilePath $az `
  -ArgumentList ('"' + (Join-Path $app "d3d9_chain_arm.3dsx") + '"') -PassThru
# bounded wait for sdmc:/d3d9_dynamic_chain.log, then close Azahar
```

Observed result:

```text
D3D9CHAIN RESULT: PASS
```

## Recorded evidence

| Check | Result |
|---|---|
| Host clang 23.1.5 build | PASS, exit 0 |
| Host ASan/UBSan build + run | PASS, no report, exit 0 |
| ARM devkitARM 15.2.0 link | PASS, `.3dsx` 560672 bytes |
| Azahar ARM run | PASS, `D3D9CHAIN RESULT: PASS` |

Artifact hashes from this run:

```text
d3d9_dynamic_chain_smoke.exe SHA-256:
668F7B91DF1C9E938F29FF4A4DA49CCEB1DA1EA450D3843DAF89E9909EED28DF

d3d9_chain_arm.3dsx SHA-256:
38346A23179DE949A67FAD7E214142038C2F67C599E892174B6566EF9DD8C161
```

## Defect review

No real defect was found in the production chain, so no production file was
modified. The dynamic path behaves exactly like the statically bound import
path: both resolve `d3d9.dll!Direct3DCreate9` to the same guest thunk, and the
thunk produces the same singleton guest factory.

Two properties are worth keeping in mind for future changes:

- `GetProcAddress` returns the executable thunk address, not a host callback
  pointer. The test checks both identity with `d9_com_resolve()` and the
  `0xfe......` address range.
- Factory/device lifetimes are shared with the bridge's internal object slots.
  The test asserts exact refcount transitions (factory 1 -> 2 -> 1 -> 0,
  device 1 -> 0, back buffer 2 -> 1 -> 0) so an accidental extra retain or
  release fails immediately.

## Scope statement

This test does **not** claim:

- a Skyrim menu, UI, or game frame was rendered;
- the PICA200 backend produced pixels;
- the real game executable reached this path.

It claims only that the dynamic Win32 -> D3D9 guest thunk -> COM factory ->
adapter -> device -> null-backend Present chain is reachable and
reference-correct on the host and on ARM.
