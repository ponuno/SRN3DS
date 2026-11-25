#requires -Version 5.1
<#
.SYNOPSIS  Rebuild the TESV diagnostic title (.3dsx + CIA) from a lifted slice.
.DESCRIPTION  Runs the ARM build and CIA packaging, then prints the artifacts and
  hashes. The CIA is a local build derived from the owner's files; do not share.
#>
param(
    [Parameter(Mandatory = $true)][string]$TesvGen,
    [string]$Opt = "-O2"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path (Join-Path $TesvGen "tesv_table.c"))) {
    throw "tesv_table.c not found in $TesvGen"
}

& powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Mode tesv -TesvGen $TesvGen -Opt $Opt
if ($LASTEXITCODE -ne 0) { throw "ARM build failed" }

& powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_cia.ps1 `
    -Elf (Join-Path $root "dist\Skyrim_N3DS_tesv\Skyrim_N3DS_tesv.elf")
if ($LASTEXITCODE -ne 0) { throw "CIA packaging failed" }

$dsx = Join-Path $root "dist\Skyrim_N3DS_tesv\Skyrim_N3DS_tesv.3dsx"
$cia = Join-Path $root "build_cia\Skyrim_N3DS_tesv.cia"
foreach ($f in @($dsx, $cia)) {
    $h = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
    Write-Host ("{0}  {1} bytes  sha256 {2}" -f $f, (Get-Item $f).Length, $h)
}
Write-Host "Install the CIA via Azahar File -> Install CIA or a 3DS CIA installer."
Write-Host "Recheck App region/free in Skyrim_N3DS_tesv.log after launch."
exit 0
