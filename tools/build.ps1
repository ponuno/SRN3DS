#requires -Version 5.1
<#
.SYNOPSIS
  Reproducible ARM build for one Skyrim_N3DS target.
.DESCRIPTION
  Runs make inside the devkitPro MSYS2 shell, then archives the .elf, .3dsx,
  a SHA256 manifest and the linker log under dist/<target>/.  Every failure
  propagates as a non-zero exit code.
.PARAMETER Mode
  default | graphics | com | boot | lift | pilot
.PARAMETER PilotGen
  Generated pilot directory (contains pilot_config.h). Required for -Mode pilot.
.PARAMETER Opt
  Optimisation flag passed to make (default -O2).
#>
param(
    [ValidateSet("default", "graphics", "com", "boot", "lift", "pilot", "tesv", "audio", "va")]
    [string]$Mode = "default",
    [string]$PilotGen = "",
    [string]$TesvGen = "",
    [string]$Opt = "-O2",
    [string]$Isolated = "",
    [switch]$NoClean,
    [int]$Jobs = 0,
    [switch]$ForceMemProbe,
    [uint32]$TesvArmSteps = 0
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$bash = "C:\devkitPro\msys2\usr\bin\bash.exe"
$devkit = "/opt/devkitpro"
if (-not (Test-Path $bash)) { throw "MSYS2 bash not found: $bash" }

$unixRoot = "/" + ((Split-Path $root -Qualifier).TrimEnd(":")) + ($root.Substring(2) -replace '\\', '/')
$unixRoot = $unixRoot.ToLower()

$modes = @{
    "default"  = @{ Target = "Skyrim_N3DS"; Vars = "" }
    "graphics" = @{ Target = "Skyrim_N3DS_graphics"; Vars = "GRAPHICS=1" }
    "com"      = @{ Target = "Skyrim_N3DS_graphics_com"; Vars = "GRAPHICS_COM=1 LIFT_STAGE=staged" }
    "boot"     = @{ Target = "Skyrim_N3DS_boot"; Vars = "LIFT_STAGE=staged BOOT_SMOKE=1" }
    "lift"     = @{ Target = "Skyrim_N3DS_lift"; Vars = "LIFT_STAGE=staged" }
    "pilot"    = @{ Target = "Skyrim_N3DS_pilot"; Vars = "LIFT_STAGE=staged" }
    "tesv"     = @{ Target = "Skyrim_N3DS_tesv"; Vars = "LIFT_STAGE=staged TESV_SLICE=1" }
    "audio"    = @{ Target = "Skyrim_N3DS_audio"; Vars = "AUDIO=1" }
    "va"       = @{ Target = "Skyrim_N3DS_va"; Vars = "VA_PROBE=1" }
}
if ($Opt -notmatch '^-O[0-3sg]$') { throw "Invalid -Opt '$Opt'; expected -O0..-O3, -Os or -Og" }
$target = $modes[$Mode].Target
$vars = $modes[$Mode].Vars

if ($Mode -ne "default" -and $Mode -ne "graphics" -and $Mode -ne "audio" -and $Mode -ne "va") {
    if (-not (Test-Path (Join-Path $root "staged\lift.py"))) {
        throw "staged/ is missing; run: python tools/stage_upstream.py ../Skyrim_Android staged"
    }
}
if ($Mode -eq "pilot") {
    if (-not $PilotGen) { throw "-PilotGen is required for -Mode pilot" }
    if (-not (Test-Path (Join-Path $PilotGen "pilot_config.h"))) {
        throw "pilot_config.h not found in $PilotGen; run tools/prepare_pilot.py"
    }
    $genUnix = "/" + ((Split-Path (Resolve-Path $PilotGen) -Qualifier).TrimEnd(":")) + ((Resolve-Path $PilotGen).Path.Substring(2) -replace '\\', '/')
    $vars += " PILOT_GEN=`"$genUnix`""
}
if ($Mode -eq "tesv") {
    if (-not $TesvGen) { throw "-TesvGen is required for -Mode tesv" }
    if (-not (Test-Path (Join-Path $TesvGen "tesv_table.c"))) {
        throw "tesv_table.c not found in $TesvGen; lift and instrument a slice first"
    }
    foreach ($f in Get-ChildItem (Join-Path $TesvGen "tesv_*.c")) {
        $text = Get-Content $f.FullName -Raw
        if ($text -match '(?m)^L_[A-Za-z0-9]+:' -and $text -notmatch 'n3ds_pilot_tick') {
            throw "Uninstrumented generated file: $($f.FullName) (run tools/instrument_generated.py)"
        }
    }
    $genPath = (Resolve-Path $TesvGen).Path
    $genUnix = "/" + ((Split-Path $genPath -Qualifier).TrimEnd(":")) + ($genPath.Substring(2) -replace '\\', '/')
    $vars += " TESV_GEN=`"$genUnix`""
}
$vars += " OPT=$Opt"
if ($ForceMemProbe) {
    if ($Mode -ne "tesv") { throw "-ForceMemProbe is only valid for -Mode tesv" }
    $vars += " TESV_FORCE_MEMPROBE=1"
}
if ($TesvArmSteps) {
    if ($Mode -ne "tesv") { throw "-TesvArmSteps is only valid for -Mode tesv" }
    $vars += " TESV_ARM_STEPS=$TesvArmSteps"
}
if ($Isolated) {
    if ($Isolated -notmatch '^[A-Za-z0-9][A-Za-z0-9_]{0,15}$') {
        throw "Invalid -Isolated '$Isolated'; use up to 16 letters, digits or underscores"
    }
    # Give this build its own target name and object directory so parallel agents
    # never overwrite each other's artifacts.
    $target = "${target}_$Isolated"
    $vars += " TARGET=$target BUILD=build_$Isolated"
}

$cleanCmd = if ($NoClean) { "" } else { "make clean $vars && " }
$jobsFlag = if ($Jobs -gt 0) { "-j$Jobs " } else { "" }
$cmd = "cd `"$unixRoot`" && export PATH=$devkit/devkitARM/bin:$devkit/tools/bin:`$PATH && ${cleanCmd}make ${jobsFlag}$vars"

$dist = Join-Path $root "dist\$target"
$bin = Join-Path $root "build\$target"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$buildLog = Join-Path $dist "build.log"
"[$([DateTime]::Now.ToString('s'))] $cmd" | Out-File $buildLog -Encoding utf8

Write-Host "Build: $Mode -> $target"
Write-Host "Command: $cmd"
$output = & $bash -lc $cmd 2>&1
$rc = $LASTEXITCODE
$output | Tee-Object -FilePath $buildLog -Append | Select-Object -Last 15
if ($rc -ne 0) {
    Write-Error "Build failed with exit code $rc (log: $buildLog)"
    exit $rc
}

$elf = Join-Path $root "$target.elf"
$dsx = Join-Path $root "$target.3dsx"
if (-not (Test-Path $elf) -or -not (Test-Path $dsx)) { Write-Error "Missing $target.elf/.3dsx"; exit 1 }
Copy-Item $elf, $dsx -Destination $dist -Force

$devkitWin = "C:\devkitPro"
$size = & "$devkitWin\devkitARM\bin\arm-none-eabi-size.exe" $elf 2>&1
$gccVer = (& "$devkitWin\devkitARM\bin\arm-none-eabi-gcc.exe" --version | Select-Object -First 1)

$sourceFiles = @()
foreach ($pattern in @("Makefile", "source\*", "tests\*", "tools\*", "staged\*")) {
    $sourceFiles += Get-ChildItem -Path (Join-Path $root $pattern) -File -ErrorAction SilentlyContinue
}
$sourceFiles = $sourceFiles | Where-Object { $_.FullName -notmatch '\\build' } | Sort-Object FullName
$sha = [System.Security.Cryptography.SHA256]::Create()
$aggregate = New-Object System.Text.StringBuilder
foreach ($f in $sourceFiles) {
    $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
    $fileHash = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()
    [void]$aggregate.Append($rel).Append(' ').Append($fileHash).Append("`n")
}
$sourceHash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($aggregate.ToString())) | ForEach-Object { $_.ToString('x2') })

