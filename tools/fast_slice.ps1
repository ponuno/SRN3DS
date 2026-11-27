# Fast host-slice loop.
#
# One command for the fix/verify cycle:
#   tools\fast_slice.ps1                 # reuse the last build, run to the first fault
#   tools\fast_slice.ps1 -Reslice        # rebuild the slice first
#   tools\fast_slice.ps1 -HangMs 3000    # shorter post-fault watchdog
#
# It prints only what matters: the first guest fault, the preceding progress
# markers, and the wall time. The whole log stays in <OutDir>\last_run.log.
# Never use the `| Select-Object -First N` pipeline on the slice itself: it
# closes the pipe and kills the process early.

param(
    [switch]$Reslice,
    [string]$OutDir = 'E:\slice_fast',
    [string]$Irc = 'E:\census_ir2\ir_container\tesv.irc',
    [string]$Gen = 'E:\Skyrim_N3DS_build\live_arm_check2\gen',
    [string]$Exe = 'E:\harness_skyrim\TESV.exe',
    [string]$Root = 'E:\harness_skyrim',
    [uint64]$Steps = 3000000000,
    [uint64]$Budget = 167772160,
    [int]$HangMs = 8000,
    [int]$TimeoutSec = 900,
    [switch]$StopAfterDraw,
    [switch]$NoShaderSkipProbe
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Find-Slice {
    if (-not (Test-Path -LiteralPath $OutDir)) { return $null }
    $hit = Get-ChildItem -LiteralPath $OutDir -Recurse -Filter tesv_slice.exe -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    return $null
}

if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

$slice = Find-Slice
if ($Reslice -or -not $slice) {
    Write-Host "[fast] building tesv_slice ..."
    $env:HOST_CLANG_JOBS = if ($env:HOST_CLANG_JOBS) { $env:HOST_CLANG_JOBS } else { '8' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\run_host_tests.ps1') `
        -TesvIrc $Irc -TesvSliceGen $Gen -TesvExe $Exe -AllowDiagnosticFault `
        -OutDir $OutDir -Only tesv_slice -SkipPython -TestTimeoutSec 900 |
        Select-String -Pattern 'BUILD|Summary|Diagnostic|HOST' | ForEach-Object { $_.Line }
    $slice = Find-Slice
}
if (-not $slice) { throw "no tesv_slice.exe under $OutDir" }

# A slice built without the locally generated D3DX body table (pilot_out_mult,
# git-ignored and derived from D3DX9_42.dll) links the real hotpatch bodies but
# cannot dispatch them, so the first guest matrix call faults at a bare body
# address such as 0802fa80 (D3DXMatrixMultiply).  Warn before the run instead
# of reporting that as a new guest fault.
$pilotTable = Join-Path $repo 'pilot_out_mult\D3DXMatrixMultiply\gen\pilot_table.c'
if (-not (Test-Path -LiteralPath $pilotTable)) {
    Write-Host "[fast] WARNING: $pilotTable is absent; the slice has no D3DX body table"
    Write-Host "[fast]          and a guest d3dx9_42.dll call (e.g. 0802fa80 D3DXMatrixMultiply) will fault"
}

$log = Join-Path $OutDir 'last_run.log'
$work = Split-Path -Parent $slice
$env:N3DS_IRC_PATH = $Irc
$env:TESV_MEMPROBE = '1'
$env:TESV_D3D9_DEPTH_PAIR_PROBE = '1'
$env:TESV_D3D9_HDR_LDR_FALLBACK = '1'
if (-not $NoShaderSkipProbe) { $env:TESV_D3D9_SHADER_SKIP_PROBE = '1' } else { Remove-Item Env:TESV_D3D9_SHADER_SKIP_PROBE -ErrorAction SilentlyContinue }
$env:TESV_D3D9_BUDGET_MB = '92'
$env:TESV_D3D9_CAPS_PROBE = '1'
$env:TESV_D3D9_SM3_GATE_PROBE = '1'
$env:TESV_MENU_TRACE = '1'
$env:TESV_FAULT_REGS = '1'
$env:N3DS_PC_RING = '1'
$env:TESV_HANG_WATCHDOG_MS = "$HangMs"
$env:TEMP = Join-Path $OutDir 'tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null

Write-Host "[fast] $slice"
Write-Host "[fast] irc=$Irc steps=$Steps budget=$Budget hang=${HangMs}ms"
if ($env:TESV_INI_OVERLAY) { Write-Host "[fast] ini overlay: $env:TESV_INI_OVERLAY" }
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $slice -WorkingDirectory $work `
    -ArgumentList @($Exe, $Root, "$Steps", "$Budget") `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
# Kill as soon as the first guest fault is in the log: the process usually
# stays alive on other threads and would otherwise sit until the timeout.
$faultSeen = $null
while (-not $p.HasExited) {
    if ($sw.Elapsed.TotalSeconds -ge $TimeoutSec) {
        Write-Host "[fast] TIMEOUT after ${TimeoutSec}s, killing pid $($p.Id)"
        try { $p.Kill() } catch {}
        break
    }
    Start-Sleep -Milliseconds 400
    if (-not $faultSeen) {
        $hit = $false
        foreach ($f in @($log, "$log.err")) {
            if ((Test-Path $f) -and (Select-String -Path $f -Pattern 'Guest fault|unmapped addr|no terminator|unknown function|block has no' -Quiet)) { $hit = $true; break }
        }
        if ($hit) { $faultSeen = Get-Date }
    }
    if ($StopAfterDraw) {
        $draw = $false
        foreach ($f in @($log, "$log.err")) {
            if ((Test-Path $f) -and (Select-String -Path $f -Pattern 'DrawPrimitive|DrawIndexed' -Quiet)) { $draw = $true; break }
        }
        if ($draw) { Write-Host "[fast] draw seen, stopping"; try { $p.Kill() } catch {}; break }
    }
    if ($faultSeen -and ((Get-Date) - $faultSeen).TotalSeconds -ge 2) {
        Write-Host "[fast] fault seen, stopping after 2s grace"
        try { $p.Kill() } catch {}
        break
    }
}
try { $p.WaitForExit(5000) | Out-Null } catch {}
$sw.Stop()
Get-Content "$log.err" -ErrorAction SilentlyContinue | Add-Content $log

$lines = Get-Content $log -ErrorAction SilentlyContinue
if (-not $lines) { $lines = @() }

function Show-Around([string]$pattern, [int]$before = 0, [int]$max = 3) {
    $idx = @()
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match $pattern) { $idx += $i; if ($idx.Count -ge $max) { break } }
    }
    if (-not $idx) { return }
    foreach ($i in $idx) {
        $from = [Math]::Max(0, $i - $before)
        $lines[$from..$i] | ForEach-Object { Write-Host "  $_" }
        Write-Host "  ---"
    }
}

Write-Host ("[fast] elapsed {0:N1}s, log {1} ({2} lines)" -f $sw.Elapsed.TotalSeconds, $log, $lines.Count)
Write-Host "[fast] first guest fault:"
Show-Around 'Guest fault|unmapped addr|no terminator|unknown function|block has no' -before 2 -max 2
if (-not (Select-String -Path $log -Pattern 'Guest fault|unmapped addr|no terminator|unknown function|block has no' -Quiet)) {
    Write-Host "  (none)"
}
Write-Host "[fast] progress markers:"
Show-Around '\[menu\] enter|StartMenu|876f60|a5ed20|8779d0|Draw' -max 4
Write-Host "[fast] mem:"
Show-Around 'mem tag=(fault|exit|init)' -max 2
