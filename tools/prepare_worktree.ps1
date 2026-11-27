# Prepare an isolated worktree for build/harness runs by linking the large
# shared inputs from the main tree.  Large generated inputs live only in the
# main tree; without them a worktree build can fail with a misleading
# guest fault at 0x802fa80 (missing staging/pilot code, not a real CPU bug).
#
# Only the three shared directories below are touched: never source, tests,
# .git, DevCaps or F:.  The record of junctions created by this tool is kept
# in %TEMP%, not in the repository.  Git shows junctions as untracked symlinks
# (ignore rules with a trailing slash do not match them); run -Clean before
# committing, or simply do not stage them.
#
#   powershell -ExecutionPolicy Bypass -File tools\prepare_worktree.ps1
#   powershell -File tools\prepare_worktree.ps1 -Worktree C:\...\wt-prep `
#     -Main C:\Users\vasya\Documents\Skyrim_N3DS
#   powershell -File tools\prepare_worktree.ps1 -Worktree C:\...\wt-prep -Clean

param(
    [string]$Worktree = (Get-Location).Path,
    [string]$Main = 'C:\Users\vasya\Documents\Skyrim_N3DS',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$names = @('staged', 'pilot_out_mult', '.venv')
$required = @(
    'staged\fpu.c',
    'pilot_out_mult\D3DXMatrixMultiply\gen',
    '.venv\Scripts\python.exe'
)

function Get-ItemOrNull([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    return Get-Item -Force -LiteralPath $path
}

function Test-Junction([object]$item) {
    if ($null -eq $item) { return $false }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
        return $false
    }
    $linkType = $item.PSObject.Properties['LinkType']
    if ($linkType -and $linkType.Value) {
        return ([string]$linkType.Value -ieq 'Junction')
    }
    # PowerShell without LinkType: NTFS junction reparse tag 0xA0000003.
    $query = & fsutil reparsepoint query $item.FullName 2>$null
    return ($LASTEXITCODE -eq 0 -and (($query -join ' ') -match '0xa0000003'))
}

function Get-Target([object]$item) {
    $prop = $item.PSObject.Properties['Target']
    if ($prop -and $prop.Value) {
        $target = @($prop.Value)[0]
        try {
            return (Resolve-Path -LiteralPath $target -ErrorAction Stop).ProviderPath
        } catch {
            return $target
        }
    }
    $query = & fsutil reparsepoint query $item.FullName 2>$null
    if ($LASTEXITCODE -eq 0) {
        $line = $query | Where-Object { $_ -match 'Substitute Name:' } |
                Select-Object -First 1
        if ($line) {
            $target = $line -replace '^.*Substitute Name:\s*', ''
            if ($target -match '^\\\\\?\?\\(.+)$') { $target = $Matches[1] }
            return $target.TrimEnd('\')
        }
    }
    return $null
}

function Test-UnderPath([string]$child, [string]$root) {
    $prefix = $root.TrimEnd('\') + '\'
    return $child.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Read-State([string]$file) {
    # ConvertFrom-Json emits an array as one pipeline object in PowerShell 5.1;
    # unwrap it so foreach sees one entry per junction.
    $parsed = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
    if ($null -eq $parsed) { return @() }
    return @($parsed)
}

try {
    $wt = (Resolve-Path -LiteralPath $Worktree -ErrorAction Stop).ProviderPath.TrimEnd('\')
    $srcRoot = (Resolve-Path -LiteralPath $Main -ErrorAction Stop).ProviderPath.TrimEnd('\')
} catch {
    Write-Error "bad -Worktree/-Main path: $($_.Exception.Message)"
    exit 2
}

# Key the state file by worktree path; no file is created inside the repo.
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $digest = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($wt.ToLowerInvariant()))
    $key = ([BitConverter]::ToString($digest) -replace '-', '').Substring(0, 16)
} finally {
    $sha.Dispose()
}
$stateDir = Join-Path $env:TEMP 'prepare_worktree_state'
$stateFile = Join-Path $stateDir "$key.json"

if ($Clean) {
    if (-not (Test-Path -LiteralPath $stateFile)) {
        Write-Host "no junctions created by this tool for $wt (nothing to clean)."
        exit 0
    }
    $state = @(Read-State $stateFile)
    $removed = 0
    foreach ($entry in $state) {
        $path = [string]$entry.Path
        $target = [string]$entry.Target
        $name = [string]$entry.Name
        if ($names -notcontains $name -or -not (Test-UnderPath $path $wt)) {
            Write-Warning "unsafe state entry, skipped: $path"
            continue
        }
        $item = Get-ItemOrNull $path
        if ($null -eq $item) {
            Write-Host "already absent: $path"
            continue
        }
        if (-not (Test-Junction $item)) {
            # A real directory (or a copy) must never be deleted here.
            Write-Warning "not a junction, left untouched: $path"
            continue
        }
        $actual = Get-Target $item
        if ($actual.TrimEnd('\') -ine $target.TrimEnd('\')) {
            Write-Warning "junction target changed ($actual), left untouched: $path"
            continue
        }
        # Non-recursive delete of the reparse point itself: never follows it.
        [IO.Directory]::Delete($path, $false)
        if (Test-Path -LiteralPath $path) {
            Write-Warning "junction still present after delete: $path"
            continue
        }
        $removed++
        $source = Join-Path $srcRoot $name
        Write-Host ("removed junction {0} -> {1} (source intact: {2})" -f
                    $path, $actual, (Test-Path -LiteralPath $source))
    }
    Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
    Write-Host ("cleaned {0} junction(s)." -f $removed)
    exit 0
}

if (-not (Test-Path -LiteralPath $stateDir)) {
    New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
}
$created = @()

foreach ($name in $names) {
    $source = Join-Path $srcRoot $name
    $dest = Join-Path $wt $name
    if (-not (Test-Path -LiteralPath $source)) {
        Write-Warning "main input missing, nothing to link: $source"
        continue
    }
    $existing = Get-ItemOrNull $dest
    if ($null -ne $existing) {
        if (Test-Junction $existing) {
            $target = Get-Target $existing
            if ($target.TrimEnd('\') -ieq $source.TrimEnd('\')) {
                Write-Host "already linked: $dest -> $target"
            } else {
                Write-Warning "existing junction to $target, left untouched: $dest"
            }
        } elseif ($existing.PSIsContainer) {
            Write-Host "present as a real directory, left untouched: $dest"
        } else {
            Write-Warning "path exists as a file, left untouched: $dest"
        }
        continue
    }
    try {
        New-Item -ItemType Junction -Path $dest -Value $source -ErrorAction Stop |
            Out-Null
        $created += [pscustomobject]@{
            Name = $name
            Path = $dest
            Target = $source
        }
        Write-Host "linked: $dest -> $source"
    } catch {
        Write-Warning ("junction failed for {0} ({1}); copying instead" -f
                       $name, $_.Exception.Message)
        try {
            Copy-Item -LiteralPath $source -Destination $dest -Recurse -Force `
                -ErrorAction Stop
            Write-Host "copied (not tracked by -Clean): $source -> $dest"
        } catch {
            Write-Warning "copy failed for ${name}: $($_.Exception.Message)"
        }
    }
}

if ($created.Count -gt 0) {
    $previous = @()
    if (Test-Path -LiteralPath $stateFile) {
        $previous = @(Read-State $stateFile)
    }
    # Keep an earlier record too: a second run must not orphan our junctions.
    ($previous + $created) | ConvertTo-Json -Depth 3 |
        Set-Content -LiteralPath $stateFile -Encoding UTF8
}

$missing = 0
foreach ($rel in $required) {
    $probe = Join-Path $wt $rel
    if (Test-Path -LiteralPath $probe) {
        Write-Host "[OK] $rel"
    } else {
        Write-Warning ("[MISSING] {0} - build may fail with a false 0x802fa80" -f $rel)
        $missing++
    }
}

if ($missing -gt 0) {
    Write-Warning ("worktree not fully prepared: {0} required input(s) missing" -f $missing)
    exit 1
}
Write-Host 'worktree ready for build.'
exit 0
