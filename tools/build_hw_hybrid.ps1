#requires -Version 5.1
<#
.SYNOPSIS
Build and optionally package a physical-test hybrid TESV CIA from one slice.
.DESCRIPTION
Runs time/call-ranked AOT selection, emits the exact IR table, lifts only the
AOT functions, builds an isolated probe-enabled TESV target, optionally runs it
in Azahar, and packages a CIA with verified hashes. Intermediates stay on E:.
#>
param(
    [Parameter(Mandatory=$true)][string]$Functions,
    [string]$Profile = 'E:\Skyrim_N3DS_build\analysis\profile_post_va_163.csv',
    [string]$ForceAot = 'E:\Skyrim_N3DS_build\analysis\ir_v8_valid_fail.txt',
    [Parameter(Mandatory=$true)][string]$OutRoot,
    [string]$Isolated = 'hwhybrid',
    [ValidateSet('time','calls','weighted_insns')][string]$RankBy = 'calls',
    [int]$Limit = 32,
    [switch]$SkipBuild,
    [switch]$RunAzahar,
    [int]$AzaharTimeoutSec = 60,
    [string]$CiaOut = '',
    [switch]$CopyToCard
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$python = Join-Path $root '.venv\Scripts\python.exe'
Set-Location $root
if(-not (Test-Path $python)){ throw "venv python missing: $python" }
if(-not (Test-Path $Functions)){ throw "functions.json missing: $Functions" }
if(-not (Test-Path $Profile)){ throw "profile missing: $Profile" }
if(-not (Test-Path $ForceAot)){ throw "forced-AOT list missing: $ForceAot" }
if($Isolated -notmatch '^[A-Za-z0-9][A-Za-z0-9_]{0,15}$'){ throw 'invalid -Isolated name' }

$select = Join-Path $OutRoot 'select'
$gen = Join-Path $OutRoot 'gen'
New-Item -ItemType Directory -Force -Path $select,$gen | Out-Null
$irTable = Join-Path $select 'ir_table.c'

& $python tools/ir_select.py $Profile $Functions --out $select --limit $Limit `
    --rank-by $RankBy --force-aot $ForceAot --emit-ir-table $irTable --validate
if($LASTEXITCODE -ne 0){ throw 'ir_select failed' }

& $python tools/ir_make_hybrid.py --functions $Functions --selection $select `
    --exe 'E:\skyr\TESV.exe' --out $gen
if($LASTEXITCODE -ne 0){ throw 'ir_make_hybrid failed' }

$target = "Skyrim_N3DS_tesv_$Isolated"
$elf = Join-Path $root "$target.elf"
$dsx = Join-Path $root "$target.3dsx"
if(-not $SkipBuild){
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Mode tesv `
        -TesvGen $gen -Isolated $Isolated -ForceMemProbe
    if($LASTEXITCODE -ne 0){ throw 'ARM build failed' }
}
if(-not (Test-Path $elf) -or -not (Test-Path $dsx)){ throw 'isolated ELF/.3dsx missing' }

$azaharEvidence = ''
if($RunAzahar){
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_azahar.ps1 `
        -ThreeDsx $dsx -Name $Isolated -TimeoutSec $AzaharTimeoutSec -AllowFaultReport
    if($LASTEXITCODE -ne 0){ throw 'Azahar diagnostic run failed; inspect archived logs' }
    $azaharEvidence = (Get-ChildItem (Join-Path $root 'build_azahar') -Directory -Filter "*$Isolated" |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}

$cia = $CiaOut
if($cia){
    $ciaDir = Split-Path -Parent $cia
    New-Item -ItemType Directory -Force -Path $ciaDir | Out-Null
    $ciaWork = Join-Path $OutRoot 'cia'
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_cia.ps1 `
        -Elf $elf -Out $cia -WorkDir $ciaWork
    if($LASTEXITCODE -ne 0){ throw 'CIA packaging failed' }
}
if($CopyToCard){
    if(-not $cia){ throw '-CopyToCard requires -CiaOut' }
    if(-not (Test-Path 'F:\')){ throw 'F: is not mounted' }
    $cardCia = Join-Path 'F:\cias' (Split-Path $cia -Leaf)
    New-Item -ItemType Directory -Force -Path 'F:\cias' | Out-Null
    Copy-Item $cia $cardCia -Force
    $srcHash = (Get-FileHash $cia -Algorithm SHA256).Hash
    $dstHash = (Get-FileHash $cardCia -Algorithm SHA256).Hash
    if($srcHash -ne $dstHash){ throw 'SD copy hash mismatch' }
    $cia = $cardCia
}

$manifest = [ordered]@{
    created_at = [DateTime]::Now.ToString('o')
    functions = (Resolve-Path $Functions).Path
    profile = (Resolve-Path $Profile).Path
    forced_aot = (Resolve-Path $ForceAot).Path
    rank_by = $RankBy
    limit = $Limit
    isolated = $Isolated
    generated_dir = $gen
    azahar_evidence = $azaharEvidence
    azahar_timeout_sec = $AzaharTimeoutSec
    elf = $elf
    elf_bytes = (Get-Item $elf).Length
    elf_sha256 = (Get-FileHash $elf -Algorithm SHA256).Hash.ToLower()
    dsx = $dsx
    dsx_bytes = (Get-Item $dsx).Length
    dsx_sha256 = (Get-FileHash $dsx -Algorithm SHA256).Hash.ToLower()
    cia = $cia
}
if($cia){
    $manifest.cia_bytes = (Get-Item $cia).Length
    $manifest.cia_sha256 = (Get-FileHash $cia -Algorithm SHA256).Hash.ToLower()
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutRoot 'hw_manifest.json') -Encoding utf8
Write-Host "Hybrid hardware manifest: $(Join-Path $OutRoot 'hw_manifest.json')"
if($cia){ Write-Host "CIA: $cia ($($manifest.cia_bytes) bytes, sha256 $($manifest.cia_sha256))" }
exit 0
