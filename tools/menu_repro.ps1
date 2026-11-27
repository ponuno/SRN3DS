#requires -Version 5.1
<#
.SYNOPSIS
  Fast deterministic repro for the menu crash at 0x60aa70 (last_call=0x60aa70,
  addr=0x62783049).

.DESCRIPTION
  Builds and runs tests/menu_crash_repro.c through the real host-test runner
  (real IR container tier, real retail bytes of 0x60aa70 and 0x604750).  The
  test asserts the exact fault register/address, not just "it did not crash".

  -Regen  rebuild tests/menu_repro_irc.h with tools/menu_repro_pack.py first
  -Full   additionally run the newest tesv_slice.exe and print only fault lines

.PARAMETER SliceRoot
  Where to look for a built tesv_slice.exe when -Full is given.
  Default: E:\Skyrim_N3DS_build (or $env:TESV_SLICE_ROOT).
#>
param(
    [switch]$Regen,
    [switch]$Full,
    [string]$SliceRoot = "",
    [string]$OutDir = "",
    [int]$SliceTimeoutSec = 240
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $OutDir) { $OutDir = Join-Path $env:TEMP "menu_repro_build" }

# The generated lift runtime lives in the main tree's `staged/` directory,
# which is not a tracked file set.  Host tests need it; link it read-only.
if (-not (Test-Path (Join-Path $root "staged\fpu.c"))) {
    $candidates = @(
        (Join-Path (Split-Path -Parent $root) "Skyrim_N3DS\staged"),
        "C:\Users\vasya\Documents\Skyrim_N3DS\staged"
    ) | Where-Object { Test-Path (Join-Path $_ "fpu.c") }
    if (-not $candidates) {
        throw "staged/fpu.c is missing and no source tree was found; set up staged/ first"
    }
    New-Item -ItemType Junction -Path (Join-Path $root "staged") -Target $candidates[0] | Out-Null
    Write-Host "linked staged/ -> $($candidates[0])"
}

$python = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) {
    $python = "C:\Users\vasya\Documents\Skyrim_N3DS\.venv\Scripts\python.exe"
}
if (-not (Test-Path $python)) { $python = "python" }

$total = [System.Diagnostics.Stopwatch]::StartNew()

if ($Regen) {
    Write-Host "== regenerating mini IR container =="
    & $python (Join-Path $root "tools\menu_repro_pack.py")
    if ($LASTEXITCODE -ne 0) { throw "menu_repro_pack.py failed" }
}

Write-Host "== fast repro: menu_crash_repro =="
$fast = [System.Diagnostics.Stopwatch]::StartNew()
$runner = Join-Path $root "tools\run_host_tests.ps1"
$logLines = @(& $runner -Only menu_crash_repro -SkipPython -OutDir $OutDir *>&1)
$runnerExit = $LASTEXITCODE
$log = ($logLines | ForEach-Object { "$_" }) -join "`n"
$logLines | Where-Object {
    "$_" -match "HOST PASS|HOST FAIL|COMPILE FAIL|FAULT |TRAP |error:"
} | ForEach-Object { Write-Host $_ }
$fast.Stop()

$passed = ($runnerExit -eq 0) -and $log.Contains("HOST PASS: menu_crash_repro")
if (-not $passed) {
    Write-Host "--- full runner output (tail) ---"
    ($logLines | Select-Object -Last 40) | ForEach-Object { Write-Host $_ }
    throw "menu_crash_repro FAILED (runner exit $runnerExit)"
}

Write-Host ("PASS menu_crash_repro in {0:N1}s (total {1:N1}s)" -f `
    $fast.Elapsed.TotalSeconds, $total.Elapsed.TotalSeconds)

if (-not $Full) {
    Write-Host "(-Full runs the slice probe as well)"
    exit 0
}

# ------------------------------------------------------------------ -Full
$slice = $null
if (-not $SliceRoot) {
    $SliceRoot = if ($env:TESV_SLICE_ROOT) { $env:TESV_SLICE_ROOT } else { "E:\Skyrim_N3DS_build" }
}
if (Test-Path $SliceRoot) {
    $slice = Get-ChildItem -Path $SliceRoot -Recurse -Depth 4 -Filter "tesv_slice.exe" `
        -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
if (-not $slice) {
    Write-Host "== -Full: no tesv_slice.exe under '$SliceRoot' (skipped, not a failure) =="
    exit 0
}

Write-Host "== -Full: $($slice.FullName) =="
$work = $slice.Directory.FullName
$exe = $slice.FullName
$argsList = @("E:\skyr\TESV.exe", ".", "200000", "167772160")

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.WorkingDirectory = $work
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.Arguments = ($argsList | ForEach-Object { '"' + $_ + '"' }) -join ' '
$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
$sliceWatch = [System.Diagnostics.Stopwatch]::StartNew()
$null = $proc.Start()
$outTask = $proc.StandardOutput.ReadToEndAsync()
$errTask = $proc.StandardError.ReadToEndAsync()
if (-not $proc.WaitForExit($SliceTimeoutSec * 1000)) {
    try { $proc.Kill() } catch { }
    $proc.WaitForExit(5000) | Out-Null
    Write-Host "slice probe timed out after ${SliceTimeoutSec}s"
}
$sliceWatch.Stop()
$text = ($outTask.Result + "`n" + $errTask.Result)
$text -split "`r?`n" | Where-Object {
    $_ -match "Guest fault at|\[guest\] unmapped|last_call=|first fault|messagebox|ExitProcess"
} | Select-Object -First 40 | ForEach-Object { Write-Host $_ }

Write-Host ("-Full slice finished in {0:N1}s; total {1:N1}s" -f `
    $sliceWatch.Elapsed.TotalSeconds, $total.Elapsed.TotalSeconds)
exit 0
