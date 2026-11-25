#requires -Version 5.1
<#
.SYNOPSIS
  Reproduce the measurements in docs/ARENA.md.

.DESCRIPTION
  Runs the deepest host slice with TESV_MEMPROBE=1 and:
    1. reports the plain memory summary (the 85 MiB Default Heap commit),
    2. stops at the guest fault under lldb and dumps the manager, the pool
       object, the committed chunk list and the per-page usage,
    3. repeats (2) with the Default Heap initial-commit argument forced to 0
       on the guest stack, to measure what the engine really commits.

  Nothing in source/, staged/ or the generated slice is modified. The lldb
  helper lives in tools/arena_dump.py.

.NOTES
  Host only. Device numbers are not produced by this script.
#>
param(
    [string]$Exe = "E:\skyr\TESV.exe",
    [int]$Budget = 169000000,
    [string]$RunDir = "",
    [string]$Lldb = "C:\Program Files\LLVM\bin\lldb.exe",
    [string]$OutDir = "E:\Skyrim_N3DS_build\arena"
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$dump = Join-Path $PSScriptRoot "arena_dump.py"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not $RunDir) {
    $run = Get-ChildItem "E:\Skyrim_N3DS_build\host" -Directory -Filter "run_tesv_slice_*" |
        Where-Object { Test-Path (Join-Path $_.FullName "tesv_slice.exe") } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $run) { throw "no host build; run tools/run_host_tests.ps1 -Only tesv_slice first" }
    $RunDir = $run.FullName
}
$slice = Join-Path $RunDir "tesv_slice.exe"
Write-Host "slice: $slice"

$env:TESV_MEMPROBE = "1"
Push-Location $RunDir
try {
    Write-Host "`n== 1. plain run (85 MiB Default Heap) =="
    $log1 = Join-Path $OutDir "run_85mib.log"
    & $slice $Exe . 200000 $Budget *>&1 | Tee-Object -FilePath $log1 |
        Select-String -Pattern "^\[mem\]|first fault" | ForEach-Object { Write-Host "  $_" }

    if (Test-Path $Lldb) {
        Write-Host "`n== 2. dump at the fault (85 MiB) =="
        $log2 = Join-Path $OutDir "dump_85mib.log"
        & $Lldb --batch `
            -o "breakpoint set -n rt_unimpl" `
            -o "command script import $dump" `
            -o "run $Exe . 200000 $Budget" `
            -o "arena_dump 16" `
            -o "quit" $slice *>&1 | Tee-Object -FilePath $log2 |
            Select-String -Pattern "GuestMemory|cell\[|pool\.|committed regions|sampled every|pages non-zero" |
            ForEach-Object { Write-Host "  $_" }

        Write-Host "`n== 3. same run, initial commit forced to 0 =="
        $log3 = Join-Path $OutDir "dump_lazy.log"
        & $Lldb --batch `
            -o "breakpoint set -n rt_unimpl" `
            -o "command script import $dump" `
            -o "arena_patcharg 0" `
            -o "run $Exe . 200000 $Budget" `
            -o "arena_dump 16" `
            -o "quit" $slice *>&1 | Tee-Object -FilePath $log3 |
            Select-String -Pattern "\[patch\]|GuestMemory|cell\[|committed regions|sampled every" |
            ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host "lldb not found at $Lldb; skipped the dumps"
    }
} finally {
    Pop-Location
}
Write-Host "`nlogs: $OutDir"
