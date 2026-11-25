#requires -Version 5.1
<#
.SYNOPSIS
Continue fault-driven TESV growth until WinMain/renderer/D3D9 is reached.
.DESCRIPTION
Runs bounded growth stages, checks host vs Azahar hybrid parity after each stage,
and stops on a target, unresolved import, compile failure, or repeated fault.
Intermediates stay on E:.
#>
param(
    [string]$StartWorkRoot = 'E:\Skyrim_N3DS_build\analysis\growth22',
    [string]$RootFile = 'E:\Skyrim_N3DS_build\analysis\growth_roots14_all.txt',
    [int]$StartFunctions = 54500,
    [int]$StartInsns = 7450000,
    [int]$StageIterations = 4,
    [int]$StepFunctions = 1000,
    [int]$StepInsns = 100000,
    [int]$MaxStages = 12,
    [int]$Steps = 5000000,
    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$python = Join-Path $root '.venv\Scripts\python.exe'
$grow = 'E:\Skyrim_N3DS_build\grow16_manual.ps1'
$exe = 'E:\skyr\TESV.exe'
Set-Location $root
if(-not (Test-Path $python)){ throw "venv python missing: $python" }
if(-not (Test-Path $grow)){ throw "growth worker missing: $grow" }
if(-not (Test-Path $RootFile)){ throw "root file missing: $RootFile" }

$targets = @(0x69d1d0, 0xc6dbf0) # WinMain, documented renderer gate
$results = New-Object System.Collections.Generic.List[object]
$functions = $StartFunctions
$insns = $StartInsns
$stopAll = ''
$stage = 0
$startLeaf = Split-Path -Leaf $StartWorkRoot
if($startLeaf -notmatch '^growth(\d+)$'){ throw "StartWorkRoot must end with growthNN: $StartWorkRoot" }
$startNumber = [int]$Matches[1]

for($stage=1; $stage -le $MaxStages; $stage++){
    $work = if($stage -eq 1){ $StartWorkRoot } else {
        Join-Path (Split-Path -Parent $StartWorkRoot) ("growth{0}" -f ($startNumber + $stage - 1))
    }
    Write-Host "=== supervised stage $stage work=$work functions=$functions insns=$insns roots_file=$RootFile ==="
    if($PlanOnly){ break }

    $env:TESV_MEMPROBE = '1'
    $env:TESV_D3D9_TRACE = '1'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $grow -WorkRoot $work `
        -RootFile $RootFile -StartFunctions $functions -StartInsns $insns `
        -StepFunctions $StepFunctions -StepInsns $StepInsns `
        -MaxIterations $StageIterations -Steps $Steps
    $workerRc = $LASTEXITCODE
    $journalPath = Join-Path $work 'growth_journal.json'
    if(-not (Test-Path $journalPath)){ $stopAll = "stage $stage has no journal (worker rc=$workerRc)"; break }
    $journal = Get-Content $journalPath -Raw | ConvertFrom-Json
    $entries = @($journal.journal)
    $lastFault = $entries | Where-Object { $_.fault_addr } | Select-Object -Last 1
    $fault = if($lastFault){ [Convert]::ToUInt32(($lastFault.fault_addr -replace '^0x',''),16) } else { 0 }
    $reason = if($lastFault){ $lastFault.reason } else { $journal.stop_reason }
    $lastGen = $journal.last_gen
    $lastSlice = if($lastFault){ $lastFault.gen } else { $lastGen }
    $lastIter = Get-ChildItem $work -Directory -Filter 'iter*' | Sort-Object Name | Select-Object -Last 1
    $sliceJson = if($lastIter){ Join-Path $lastIter.FullName 'slice_functions.json' } else { '' }
    $roots = @($journal.roots).Count
    $faultHex = '0x{0:x8}' -f $fault
    Write-Host "stage ${stage}: roots=$roots fault=$faultHex reason=$reason gen=$lastGen"

    $profile = Get-ChildItem (Join-Path $root 'build_azahar') -Directory -Filter '*hwparity*' -ErrorAction SilentlyContinue
    $hostLogs = Get-ChildItem $work -Recurse -File -Filter 'probe_run.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $reachedWinMain = $false
    if($hostLogs){
        $hostText = Get-Content $hostLogs.FullName -Raw
        if($hostText -match 'Direct3DCreate9|CreateDevice|PICA200 backend installed'){ $reachedWinMain = $true }
    }
    if($fault -ge 0x69d000 -and $fault -lt 0x6a0000){ $reachedWinMain = $true }
    if($fault -ge 0xc6c000 -and $fault -lt 0xc70000){ $reachedWinMain = $true }

    $record = [ordered]@{
        stage=$stage; work=$work; roots=$roots; fault=$faultHex
        reason=$reason; gen=$lastGen; reached_winmain_or_d3d9=$reachedWinMain
        parity='not-run'; parity_azahar_fault=''; parity_ok=$false
    }

    # Host/Azahar parity for the last generated slice.
    if($sliceJson -and (Test-Path $sliceJson) -and $lastGen){
        $failRaw = Join-Path $work 'forced_aot_raw.txt'
        $failValid = Join-Path $work 'forced_aot_valid.txt'
        & $python tools/ir_measure.py --exe $exe --functions $sliceJson --fail-list $failRaw 2>&1 |
            Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
        if(Test-Path $failRaw){
            Get-Content $failRaw | Where-Object { $_ -notmatch '^0x00636573$|^0x00676f46$' } |
                Set-Content $failValid -Encoding ascii
            $isolated = 'hwparity{0}' -f $stage
            $parityOut = Join-Path $work 'hwparity'
            & powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_hw_hybrid.ps1 `
                -Functions $sliceJson -ForceAot $failValid -OutRoot $parityOut `
                -Isolated $isolated -AzaharTimeoutSec 300 -RunAzahar
            if($LASTEXITCODE -ne 0){ $record.parity='FAIL'; $stopAll="stage $stage hybrid parity failed"; $results.Add([pscustomobject]$record) | Out-Null; break }
            $hm = Join-Path $parityOut 'hw_manifest.json'
            $azFault = ''
            if(Test-Path $hm){
                $m = Get-Content $hm -Raw | ConvertFrom-Json
                $ev = $m.azahar_evidence
                $log = if($ev){ Join-Path $ev 'Skyrim_N3DS_tesv.log' } else { '' }
                if($log -and (Test-Path $log)){
                    $t = Get-Content $log -Raw
                    $mm = [regex]::Match($t,'Guest fault at ([0-9a-fA-F]+):')
                    if($mm.Success){ $azFault = '0x' + $mm.Groups[1].Value.ToLower() }
                }
            }
            $record.parity_azahar_fault = $azFault
            $record.parity_ok = ($azFault -eq $record.fault)
            $record.parity = if($record.parity_ok){'PASS'}else{'MISMATCH'}
            if(-not $record.parity_ok){ $stopAll="stage $stage parity mismatch: host $($record.fault), azahar $azFault" }
        }
    }
    $results.Add([pscustomobject]$record) | Out-Null
    if($stopAll){ break }
    if($reachedWinMain){ $stopAll="stage $stage reached WinMain/renderer/D3D9 target"; break }
    if($reason -match 'unresolved (ordinal )?import|host compile failed|no fault parsed|repeated fault|slice failed|lift failed|instrument failed'){ $stopAll="stage $stage stopped: $reason"; break }

    $functions = $functions + $StageIterations * $StepFunctions
    $insns = $insns + $StageIterations * $StepInsns
    $RootFile = $journal.root_file
}

if(-not $stopAll){ $stopAll = "stage limit $MaxStages reached" }
$resultArray = @($results.ToArray())
$summary = [ordered]@{
    finished_at=[DateTime]::Now.ToString('o'); stop_reason=$stopAll
    stages=$stage; roots_file=$RootFile; results=$resultArray
}
$summaryPath = Join-Path $StartWorkRoot 'supervisor.json'
New-Item -ItemType Directory -Force -Path $StartWorkRoot | Out-Null
$summary | ConvertTo-Json -Depth 8 | Set-Content $summaryPath -Encoding utf8
Write-Host "SUPERVISOR: $stopAll"
Write-Host "SUMMARY: $summaryPath"
if($stopAll -match 'failed|mismatch|no fault|repeated'){ exit 1 }
exit 0
