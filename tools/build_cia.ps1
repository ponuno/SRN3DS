#requires -Version 5.1
<#
.SYNOPSIS
  Package a locally built ELF into an installable CIA (New 3DS, 178 MB mode).
.DESCRIPTION
  Generates the icon/banner assets, runs bannertool and makerom with
  tools/cia/template.rsf (SystemModeExt: 178MB, CPU 804MHz, L2 on), and writes
  a manifest with hashes. The resulting CIA contains locally generated code and
  must not be distributed.
.PARAMETER Elf
  Input ELF (default: dist/Skyrim_N3DS_tesv/Skyrim_N3DS_tesv.elf)
.PARAMETER Out
  Output CIA path.
#>
param(
    [string]$Elf = "",
    [string]$Out = "",
    [string]$Rsf = "",
    [string]$WorkDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Elf) { $Elf = Join-Path $root "dist\Skyrim_N3DS_tesv\Skyrim_N3DS_tesv.elf" }
if (-not $Out) { $Out = Join-Path $root "build_cia\Skyrim_N3DS_tesv.cia" }
if (-not $Rsf) { $Rsf = Join-Path $root "tools\cia\template.rsf" }
if (-not $WorkDir) { $WorkDir = Join-Path $root "build_cia" }

$tools = "C:\devkitPro\tools\bin"
$bannertool = Join-Path $tools "bannertool.exe"
$makerom = Join-Path $tools "makerom.exe"
foreach ($p in @($Elf, $Rsf)) { if (-not (Test-Path $p)) { throw "Missing input: $p" } }
foreach ($p in @($bannertool, $makerom)) { if (-not (Test-Path $p)) { throw "Missing tool: $p" } }

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

function New-Png([string]$path, [int]$w, [int]$h, [string]$text, [int]$size) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(8, 10, 16))
    $font = New-Object System.Drawing.Font("Arial", $size, [System.Drawing.FontStyle]::Bold)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = "Center"; $fmt.LineAlignment = "Center"
    $g.DrawString($text, $font, [System.Drawing.Brushes]::White,
        (New-Object System.Drawing.RectangleF(0, 0, $w, $h)), $fmt)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

function New-SilentWav([string]$path) {
    $rate = 44100; $samples = $rate; $dataBytes = $samples * 2
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([Text.Encoding]::ASCII.GetBytes("RIFF"))
    $bw.Write([int](36 + $dataBytes))
    $bw.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
    $bw.Write([int]16); $bw.Write([int16]1); $bw.Write([int16]1)
    $bw.Write([int]$rate); $bw.Write([int]($rate * 2)); $bw.Write([int16]2); $bw.Write([int16]16)
    $bw.Write([Text.Encoding]::ASCII.GetBytes("data")); $bw.Write([int]$dataBytes)
    $bw.Write((New-Object byte[] $dataBytes))
    $bw.Flush()
    [IO.File]::WriteAllBytes($path, $ms.ToArray())
    $bw.Dispose(); $ms.Dispose()
}

$iconPng = Join-Path $WorkDir "icon.png"
$bannerPng = Join-Path $WorkDir "banner.png"
$bannerWav = Join-Path $WorkDir "banner.wav"
$iconBin = Join-Path $WorkDir "icon.bin"
$bannerBin = Join-Path $WorkDir "banner.bin"

New-Png $iconPng 48 48 "S3DS" 10
New-Png $bannerPng 256 128 "Skyrim N3DS`nTESV startup slice" 16
New-SilentWav $bannerWav

& $bannertool makesmdh -s "Skyrim N3DS" -l "TESV startup slice (local build)" -p "local" -i $iconPng -o $iconBin
if ($LASTEXITCODE -ne 0) { throw "bannertool makesmdh failed" }
& $bannertool makebanner -i $bannerPng -a $bannerWav -o $bannerBin
if ($LASTEXITCODE -ne 0) { throw "bannertool makebanner failed" }

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
& $makerom -f cia -target t -elf $Elf -rsf $Rsf -icon $iconBin -banner $bannerBin -o $Out
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Out)) { throw "makerom failed (exit $LASTEXITCODE)" }

$manifest = [ordered]@{
    created_at = [DateTime]::Now.ToString("o")
    elf = $Elf
    elf_sha256 = (Get-FileHash $Elf -Algorithm SHA256).Hash.ToLower()
    rsf = $Rsf
    cia = $Out
    cia_bytes = (Get-Item $Out).Length
    cia_sha256 = (Get-FileHash $Out -Algorithm SHA256).Hash.ToLower()
    note = "Local build only. Contains generated code derived from the owner's game files; do not distribute."
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $WorkDir "cia_manifest.json") -Encoding utf8
Write-Host "CIA: $Out ($($manifest.cia_bytes) bytes)"
Write-Host "Manifest: $(Join-Path $WorkDir 'cia_manifest.json')"
exit 0
