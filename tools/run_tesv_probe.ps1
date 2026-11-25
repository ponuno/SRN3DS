#requires -Version 5.1
<#
.SYNOPSIS
  Run the deepest lifted TESV slice on the host with the memory probe enabled.
.DESCRIPTION
  Builds (or reuses) the host diagnostic binary for one generated slice, then runs
  it with a configurable guest budget and the measurement-only memory probe. The
  probe reports a 512 MiB commit limit so the engine's minimum-configuration check
  can be passed; every allocation is counted and printed.

  The probe is a measurement, not a shippable value: the normal build reports the
  memory the port can actually back.
.PARAMETER TesvGen
  Generated slice directory (contains tesv_table.c). Defaults to the deepest one
  under E:\Skyrim_N3DS_build\analysis\growth*\gen*.
.PARAMETER BudgetMB
  Guest memory budget in MiB for mappings and backing. With lazy VA the current
  startup footprint is about 33 MiB plus touched pages; the probe is still
  required to pass the engine's separate 512 MiB minimum-configuration check.
.PARAMETER NoProbe
  Run without the probe (honest 36 MiB machine: the game refuses to start).
#>
param(
    [string]$TesvGen = "",
    [int]$BudgetMB = 160,
    [switch]$NoProbe,
    [string]$OutDir = "E:\Skyrim_N3DS_build\host"
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $TesvGen) {
    $cand = Get-ChildItem "E:\Skyrim_N3DS_build\analysis" -Directory -Filter "growth*" -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter "gen*" -ErrorAction SilentlyContinue } |
        Where-Object { Test-Path (Join-Path $_.FullName "tesv_table.c") } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $cand) { throw "no generated slice with tesv_table.c found; pass -TesvGen" }
    $TesvGen = $cand.FullName
}
Write-Host "slice: $TesvGen"

& powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
    -Only tesv_slice -TesvSliceGen $TesvGen -AllowDiagnosticFault -OutDir $OutDir 2>&1 |
    Select-String -Pattern "COMPILE FAIL|HOST DIAG|HOST FAIL|Summary" | ForEach-Object { Write-Host "  $_" }

$run = Get-ChildItem $OutDir -Directory -Filter "run_tesv_slice_*" |
    Where-Object { Test-Path (Join-Path $_.FullName "tesv_slice.exe") } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $run) { throw "no run directory with tesv_slice.exe" }
$exe = Join-Path $run.FullName "tesv_slice.exe"
$args = @("E:\skyr\TESV.exe", ".", "200000", "$($BudgetMB * 1MB)")
if (-not $NoProbe) {
    $env:TESV_MEMPROBE = "1"
    Write-Host "probe: ON (measurement only), budget ${BudgetMB} MiB"
} else {
    Remove-Item Env:\TESV_MEMPROBE -ErrorAction SilentlyContinue
    Write-Host "probe: OFF, budget ${BudgetMB} MiB"
}
Push-Location $run.FullName
try {
    $log = Join-Path $run.FullName "probe_run.log"
    & $exe @args *>&1 | Tee-Object -FilePath $log | ForEach-Object { Write-Host "  $_" }
    Write-Host "exit=$LASTEXITCODE"
} finally {
    Pop-Location
}
Write-Host "run dir: $($run.FullName)"