$genDir = ""
if ($Mode -eq "tesv" -and $TesvGen) { $genDir = (Resolve-Path $TesvGen).Path }
elseif ($Mode -eq "pilot" -and $PilotGen) { $genDir = (Resolve-Path $PilotGen).Path }
$genHash = ""
if ($genDir) {
    $genAgg = New-Object System.Text.StringBuilder
    foreach ($f in (Get-ChildItem $genDir -File | Sort-Object Name)) {
        [void]$genAgg.Append($f.Name).Append(' ').Append((Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower()).Append("`n")
    }
    $genHash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($genAgg.ToString())) | ForEach-Object { $_.ToString('x2') })
}
$manifest = [ordered]@{
    target = $target
    mode = $Mode
    opt = $Opt
    built_at = [DateTime]::Now.ToString("o")
    command = $cmd
    toolchain = $gccVer
    devkitpro = $devkit
    source_sha256 = $sourceHash
    generated_dir = $genDir
    generated_sha256 = $genHash
    size = $size
    files = @{}
}
foreach ($f in @($elf, $dsx)) {
    $manifest.files[[IO.Path]::GetFileName($f)] = @{
        bytes = (Get-Item $f).Length
        sha256 = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
    }
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $dist "build_manifest.json") -Encoding utf8
Write-Host "Artifacts: $dist"
exit 0
