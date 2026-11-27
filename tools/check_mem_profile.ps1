#requires -Version 5.1
<#
.SYNOPSIS
  Build and run the host memory-profile test, then print the profile table.
.DESCRIPTION
  Compiles source/mem_stats.c + tests/mem_stats_test.c with the same clang/MSVC
  environment as tools/run_host_tests.ps1.  The test prints the profile
  arithmetic table and checks env parsing, guest peak, linear/VRAM deltas, the
  snapshot line format and counter leaks.  Exits non-zero on any failure.
#>
param(
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$clang = "C:\Program Files\LLVM\bin\clang.exe"
if (-not (Test-Path $clang)) { throw "clang not found at $clang" }
$vsVars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vsVars)) { throw "vcvars64.bat not found at $vsVars" }

$vsDump = & C:\Windows\System32\cmd.exe /c "call `"$vsVars`" >nul && set" 2>$null
$vsEnv = @{}
foreach ($line in $vsDump) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { $vsEnv[$line.Substring(0, $i)] = $line.Substring($i + 1) }
}
foreach ($k in $vsEnv.Keys) { Set-Item -Path "env:$k" -Value $vsEnv[$k] }

if (-not $OutDir) { $OutDir = Join-Path $root "build_host\mem_stats" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$exe = Join-Path $OutDir "mem_stats_test.exe"

& $clang -std=gnu11 -O1 -g -Isource -D_CRT_SECURE_NO_WARNINGS `
    "source/mem_stats.c" "tests/mem_stats_test.c" -o $exe
if ($LASTEXITCODE -ne 0) { throw "clang failed with exit code $LASTEXITCODE" }

& $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
