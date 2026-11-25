#requires -Version 5.1
<#
.SYNOPSIS
  Grow the TESV startup slice from observed guest faults.
.DESCRIPTION
  Repeats: slice_pe -> lift.py -> instrument -> host run. When the run reports
  an unlifted indirect target, it becomes an extra root. Stops on an unresolved
  import, a lifter-rejected function, a repeated fault, or the iteration limit.
  Writes a JSON journal. Does not modify the game or publish generated code.
#>
param(
    [string]$Exe = "E:\skyr\TESV.exe",
    [string]$Functions = "E:\Skyrim_N3DS_build\analysis\tesv\functions.json",
    [string]$WorkRoot = "E:\Skyrim_N3DS_build\analysis\growth",
    [string]$HostOut = "E:\Skyrim_N3DS_build\host",
    [string[]]$TlsRoots = @("0xf794ab", "0xf5b303"),
    [string]$ExtraRoots = "",
    [string]$ExtraRootFile = "",
    [int]$MaxIterations = 12,
    [int]$StartFunctions = 70,
    [int]$StepFunctions = 40,
    [int]$StartInsns = 45000,
    [int]$StepInsns = 10000,
    [switch]$FinalCia
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$hardFail = $false
$python = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) { throw "venv python missing: $python" }
if (-not (Test-Path $Exe)) { throw "TESV not found: $Exe" }
if (-not (Test-Path $Functions)) { throw "functions.json not found: $Functions" }
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null

$roots = New-Object System.Collections.Generic.List[string]
foreach ($r in $TlsRoots) { $roots.Add($r.ToLower()) }
if ($ExtraRoots) {
    # Accept both "0x200", 512 (PowerShell parses unquoted 0x.. lists as integers)
    # and a comma-separated string. Normalize everything to 0x lowercase hex.
    foreach ($r in ($ExtraRoots -split ',')) {
        $v = "$r".Trim().ToLower()
        if (-not $v) { continue }
        if ($v -notmatch '^0x') {
            try { $v = '0x' + ([Convert]::ToUInt32($v, 10)).ToString('x') } catch { }
        }
        if ($v -and -not $roots.Contains($v)) { $roots.Add($v) }
    }
}
foreach ($r in $roots) {
    if ($r -notmatch '^0x[0-9a-f]{1,8}$') { throw "invalid root '$r' (expected 0x hex address)" }
}
if ($ExtraRootFile) {
    foreach ($line in Get-Content $ExtraRootFile) {
        $v = $line.Trim().ToLower()
        if (-not $v -or $v.StartsWith('#')) { continue }
        if ($v -notmatch '^0x[0-9a-f]{1,8}$') { throw "invalid root '$v' in $ExtraRootFile" }
        if (-not $roots.Contains($v)) { $roots.Add($v) }
    }
}
$journal = New-Object System.Collections.Generic.List[object]
$stop = ""
$lastGen = ""

