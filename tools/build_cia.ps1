#requires -Version 5.1
<#
.SYNOPSIS
  Package a locally built ELF into an installable CIA (New 3DS, 178 MB mode).
.DESCRIPTION
  Generates the icon/banner assets, runs bannertool and makerom with
  tools/cia/template.rsf (SystemModeExt: 178MB, CPU 804MHz, L2 on), then parses
  the resulting CIA header/TMD to confirm the title ID and writes a manifest
  with hashes. The resulting CIA contains locally generated code and must not
  be distributed.

  makerom 0.19 has no "-i" inspector, so verification is done in-script: the
  CIA header sizes are checked and the TMD title ID must match the RSF
  UniqueId.
.PARAMETER Elf
  Input ELF (default: dist/Skyrim_N3DS_tesv/Skyrim_N3DS_tesv.elf)
.PARAMETER Out
  Output CIA path (default: build_cia/Skyrim_N3DS_tesv.cia)
.PARAMETER Rsf
  ROM spec file (default: tools/cia/template.rsf)
.PARAMETER WorkDir
  Directory for generated icon/banner, manifest and verification report.
.PARAMETER Tools
  Optional devkitPro tools/bin directory. Defaults to $env:DEVKITPRO\tools\bin,
  C:\devkitPro\tools\bin, then PATH.
#>
param(
    [string]$Elf = "",
    [string]$Out = "",
    [string]$Rsf = "",
    [string]$WorkDir = "",
    [string]$Tools = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Elf) { $Elf = Join-Path $root "dist\Skyrim_N3DS_tesv\Skyrim_N3DS_tesv.elf" }
if (-not $Out) { $Out = Join-Path $root "build_cia\Skyrim_N3DS_tesv.cia" }
if (-not $Rsf) { $Rsf = Join-Path $root "tools\cia\template.rsf" }
if (-not $WorkDir) { $WorkDir = Join-Path $root "build_cia" }

function Resolve-Tool([string]$name) {
    $hints = @()
    if ($Tools) { $hints += $Tools }
    if ($env:DEVKITPRO) { $hints += (Join-Path $env:DEVKITPRO "tools\bin") }
    $hints += "C:\devkitPro\tools\bin"
    foreach ($h in $hints) {
        $p = Join-Path $h $name
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "Missing tool '$name'. Install devkitPro tools or pass -Tools <dir>."
}

$bannertool = Resolve-Tool "bannertool.exe"
$makerom = Resolve-Tool "makerom.exe"

foreach ($p in @($Elf, $Rsf)) {
    if (-not (Test-Path $p)) { throw "Missing input: $p" }
    if ((Get-Item $p).Length -le 0) { throw "Empty input: $p" }
}
if ($Elf -notmatch '\.elf$') { throw "Expected an ELF input, got: $Elf" }

# Parse the RSF TitleInfo block so the built CIA can be checked against it.
$rsfText = Get-Content $Rsf -Raw
$uidMatch = [regex]::Match($rsfText, '(?m)^\s*UniqueId\s*:\s*(0x[0-9A-Fa-f]+)')
if (-not $uidMatch.Success) { throw "No TitleInfo/UniqueId in $Rsf" }
$uniqueId = [Convert]::ToUInt32($uidMatch.Groups[1].Value, 16)
if ($uniqueId -gt 0xFFFFF) { throw "UniqueId 0x$($uniqueId.ToString('X')) exceeds 20 bits" }
$expectedTitleId = 0x0004000000000000L -bor ([int64]$uniqueId -shl 8)

$rsfInfo = @(
    (Select-String -Path $Rsf -Pattern '^\s*SystemModeExt\s*:\s*(\S+)'),
    (Select-String -Path $Rsf -Pattern '^\s*CpuSpeed\s*:\s*(\S+)'),
    (Select-String -Path $Rsf -Pattern '^\s*EnableL2Cache\s*:\s*(\S+)'),
    (Select-String -Path $Rsf -Pattern '^\s*SystemMode\s*:\s*(\S+)')
) | ForEach-Object { $_.Matches[0].Groups[1].Value }
if ($rsfInfo[0] -notmatch '178MB') { throw "RSF SystemModeExt is '$($rsfInfo[0])', expected 178MB (New 3DS)" }

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
# -target t = test keys + production certs (installable on CFW/Azahar).
& $makerom -f cia -target t -elf $Elf -rsf $Rsf -icon $iconBin -banner $bannerBin -exefslogo -o $Out
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Out)) { throw "makerom failed (exit $LASTEXITCODE)" }

