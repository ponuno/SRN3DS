#requires -Version 5.1
<#
.SYNOPSIS
  Repeatable ARM code-size benchmark for the staged x86 lifter.
.DESCRIPTION
  Slices a fixed number of functions from a real PE, lifts the same slice once
  per variant, instruments it exactly like the diagnostic builds, builds it with
  devkitARM and reports the resulting ARM .text bytes and bytes per x86
  instruction.

  Slice and lift are cached per variant; -ForceLift re-lifts. Each variant uses
  `tools/build.ps1 -Isolated cg` (target Skyrim_N3DS_tesv_cg), so a benchmark
  never touches the shared Skyrim_N3DS_tesv artifacts.

  Variants are "name=VAR=value;VAR=value" strings. The default compares eager
  and lazy flags.
.PARAMETER Variant
  One or more variant specifications. Default: eager, lazy.
.PARAMETER Opt
  Optimisation flag (default -O2, matching the recorded baseline).
.PARAMETER SkipBuild
  Only lift/instrument and re-report cached sizes.
.PARAMETER Diff
  Also run tools/codegen_diff.py (eager vs lazy differential host test).
.PARAMETER Regression
  Also lift the growth-loop slice at -RegressionSlice to prove the lifter does
  not crash on it.
.EXAMPLE
  powershell -File tools/codegen_bench.ps1
.EXAMPLE
  powershell -File tools/codegen_bench.ps1 -Variant "eager=LIFT_LAZY_FLAGS=0;LIFT_SPILL=0|selective=LIFT_LAZY_FLAGS=1;LIFT_SPILL=1"
#>
param(
    [string]$Exe = "E:\skyr\TESV.exe",
    [string]$Functions = "E:\Skyrim_N3DS_build\analysis\tesv\functions.json",
    [string]$WorkRoot = "E:\Skyrim_N3DS_build\cgbench",
    [int]$MaxFunctions = 2000,
    [int]$MaxInsns = 400000,
    [string[]]$Variant = @("eager=LIFT_LAZY_FLAGS=0;LIFT_SPILL=0;LIFT_GLOBALREGS=0",
                           "lazy=LIFT_LAZY_FLAGS=1;LIFT_SPILL=0;LIFT_GLOBALREGS=0",
                           "selective=LIFT_LAZY_FLAGS=1;LIFT_SPILL=1;LIFT_GLOBALREGS=0",
                           "global=LIFT_LAZY_FLAGS=1;LIFT_SPILL=1;LIFT_GLOBALREGS=1"),
    [string]$Opt = "-O2",
    [ValidateSet("objects", "link")]
    [string]$Measure = "objects",
    [string[]]$ExtraLiftEnv = @(),
    [switch]$RebuildSlice,
    [switch]$ForceLift,
    [switch]$SkipBuild,
    [switch]$Diff,
    [switch]$Regression,
    [string]$RegressionSlice = "E:\Skyrim_N3DS_build\analysis\growth11\iter01\slice_functions.json"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "python venv not found: $py" }
$size = "C:\devkitPro\devkitARM\bin\arm-none-eabi-size.exe"
if (-not (Test-Path $size)) { throw "arm-none-eabi-size not found: $size" }

$sliceDir = Join-Path $WorkRoot "slice"
$sliceJson = Join-Path $sliceDir "slice_functions.json"
$reportJson = Join-Path $sliceDir "slice_report.json"
New-Item -ItemType Directory -Force -Path $WorkRoot, $sliceDir | Out-Null

function Write-Step([string]$text) {
    Write-Host ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $text)
}

