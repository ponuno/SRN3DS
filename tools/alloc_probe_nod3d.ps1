#requires -Version 5.1
<#
.SYNOPSIS
  Build/run the deep TESV startup slice with D3D9 intentionally unavailable.
.DESCRIPTION
  This diagnostic isolates the AOT startup/allocator path from concurrent
  d3d9_*/IR work. It compiles the real shims, guest memory, runtime and lifted
  TESV slice, but links a small D3D9-unavailable stub instead of the graphics
  modules. It is not a playable build and does not claim renderer parity.
#>
param(
    [Parameter(Mandatory=$true)][string]$TesvGen,
    [int]$BudgetMB = 160,
    [int]$Steps = 5000000,
    [string]$OutDir = "E:\Skyrim_N3DS_build\host",
    [string]$TesvExe = "E:\skyr\TESV.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not (Test-Path (Join-Path $TesvGen "tesv_table.c"))) {
    throw "no tesv_table.c in $TesvGen"
}
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$run = Join-Path $OutDir ("alloc_probe_$stamp")
New-Item -ItemType Directory -Force -Path $run | Out-Null
$exe = Join-Path $run "tesv_slice.exe"
$buildLog = Join-Path $run "build.log"

$vsRoot = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vsVars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
$clang = "C:\Program Files\LLVM\bin\clang.exe"
if (-not (Test-Path $vsVars)) { throw "vcvars64.bat not found" }
if (-not (Test-Path $clang)) { throw "clang not found" }

$src = @(
    "source/guest.c", "source/guest_heap.c", "source/pe_image.c",
    "source/pe_imports.c", "source/win32_shims.c", "source/bink_decode.c", "source/win32_sync.c",
    "source/win32_user.c", "source/win32_locale.c", "tools/alloc_win32_shell_fixed.c",
    "source/win32_audio.c", "source/win32_audio_3ds.c", "source/win32_thread.c",
    "source/win32_input.c", "source/n3ds_lift_runtime.c", "source/ir_core.c",
    "source/ir_decode.c", "source/ir_exec.c", "source/rt_prof.c", "staged/fpu.c",
    "source/guest_process.c", "source/n3ds_boot.c", "source/n3ds_pilot.c",
    "source/tesv_slice_main.c", "tools/alloc_probe_stub.c"
)
$src += Get-ChildItem -Path $TesvGen -Filter "tesv_*.c" | ForEach-Object { $_.FullName }
$flags = @("-std=gnu11", "-O1", "-g", "-Istaged", "-Isource", "-fwrapv",
           "-fno-strict-aliasing", "-Wno-deprecated-declarations", "-Wno-format",
           "-Wl,/STACK:67108864", "-O0", "-I`"$TesvGen`"")
$quoted = $src | ForEach-Object { '"' + $_ + '"' }
$cmd = 'call "' + $vsVars + '" >nul && "' + $clang + '" ' +
       ($flags -join ' ') + ' ' + ($quoted -join ' ') + ' -o "' + $exe + '"'
"[$([DateTime]::Now.ToString('s'))] $cmd" | Out-File $buildLog -Encoding utf8
Write-Host "alloc probe build: $run"
$ErrorActionPreference = "Continue"
$build = & C:\Windows\System32\cmd.exe /c $cmd 2>&1
$buildCode = $LASTEXITCODE
$ErrorActionPreference = "Stop"
$build | Out-File $buildLog -Append -Encoding utf8
if ($buildCode -ne 0) {
    Get-Content $buildLog -Tail 40 | ForEach-Object { Write-Host $_ }
    throw "alloc probe compile failed"
}

# Preserve the same default INI the ordinary probe run uses.
$oldIni = Get-ChildItem $OutDir -Directory -Filter "run_tesv_slice_*" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    ForEach-Object { Join-Path $_.FullName "Skyrim_default.ini" } |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if ($oldIni) { Copy-Item $oldIni (Join-Path $run "Skyrim_default.ini") -Force }

$env:TESV_MEMPROBE = "1"
$args = @($TesvExe, ".", "$Steps", "$($BudgetMB * 1MB)")
Push-Location $run
try {
    $ErrorActionPreference = "Continue"
    & $exe @args *>&1 | Tee-Object -FilePath (Join-Path $run "probe_run.log") |
        ForEach-Object { Write-Host $_ }
    $runCode = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    Write-Host "exit=$runCode"
} finally {
    Pop-Location
    Remove-Item Env:\TESV_MEMPROBE -ErrorAction SilentlyContinue
}
Write-Host "run dir: $run"
