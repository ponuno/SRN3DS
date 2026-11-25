#requires -Version 5.1
<#
.SYNOPSIS
  Run the isolated armtime .3dsx in Azahar and archive bounded evidence.
.DESCRIPTION
  Starts only the Azahar process it creates, waits for the guest PASS/FAIL marker
  or a bounded timeout, closes that process, and archives guest outputs plus a
  bounded tail of the emulator log under tests/isolated/armtime/results/.
  No main runtime/build input is modified.
#>
param(
    [string]$ThreeDsx = (Join-Path $PSScriptRoot "armtime.3dsx"),
    [int]$TimeoutSec = 90,
    [string]$Azahar = "C:\Program Files\Azahar\azahar.exe",
    [string]$SdRoot = (Join-Path $env:APPDATA "Azahar\sdmc"),
    [string]$RunName = "armtime",
    [int]$MaxLogBytes = 131072
)

$ErrorActionPreference = "Stop"
$runMutex = New-Object System.Threading.Mutex($false, "Global\SkyrimN3DS_AzaharRun")
$haveMutex = $false
try { $haveMutex = $runMutex.WaitOne(0) }
catch [System.Threading.AbandonedMutexException] { $haveMutex = $true }
if (-not $haveMutex) {
    Write-Host "Another Skyrim_N3DS Azahar run is active; refusing to share SD/log state"
    exit 3
}
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$results = Join-Path $PSScriptRoot "results"
New-Item -ItemType Directory -Force -Path $results | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runDir = Join-Path $results "$stamp-$RunName"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$runnerLog = Join-Path $runDir "runner.log"

function Write-RunLog([string]$text) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $text
    Write-Host $line
    Add-Content -Path $runnerLog -Value $line
}

Write-RunLog "scope=Azahar emulator timing; NOT New 3DS hardware"
foreach ($path in @($ThreeDsx, $Azahar)) {
    if (-not (Test-Path $path)) { throw "missing required path: $path" }
}
$ThreeDsx = (Resolve-Path $ThreeDsx).Path
$Azahar = (Resolve-Path $Azahar).Path
$sourceHash = (Get-FileHash $ThreeDsx -Algorithm SHA256).Hash
Copy-Item $ThreeDsx (Join-Path $runDir "armtime.3dsx") -Force
Write-RunLog "source=$ThreeDsx sha256=$sourceHash"

$appDir = Join-Path $SdRoot "3ds\$RunName"
New-Item -ItemType Directory -Force -Path $appDir | Out-Null
$sdFile = Join-Path $appDir "armtime.3dsx"
Copy-Item $ThreeDsx $sdFile -Force
$sdHash = (Get-FileHash $sdFile -Algorithm SHA256).Hash
if ($sdHash -ne $sourceHash) { throw "virtual SD copy hash mismatch" }
Write-RunLog "sdmc_copy=$sdFile sha256=$sdHash"

$outputs = @("Skyrim_N3DS.log", "armtime_profile.csv", "armtime_aot.csv",
             "armtime_ir_warm.csv", "armtime_ir_cold.csv", "armtime_meta.json")
foreach ($name in $outputs) {
    $path = Join-Path $SdRoot $name
    if (Test-Path $path) { Remove-Item $path -Force }
}

$versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($Azahar)
$versionText = "file=$($versionInfo.FileVersion) product=$($versionInfo.ProductVersion)"
Write-RunLog "azahar_version=$versionText"
$proc = Start-Process -FilePath $Azahar -ArgumentList "`"$sdFile`"" -PassThru
Write-RunLog "azahar_pid=$($proc.Id) timeout_sec=$TimeoutSec"

$guestLog = Join-Path $SdRoot "Skyrim_N3DS.log"
$marker = ""
$exitedEarly = $false
$watch = [Diagnostics.Stopwatch]::StartNew()
while ($watch.Elapsed.TotalSeconds -lt $TimeoutSec) {
    $proc.Refresh()
    if ($proc.HasExited) { $exitedEarly = $true; break }
    if (Test-Path $guestLog) {
        $text = Get-Content $guestLog -Raw -ErrorAction SilentlyContinue
        if ($text -match "ARMTIME RESULT: (PASS|FAIL)") {
            $marker = $Matches[1]
            break
        }
    }
    Start-Sleep -Milliseconds 250
}
$watch.Stop()
if (-not $marker) {
    if ($exitedEarly) { $marker = "PROCESS_EXITED" } else { $marker = "TIMEOUT" }
}
Write-RunLog "guest_marker=$marker elapsed_sec=$([math]::Round($watch.Elapsed.TotalSeconds, 3))"

$proc.Refresh()
if (-not $proc.HasExited) {
    $proc.CloseMainWindow() | Out-Null
    Start-Sleep -Seconds 2
    $proc.Refresh()
    if (-not $proc.HasExited) { $proc.Kill() }
    Write-RunLog "closed_azahar_pid=$($proc.Id)"
}

$collect = Join-Path $root "tools\arm_profile.py"
$collectArgs = @("collect", "--sd-root", $SdRoot, "--out", $runDir,
                 "--azahar-log", (Join-Path $env:APPDATA "Azahar\log\azahar_log.txt"),
                 "--max-log-bytes", "$MaxLogBytes")
$previousErrorPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$collectOutput = & py -3 $collect @collectArgs 2>&1
$collectCode = $LASTEXITCODE
$ErrorActionPreference = $previousErrorPreference
$collectOutput | ForEach-Object { Write-RunLog "collect: $_" }

$emuLog = Join-Path $env:APPDATA "Azahar\log\azahar_log.txt"
if (Test-Path $emuLog) {
    $versionLine = Select-String -Path $emuLog -Pattern "Azahar Version:" | Select-Object -First 1
    if ($versionLine) {
        $versionMatch = [regex]::Match($versionLine.Line, "Azahar Version:\s*([^|\r\n]+)\|\s*([^\r\n]+)")
        if ($versionMatch.Success) {
            $versionText = "$($versionMatch.Groups[1].Value.Trim()) | $($versionMatch.Groups[2].Value.Trim())"
            Write-RunLog "azahar_version_from_log=$versionText"
        }
    }
}

$pass = ($marker -eq "PASS" -and $collectCode -eq 0 -and
         (Test-Path (Join-Path $runDir "armtime_profile.csv")))
Write-RunLog "AZAHAR ARMTIME RESULT: $(if ($pass) { 'PASS' } else { 'FAIL' })"
Write-RunLog "evidence=$runDir"

# Hash the final runner log after all evidence lines have been appended.
$runnerBytes = [IO.File]::ReadAllBytes($runnerLog)
$runnerSha = (Get-FileHash $runnerLog -Algorithm SHA256).Hash
$manifestPath = Join-Path $runDir "archive_manifest.json"
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$runnerItem = [pscustomobject]@{
    name = "runner.log"; source = $runnerLog; original_bytes = $runnerBytes.Length
    archived_bytes = $runnerBytes.Length; truncated = $false; sha256 = $runnerSha
}
$manifest.files = @($manifest.files) + @($runnerItem)
$manifest | Add-Member -NotePropertyName azahar_pid -NotePropertyValue $proc.Id -Force
$manifest | Add-Member -NotePropertyName azahar_version -NotePropertyValue $versionText -Force
$manifest | Add-Member -NotePropertyName source_3dsx_sha256 -NotePropertyValue $sourceHash -Force
$manifest | Add-Member -NotePropertyName guest_marker -NotePropertyValue $marker -Force
$manifest | Add-Member -NotePropertyName elapsed_sec -NotePropertyValue $watch.Elapsed.TotalSeconds -Force
$manifestJson = $manifest | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($manifestPath, $manifestJson, (New-Object System.Text.UTF8Encoding($false)))

$runMutex.ReleaseMutex()
$runMutex.Dispose()
if (-not $pass) { exit 1 }
exit 0
