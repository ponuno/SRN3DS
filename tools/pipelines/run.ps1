#requires -Version 5.1
<#
.SYNOPSIS
  Build and run the specialised graphics-pipeline registry tests without
  modifying shared build lists. Optionally measure the read-only retail archive.
#>
param(
    [switch]$RealInventory,
    [switch]$UpdateCoverage,
    [string]$Data = "E:\SteamLibrary\steamapps\common\Skyrim\Data",
    [string]$Python = "",
    [string]$Clang = "C:\Program Files\LLVM\bin\clang.exe",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
if (-not $Python) {
    $candidate = Join-Path $root ".venv\Scripts\python.exe"
    $Python = if (Test-Path $candidate) { $candidate } else { "python" }
}
if (-not (Test-Path $Clang)) { throw "clang not found: $Clang" }
if (-not $OutDir) { $OutDir = Join-Path $root "build_host\pipelines" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$tempRoot = Join-Path $env:LOCALAPPDATA "Temp\opencode"
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$env:TEMP = $tempRoot
$env:TMP = $tempRoot
$env:PYTHONDONTWRITEBYTECODE = "1"

$commonSources = @(
    "source/d3d9_pipeline_match.c",
    "source/d3d9_pipeline_registry.c",
    "source/d3d9_tev.c",
    "source/d3d9_menu_light.c"
)
$flags = @("-std=gnu11", "-O1", "-g", "-Isource", "-Wall", "-Wextra", "-Werror",
           "-Wno-deprecated-declarations")

$smoke = Join-Path $OutDir "pipeline_registry_smoke.exe"
Write-Host "BUILD pipeline_registry_smoke"
& $Clang @flags @commonSources "tests/pipeline_registry_smoke.c" -o $smoke
if ($LASTEXITCODE -ne 0) { throw "pipeline_registry_smoke build failed" }
& $smoke
if ($LASTEXITCODE -ne 0) { throw "pipeline_registry_smoke failed" }

Write-Host "CHECK generated registry"
& $Python "tools/pipelines/generate_registry.py" --check
if ($LASTEXITCODE -ne 0) { throw "generated registry is out of date" }

Write-Host "PYTHON pipeline data tests"
& $Python -m unittest tests.pipeline_test -v
if ($LASTEXITCODE -ne 0) { throw "pipeline Python tests failed" }

if ($RealInventory) {
    $real = Join-Path $OutDir "pipeline_real.exe"
    Write-Host "BUILD pipeline_real"
    & $Clang @flags @commonSources "tests/pipeline_real.c" -o $real
    if ($LASTEXITCODE -ne 0) { throw "pipeline_real build failed" }
    $coverage = if ($UpdateCoverage) {
        Join-Path $root "tools/pipelines/coverage_base.json"
    } else {
        Join-Path $OutDir "coverage_real.json"
    }
    Write-Host "MEASURE real base archive"
    & $Python "tools/pipelines/inventory.py" measure --data $Data --exe $real --output $coverage
    if ($LASTEXITCODE -ne 0) { throw "real inventory measurement failed" }
}

Write-Host "pipeline registry tests passed"