for ($i = 1; $i -le $MaxIterations; $i++) {
    $maxf = $StartFunctions + ($i - 1) * $StepFunctions
    $maxi = $StartInsns + ($i - 1) * $StepInsns
    $sliceDir = Join-Path $WorkRoot ("iter{0:D2}" -f $i)
    $genDir = Join-Path $WorkRoot ("gen{0:D2}" -f $i)
    Remove-Item -Recurse -Force $sliceDir, $genDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $sliceDir | Out-Null

    Write-Host "=== iteration ${i}: roots=$($roots.Count) maxf=$maxf maxinsns=$maxi ==="
    $sliceArgs = @("tools/slice_pe.py", $Exe, $Functions, "--root", "entry",
        "--max-functions", "$maxf", "--max-insns", "$maxi",
        "--out", $sliceDir, "--emit", "--allow-open")
    foreach ($r in $roots) { $sliceArgs += @("--extra-root", $r) }
    & $python @sliceArgs 2>&1 | Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $sliceDir "slice_functions.json"))) {
        $stop = "slice generation failed"; $hardFail = $true; break
    }

    & $python staged/lift.py $Exe (Join-Path $sliceDir "slice_functions.json") $genDir tesv 400000 2>&1 |
        Select-Object -Last 2 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { $stop = "lift failed"; $hardFail = $true; break }
    & $python tools/instrument_generated.py $genDir 2>&1 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { $stop = "instrumentation failed"; $hardFail = $true; break }

    $rejected = @()
    $rejPath = Join-Path $genDir "tesv_rejected.json"
    if (Test-Path $rejPath) {
        try {
            $rejObj = Get-Content $rejPath -Raw | ConvertFrom-Json
            $rejected = @($rejObj.PSObject.Properties.Name | ForEach-Object {
                try { [Convert]::ToUInt32($_, 16) } catch { }
            })
        } catch { }
    }

    $runOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
        -Only tesv_slice -TesvSliceGen $genDir -OutDir $HostOut -AllowDiagnosticFault -TesvExe $Exe 2>&1 | Out-String
    $runCode = $LASTEXITCODE
    $entry = $null
    $m = [regex]::Match($runOutput, 'first fault: Guest fault at ([0-9a-fA-F]+):\s*(.+)')
    if ($m.Success) {
        $addr = "0x" + $m.Groups[1].Value.ToLower()
        $reason = $m.Groups[2].Value.Trim()
        $entry = [pscustomobject]@{
            iteration = $i; roots = $roots.Count; max_functions = $maxf; max_insns = $maxi
            slice_dir = $sliceDir; gen = $genDir; fault_addr = $addr; reason = $reason
        }
        $journal.Add($entry) | Out-Null
        Write-Host "  fault $addr : $reason"
        if ($reason -match 'unresolved (ordinal )?import') { $stop = "needs import: $reason"; $lastGen = $genDir; break }
        if ($reason -match 'unknown function/import|unknown indirect function') {
            $addrInt = 0
            try { $addrInt = [Convert]::ToUInt32($addr.Substring(2), 16) } catch { }
            if ($rejected -contains $addrInt) { $stop = "lifter rejected $addr"; $lastGen = $genDir; break }
            if ($roots.Contains($addr)) { $stop = "repeated fault at $addr"; $lastGen = $genDir; break }
            $roots.Add($addr)
            # If the target sits in a function-pointer table, pull the whole
            # verified table in now instead of walking it one fault per run.
            $tableOut = Join-Path $WorkRoot ("iter{0:d2}\table_roots.txt" -f $i)
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $tableOut) | Out-Null
            & $python tools/table_roots.py $Exe $Functions --seed $addr --out $tableOut 2>&1 |
                Select-Object -First 2 | ForEach-Object { Write-Host "  $_" }
            if (Test-Path $tableOut) {
                $added = 0
                foreach ($line in Get-Content $tableOut) {
                    $v = $line.Trim().ToLower()
                    if ($v -match '^0x[0-9a-f]{1,8}$' -and -not $roots.Contains($v)) { $roots.Add($v); ++$added }
                }
                if ($added) { Write-Host "  table roots: +$added functions from the table containing 0x$addr" }
            }
            $lastGen = $genDir
            continue
        }
        $stop = "non-unlifted fault: $reason"; $lastGen = $genDir; break
    } else {
        $gx = [regex]::Match($runOutput, '\[guest\] ExitProcess\((\d+)\) from ([0-9a-fA-F]+)')
        if ($gx.Success) {
            $code = $gx.Groups[1].Value; $from = $gx.Groups[2].Value.ToLower()
            $mb = [regex]::Match($runOutput, '\[messagebox\] (.+)')
            $msg = if ($mb.Success) { $mb.Groups[1].Value.Trim() } else { "" }
            $journal.Add([pscustomobject]@{
                iteration = $i; roots = $roots.Count; max_functions = $maxf; max_insns = $maxi
                slice_dir = $sliceDir; gen = $genDir; fault_addr = ""; reason = "guest exit $code from 0x$from"
                messagebox = $msg
            }) | Out-Null
            Write-Host "  boundary: guest ExitProcess($code) from 0x$from $(if($msg){"messagebox='$msg'"})"
            $stop = "guest exit $code from 0x$from"
            if ($msg) { $stop += " (messagebox: $msg)" }
            $lastGen = $genDir
            break
        }
        $journal.Add([pscustomobject]@{
            iteration = $i; roots = $roots.Count; max_functions = $maxf; max_insns = $maxi
            slice_dir = $sliceDir; gen = $genDir; fault_addr = ""; reason = "no fault parsed"
        }) | Out-Null
        if ($runCode -ne 0) { $stop = "host run failed (exit $runCode)"; $hardFail = $true }
        else { $stop = "no fault parsed from host run"; $hardFail = $true }
        $lastGen = $genDir; break
    }
}

if (-not $stop) { $stop = "iteration limit ($MaxIterations)"; $hardFail = $true }

$exeSha = (Get-FileHash $Exe -Algorithm SHA256).Hash.ToLower()
$funcSha = (Get-FileHash $Functions -Algorithm SHA256).Hash.ToLower()
$result = [pscustomobject]@{
    finished_at = [DateTime]::Now.ToString("o")
    exe = $Exe; exe_sha256 = $exeSha
    functions = $Functions; functions_sha256 = $funcSha
    stop_reason = $stop
    iterations = $journal.Count
    roots = @($roots)
    last_gen = $lastGen
    journal = $journal
}
$outFile = Join-Path $WorkRoot "growth_journal.json"
$result | ConvertTo-Json -Depth 6 | Set-Content $outFile -Encoding utf8
Write-Host "JOURNAL: $outFile"
Write-Host "STOP: $stop"

if ($FinalCia -and $lastGen) {
    Write-Host "Building ARM + CIA for the last slice..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Mode tesv -TesvGen $lastGen
    if ($LASTEXITCODE -eq 0) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_cia.ps1
    }
}
if ($hardFail) { exit 1 }
if ($stop) { exit 2 }
exit 0