if ($RebuildSlice -or -not (Test-Path $sliceJson)) {
    Write-Step "slice: $MaxFunctions functions / $MaxInsns instructions -> $sliceDir"
    & $py (Join-Path $root "tools/slice_pe.py") $Exe $Functions --root entry `
        --max-functions $MaxFunctions --max-insns $MaxInsns --out $sliceDir --emit --allow-open
    if ($LASTEXITCODE -ne 0) { throw "slice_pe.py failed" }
} else {
    Write-Step "slice: reusing $sliceJson"
}
$report = Get-Content $reportJson -Raw | ConvertFrom-Json
$nfuncs = [int]$report.admitted_functions
$ninsns = [int]$report.x86_instructions

$rows = @()
$Variant = @($Variant | ForEach-Object { $_ -split '\|' } | Where-Object { $_ })
foreach ($spec in $Variant) {
    $parts = $spec -split "=", 2
    $name = $parts[0]
    $assignments = @()
    if ($parts.Count -gt 1 -and $parts[1]) { $assignments = $parts[1] -split ";" }
    $vdir = Join-Path $WorkRoot ("v_" + $name)
    $gen = Join-Path $vdir "gen"
    $elfCopy = Join-Path $vdir "codegen.elf"

    if ($ForceLift -or -not (Test-Path (Join-Path $gen "tesv_table.c"))) {
        Remove-Item -Recurse -Force $gen -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $gen | Out-Null
        Write-Step "lift [$name]: $($assignments -join ' ')"
        $saved = @{}
        foreach ($envName in $ExtraLiftEnv) {
            $kv = $envName -split "=", 2
            if ($kv.Count -eq 2) { $saved[$kv[0]] = [Environment]::GetEnvironmentVariable($kv[0]); Set-Item "env:$($kv[0])" $kv[1] }
        }
        foreach ($a in $assignments) {
            $kv = $a -split "=", 2
            if ($kv.Count -eq 2) { $saved[$kv[0]] = [Environment]::GetEnvironmentVariable($kv[0]); Set-Item "env:$($kv[0])" $kv[1] }
        }
        try {
            & $py (Join-Path $root "staged/lift.py") $Exe $sliceJson $gen tesv 400000
            if ($LASTEXITCODE -ne 0) { throw "lift.py failed for $name" }
            & $py (Join-Path $root "tools/instrument_generated.py") $gen
            if ($LASTEXITCODE -ne 0) { throw "instrument_generated.py failed for $name" }
        } finally {
            foreach ($k in $saved.Keys) {
                if ($null -eq $saved[$k]) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
                else { Set-Item "env:$k" $saved[$k] }
            }
        }
    } else {
        Write-Step "lift [$name]: cached $gen"
    }

    $textBytes = $null
    if (-not $SkipBuild) {
        if ($Measure -eq "objects") {
            # Direct object measurement: no shared runtime sources, so an
            # unrelated edit in source/ cannot break or skew the benchmark.
            $objdir = Join-Path $vdir "obj"
            Remove-Item -Recurse -Force $objdir -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force -Path $objdir | Out-Null
            Write-Step "build [$name]: arm-none-eabi-gcc $Opt (objects)"
            $gcc = "C:\devkitPro\devkitARM\bin\arm-none-eabi-gcc.exe"
            $inc = @("-I$root\source", "-I$root\staged", "-I$gen", "-IC:/devkitPro/libctru/include")
            $cf = @("-g", $Opt, "-Wall", "-Wextra", "-std=gnu11", "-mword-relocations", "-ffunction-sections",
                    "-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft", "-D__3DS__",
                    "-DN3DS_LIFT_TEST", "-fwrapv", "-fno-strict-aliasing", "-Wno-misleading-indentation") + $inc
            Push-Location $objdir
            try {
                & $gcc @cf -c (Get-ChildItem $gen -Filter "*.c" | ForEach-Object { $_.FullName })
                if ($LASTEXITCODE -ne 0) { throw "arm-none-eabi-gcc failed for $name" }
            } finally { Pop-Location }
            $textBytes = [int64]0
            foreach ($o in Get-ChildItem $objdir -Filter "*.o") {
                $line = (& $size $o.FullName | Select-Object -Skip 1 | Select-Object -First 1).Trim()
                $textBytes += [int64](($line -split '\s+')[0])
            }
        } else {
            Write-Step "build [$name]: $Opt (linked, -Isolated cg)"
            $t0 = Get-Date
            & powershell -NoProfile -File (Join-Path $root "tools/build.ps1") -Mode tesv `
                -TesvGen $gen -Isolated cg -Opt $Opt | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed for $name" }
            Write-Step ("build [$name]: {0:n0}s" -f ((Get-Date) - $t0).TotalSeconds)
            $elf = Join-Path $root "Skyrim_N3DS_tesv_cg.elf"
            if (Test-Path $elf) {
                Copy-Item $elf $elfCopy -Force
                $line = (& $size $elfCopy | Select-Object -Skip 1 | Select-Object -First 1).Trim()
                $textBytes = [int64](($line -split '\s+')[0])
            }
        }
    } else {
        $elf = Join-Path $root "Skyrim_N3DS_tesv_cg.elf"
        if (Test-Path $elf) {
            $line = (& $size $elf | Select-Object -Skip 1 | Select-Object -First 1).Trim()
            $textBytes = [int64](($line -split '\s+')[0])
        }
    }
    $rows += [pscustomobject]@{
        Variant  = $name
        Env      = ($assignments -join ";")
        Functions = $nfuncs
        x86insns = $ninsns
        ArmText  = $textBytes
        BperInsn = if ($textBytes) { [math]::Round($textBytes / $ninsns, 2) } else { $null }
    }
}

$rows | Format-Table -AutoSize
if ($rows | Where-Object { $_.ArmText }) {
    $base = ($rows | Select-Object -First 1)
    foreach ($r in $rows) {
        if ($r.ArmText -and $base.ArmText) {
            $pct = [math]::Round(100.0 * ($r.ArmText - $base.ArmText) / $base.ArmText, 1)
            Write-Host ("{0}: {1} B vs {2}: {3} B ({4}%)" -f $r.Variant, $r.ArmText, $base.Variant, $base.ArmText, $pct)
        }
    }
}

if ($Diff) {
    Write-Step "differential test: eager vs lazy"
    & $py (Join-Path $root "tools/codegen_diff.py") (Join-Path $WorkRoot "diff")
    if ($LASTEXITCODE -ne 0) { throw "codegen_diff.py failed" }
}

if ($Regression) {
    Write-Step "regression: lift $RegressionSlice"
    if (-not (Test-Path $RegressionSlice)) { throw "regression slice not found: $RegressionSlice" }
    & $py (Join-Path $root "tools/lift_regression.py") $RegressionSlice (Join-Path $WorkRoot "regress")
    if ($LASTEXITCODE -ne 0) { throw "lift_regression.py failed" }
}
exit 0
