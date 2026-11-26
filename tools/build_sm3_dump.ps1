# Build the host-only MojoShader-based SM3 dumper.
#
# Nothing built here is linked into the 3DS binary; this is an offline census
# tool. See tools/mojoshader/README.md for the vendor notice.
#
#   powershell -File tools\build_sm3_dump.ps1 [-OutDir <dir>] [-Clang <path>]

param(
    [string]$OutDir = "",
    [string]$Clang = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root "build_host" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

if (-not $Clang) {
    $candidates = @(
        "C:\Program Files\LLVM\bin\clang.exe",
        "C:\Program Files (x86)\LLVM\bin\clang.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $Clang = $c; break } }
    if (-not $Clang) {
        $cmd = Get-Command clang.exe -ErrorAction SilentlyContinue
        if ($cmd) { $Clang = $cmd.Source }
    }
}
if (-not $Clang) { throw "clang.exe not found; pass -Clang <path>" }

$mj = Join-Path $root "tools\mojoshader"
$out = Join-Path $OutDir "sm3_dump.exe"

$flags = @(
    "-std=c99", "-O1", "-w",
    "-DMOJOSHADER_NO_VERSION_INCLUDE=1",
    "-DSUPPORT_PROFILE_D3D=1",
    "-DSUPPORT_PROFILE_BYTECODE=0",
    "-DSUPPORT_PROFILE_HLSL=0",
    "-DSUPPORT_PROFILE_GLSL=0",
    "-DSUPPORT_PROFILE_GLSL120=0",
    "-DSUPPORT_PROFILE_GLSLES=0",
    "-DSUPPORT_PROFILE_GLSLES3=0",
    "-DSUPPORT_PROFILE_ARB1=0",
    "-DSUPPORT_PROFILE_ARB1_NV=0",
    "-DSUPPORT_PROFILE_METAL=0",
    "-DSUPPORT_PROFILE_SPIRV=0",
    "-DSUPPORT_PROFILE_GLSPIRV=0",
    "-I$mj", "-I$mj\profiles"
)

& $Clang @flags `
    (Join-Path $mj "mojoshader.c") `
    (Join-Path $mj "mojoshader_common.c") `
    (Join-Path $mj "profiles\mojoshader_profile_common.c") `
    (Join-Path $mj "profiles\mojoshader_profile_d3d.c") `
    (Join-Path $root "tools\sm3_dump.c") `
    "-o" $out
if ($LASTEXITCODE -ne 0) { throw "sm3_dump build failed ($LASTEXITCODE)" }
Write-Host "built $out"
