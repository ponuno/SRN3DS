#requires -Version 5.1
<#
.SYNOPSIS
  Launch a .3dsx in desktop Azahar, capture evidence, and stop only that instance.
.DESCRIPTION
  Copies the file into the virtual SD card, starts Azahar with the file, waits for
  a bounded time, saves a window screenshot plus the emulator/app logs under
  build_azahar/<stamp>-<name>/, then closes the emulator instance it started.
  Existing Azahar windows are never touched.
#>
param(
    [Parameter(Mandatory = $true)][string]$ThreeDsx,
    [string]$Name = "",
    [int]$TimeoutSec = 20,
    [string]$Azahar = "C:\Program Files\Azahar\azahar.exe",
    [string]$SdRoot = "",
    [switch]$SkipAppLogCheck,
    [switch]$AllowFaultReport,
    [string[]]$Grep = @("FAIL", "CRASH")
)

$ErrorActionPreference = "Continue"
$runMutex = New-Object System.Threading.Mutex($false, "Global\SkyrimN3DS_AzaharRun")
$haveMutex = $false
try { $haveMutex = $runMutex.WaitOne(0) }
catch [System.Threading.AbandonedMutexException] { $haveMutex = $true; Write-Host "WARN: previous run_azahar.ps1 ended while holding the lock" }
if (-not $haveMutex) { Write-Error "Another run_azahar.ps1 instance is active; refusing to share logs/SD state"; exit 3 }
$root = Split-Path -Parent $PSScriptRoot
if (-not $SdRoot) { $SdRoot = Join-Path $env:APPDATA "Azahar\sdmc" }
if (-not (Test-Path $ThreeDsx)) { throw "3dsx not found: $ThreeDsx" }
if (-not (Test-Path $Azahar)) { throw "Azahar not found: $Azahar" }
if (-not $Name) { $Name = [IO.Path]::GetFileNameWithoutExtension($ThreeDsx) }

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$ev = Join-Path $root "build_azahar\$stamp-$Name"
New-Item -ItemType Directory -Force -Path $ev | Out-Null
$log = Join-Path $ev "run.log"
function Write-Log([string]$text) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $text
    Write-Host $line
    Add-Content -Path $log -Value $line
}

$appDir = Join-Path $SdRoot "3ds\$Name"
New-Item -ItemType Directory -Force -Path $appDir | Out-Null
$sdFile = Join-Path $appDir ([IO.Path]::GetFileName($ThreeDsx))
Copy-Item $ThreeDsx $sdFile -Force
$srcHash = (Get-FileHash $ThreeDsx -Algorithm SHA256).Hash
$dstHash = (Get-FileHash $sdFile -Algorithm SHA256).Hash
if ($srcHash -ne $dstHash) { throw "Copy to virtual SD failed or was truncated: $sdFile" }
Write-Log "Copied $ThreeDsx -> $sdFile (sha256 $srcHash)"

$appLog = Join-Path $SdRoot "Skyrim_N3DS.log"
$pilotLog = Join-Path $SdRoot "Skyrim_N3DS_pilot.log"
$tesvLog = Join-Path $SdRoot "Skyrim_N3DS_tesv.log"
$stderrLog = Join-Path $SdRoot "Skyrim_N3DS_stderr.log"
foreach ($f in @($appLog, $stderrLog, $pilotLog, $tesvLog)) { if (Test-Path $f) { Remove-Item $f -Force } }

Add-Type -AssemblyName System.Drawing
if (-not ("AzWin" -as [type])) {
    Add-Type @'
using System;using System.Runtime.InteropServices;
public class AzWin {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
  public struct RECT{public int L,T,R,B;}
}
'@
}

$p = Start-Process -FilePath $Azahar -ArgumentList "`"$sdFile`"" -PassThru
Write-Log "Started Azahar PID=$($p.Id); waiting ${TimeoutSec}s"
Start-Sleep -Seconds $TimeoutSec
$p.Refresh()
$p | Select-Object Id, HasExited, MainWindowTitle | Format-List | Out-String | ForEach-Object { Write-Log $_.Trim() }

if (-not $p.HasExited -and $p.MainWindowHandle -ne 0) {
    $r = New-Object AzWin+RECT
    [AzWin]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $h = $r.B - $r.T
    if ($w -gt 0 -and $h -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [AzWin]::PrintWindow($p.MainWindowHandle, $hdc, 2) | Out-Null
        $g.ReleaseHdc($hdc); $g.Dispose()
        $png = Join-Path $ev "screen.png"
        $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        Write-Log "Screenshot: $png"
    }
}

if (-not $p.HasExited) {
    $p.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 3
    $p.Refresh()
    if (-not $p.HasExited) { $p.Kill() }
    Write-Log "Closed Azahar PID=$($p.Id)"
}

foreach ($f in @($appLog, $stderrLog, $pilotLog, $tesvLog)) {
    if (Test-Path $f) {
        $dest = Join-Path $ev ([IO.Path]::GetFileName($f))
        Copy-Item $f $dest -Force
        Write-Log "--- $dest ---"
        Get-Content $dest | ForEach-Object { Write-Log "  $_" }
    }
}
$emuLog = Join-Path $env:APPDATA "Azahar\log\azahar_log.txt"
if (Test-Path $emuLog) {
    Copy-Item $emuLog (Join-Path $ev "azahar_log.txt") -Force
    $tail = Get-Content $emuLog -Tail 25
    Write-Log "--- azahar_log.txt (tail) ---"
    $tail | ForEach-Object { Write-Log "  $_" }
}

$failed = $false
$hasPass = $false
$faultAddr = $null
$faultReason = $null
foreach ($f in @($appLog, $stderrLog, $pilotLog, $tesvLog)) {
    if (-not (Test-Path $f)) { continue }
    $text = Get-Content $f -Raw
    foreach ($needle in $Grep) { if ($text -match [regex]::Escape($needle)) { $failed = $true } }
    if ($text -match '(?m):\s*PASS(?:\s|\(|$)') { $hasPass = $true }
    $fm = [regex]::Match($text, 'Guest fault at ([0-9a-fA-F]+):\s*(.+)')
    if ($fm.Success) { $faultAddr = $fm.Groups[1].Value; $faultReason = $fm.Groups[2].Value.Trim() }
    if ($AllowFaultReport) {
        if (-not $hasPass -and -not $fm.Success) { $failed = $true }
    } elseif (-not $hasPass) {
        $failed = $true
    }
}
if (-not $SkipAppLogCheck -and -not (Test-Path $appLog) -and -not (Test-Path $stderrLog) -and -not (Test-Path $pilotLog) -and -not (Test-Path $tesvLog)) {
    Write-Log "Expected an application log but none was written"
    $failed = $true
}
$emuLog = Join-Path $env:APPDATA "Azahar\log\azahar_log.txt"
if (Test-Path $emuLog) {
    $emuText = Get-Content $emuLog -Raw
    foreach ($marker in @("Failed to obtain loader", "Frontend <Critical>")) {
        if ($emuText -match [regex]::Escape($marker)) {
            Write-Log "Emulator could not load the ROM: $marker"
            $failed = $true
        }
    }
}
if ($failed) { Write-Log "AZAHAR RESULT: FAIL" }
elseif ($faultAddr) { Write-Log "AZAHAR DIAG: fault at $faultAddr : $faultReason" }
elseif ($hasPass) { Write-Log "AZAHAR RESULT: PASS" }
else { Write-Log "AZAHAR RESULT: no PASS marker"; $failed = $true }

Write-Log "Evidence: $ev"
if ($failed) { exit 1 }
exit 0