# --- verification ------------------------------------------------------------
# makerom 0.19 has no "-i": check the CIA header, then read the embedded NCCH
# partition id (which equals the application title ID) and the SMDH strings.
function Read-Cia([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $u32 = { param($o) [BitConverter]::ToUInt32($b, $o) }
    $u64 = { param($o) [BitConverter]::ToUInt64($b, $o) }

    $headerSize = & $u32 0
    if ($headerSize -ne 0x2020) { throw "Bad CIA header size 0x$($headerSize.ToString('X'))" }
    $certSize = & $u32 0x08
    $ticketSize = & $u32 0x0C
    $tmdSize = & $u32 0x10
    $metaSize = & $u32 0x14
    $contentSize = & $u64 0x18
    if ($certSize -le 0) { throw "CIA has no certificate chain" }
    if ($ticketSize -le 0) { throw "CIA has no ticket" }
    if ($tmdSize -lt 0x120 -or $tmdSize -gt 0x4000) { throw "Implausible TMD size $tmdSize" }
    if ($metaSize -le 0) { throw "CIA has no SMDH meta (icon/banner) block" }
    if ($contentSize -le 0) { throw "CIA has no content" }

    # Locate the NCCH content by its magic; signature block precedes it.
    $ascii = [Text.Encoding]::ASCII.GetString($b)
    $ncchAt = $ascii.IndexOf("NCCH")
    if ($ncchAt -lt 0x100 -or $ncchAt -gt $b.Length) { throw "No NCCH magic found in CIA content" }
    $ncch = $ncchAt - 0x100
    $partitionId = & $u64 ($ncch + 0x108)
    $contentBytes = (& $u32 ($ncch + 0x104)) * 0x200   # media units (0x200)

    if ($ascii.IndexOf("SMDH") -lt 0) { throw "No SMDH meta block in CIA" }

    return [pscustomobject]@{
        TitleId        = ('{0:X16}' -f $partitionId)
        ContentOffset  = ('0x{0:X}' -f $ncch)
        ContentBytes   = $contentBytes
        HeaderBytes    = $b.Length
        CertBytes      = $certSize
        TicketBytes    = $ticketSize
        TmdBytes       = $tmdSize
        MetaBytes      = $metaSize
        HasSmdh        = $true
    }
}

$cia = Read-Cia $Out
if ($cia.TitleId -ne ('{0:X16}' -f $expectedTitleId)) {
    throw "CIA title ID $($cia.TitleId) does not match RSF UniqueId $('{0:X16}' -f $expectedTitleId)"
}
$infoText = @(
    "CIA: $Out"
    "bytes: $($cia.HeaderBytes)"
    "sha256: $((Get-FileHash $Out -Algorithm SHA256).Hash.ToLower())"
    "title_id: $($cia.TitleId)"
    "ncch_offset: $($cia.ContentOffset)"
    "content_bytes: $($cia.ContentBytes)"
    "smdh: present"
    "sections: cert=$($cia.CertBytes) ticket=$($cia.TicketBytes) tmd=$($cia.TmdBytes) meta=$($cia.MetaBytes)"
    "rsf: $Rsf"
    "exheader: SystemModeExt=$($rsfInfo[0]) CpuSpeed=$($rsfInfo[1]) EnableL2Cache=$($rsfInfo[2]) SystemMode=$($rsfInfo[3])"
) -join "`n"
$infoPath = Join-Path $WorkDir "cia_info.txt"
$infoText | Set-Content $infoPath -Encoding utf8

$manifest = [ordered]@{
    created_at = [DateTime]::Now.ToString("o")
    elf = $Elf
    elf_sha256 = (Get-FileHash $Elf -Algorithm SHA256).Hash.ToLower()
    rsf = $Rsf
    rsf_sha256 = (Get-FileHash $Rsf -Algorithm SHA256).Hash.ToLower()
    cia = $Out
    cia_bytes = $cia.HeaderBytes
    cia_sha256 = (Get-FileHash $Out -Algorithm SHA256).Hash.ToLower()
    title_id = $cia.TitleId
    content_bytes = $cia.ContentBytes
    smdh = $cia.HasSmdh
    exheader = @{
        system_mode = $rsfInfo[3]
        system_mode_ext = $rsfInfo[0]
        cpu_speed = $rsfInfo[1]
        l2_cache = $rsfInfo[2]
    }
    tools = @{
        makerom = $makerom
        bannertool = $bannertool
    }
    note = "Local build only. Contains generated code derived from the owner's game files; do not distribute."
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $WorkDir "cia_manifest.json") -Encoding utf8
Write-Host "CIA: $Out ($($manifest.cia_bytes) bytes)"
Write-Host "Title ID: $($manifest.title_id)"
Write-Host "Manifest: $(Join-Path $WorkDir 'cia_manifest.json')"
Write-Host "Verify: $infoPath"
exit 0
