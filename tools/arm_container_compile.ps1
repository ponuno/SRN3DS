#requires -Version 5.1
<#
.SYNOPSIS
  Compile the integrated IR container and lift-runtime objects for ARM11.
.DESCRIPTION
  Uses devkitARM directly (no link, because the generated TESV tables are a
  separate build input). Starts up to 16 compiler processes in parallel.
#>
param(
    [string]$OutDir = "build_arm",
    [int]$Jobs = 16,
    [string]$Gcc = "C:\devkitPro\devkitARM\bin\arm-none-eabi-gcc.exe",
    [string]$Size = "C:\devkitPro\devkitARM\bin\arm-none-eabi-size.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not (Test-Path $Gcc)) { throw "arm-none-eabi-gcc not found: $Gcc" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$files = @(
    "source/ir_container.c",
    "source/n3ds_lift_runtime.c",
    "source/ir_core.c",
    "source/ir_decode.c",
    "source/ir_exec.c",
    "source/guest.c",
    "source/guest_swap.c",
    "third_party/lz4/lz4.c",
    "source/guest_heap.c",
    "source/pe_image.c",
    "source/pe_imports.c",
    "source/guest_process.c",
    "source/n3ds_boot.c",
    "source/n3ds_pilot.c",
    "source/rt_prof.c",
    "source/win32_shims.c",
    "source/win32_sync.c",
    "source/win32_user.c",
    "source/win32_steam.c",
    "source/d3d9_capture.c",
    "staged/fpu.c"
)

$cf = @("-O2", "-g", "-std=gnu11", "-march=armv6k", "-mtune=mpcore",
        "-mfloat-abi=hard", "-mtp=soft", "-D__3DS__", "-fwrapv",
        "-fno-strict-aliasing", "-Wall", "-Wextra",
        "-Wno-misleading-indentation", "-Wno-unused-parameter",
        "-Isource", "-Ithird_party/lz4", "-Istaged", "-IC:/devkitPro/libctru/include")

$started = Get-Date
$jobsList = @()
$pairs = @()
foreach ($f in $files) {
    $obj = Join-Path $OutDir ((Split-Path $f -Leaf) + ".o")
    Remove-Item $obj -ErrorAction SilentlyContinue
    $pairs += ,@($f, $obj)
    $argList = $cf + @("-c", $f, "-o", $obj)
    $jobsList += Start-Job -ScriptBlock {
        param($gcc, $argsList, $dir)
        Set-Location $dir
        & $gcc @argsList 2>&1
        exit $LASTEXITCODE
    } -ArgumentList $Gcc, $argList, $root
    while (($jobsList | Where-Object { $_.State -eq "Running" }).Count -ge $Jobs) {
        Start-Sleep -Milliseconds 40
    }
}
$jobsList | Wait-Job | Out-Null
foreach ($j in $jobsList) {
    $out = Receive-Job $j
    if ($out) { $out | ForEach-Object { Write-Host $_ } }
    Remove-Job $j
}
$failed = @()
foreach ($p in $pairs) {
    if (-not (Test-Path $p[1])) { $failed += $p[0] }
}
$elapsed = (Get-Date) - $started
Write-Host ("ARM objects: {0} requested, {1} seconds, {2} failed" -f
            $files.Count, [math]::Round($elapsed.TotalSeconds, 2), $failed.Count)
foreach ($f in $failed) { Write-Host ("FAILED {0}" -f $f) }

$objs = Get-ChildItem $OutDir -Filter "*.o" -ErrorAction SilentlyContinue
$totalText = 0
foreach ($o in $objs) {
    $line = & $Size $o.FullName | Select-Object -Index 1
    if (-not $line) { continue }
    $parts = $line -split "\s+"
    $text = [int]$parts[1]
    $bss = [int]$parts[3]
    $totalText += $text
    Write-Host ("{0,-34} text {1,8} bss {2,7}" -f $o.Name, $text, $bss)
}
Write-Host ("TOTAL text {0} bytes ({1:N1} KiB)" -f $totalText, ($totalText / 1024.0))
if ($failed.Count -gt 0) { exit 1 }
