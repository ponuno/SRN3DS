#requires -Version 5.1
<#
.SYNOPSIS
  Print a compact first-cause summary of one or more TESV slice logs.

.DESCRIPTION
  Reads a host-slice .log (and its paired .err when present) and prints:
    * the first Guest fault / unmapped line with two lines of context;
    * counts of DrawPrimitive*, DrawIndexed* and Present line matches;
    * the last unique [menu] enter addresses;
    * the last mem tag=fault|exit line;
    * effective line count and byte size.

  The file named by -Log is the baseline. A sibling .err/.log is folded in only
  when it is not already embedded in the baseline (host runners often tee the
  guest stderr into stdout, so both captures can hold the same lines). Counts
  are literal line matches of the tokens below, not decoded D3D9 calls: header
  lines such as "Present scales to 400x240" are counted too.

.PARAMETER Log
  One or more log paths. A paired <name>.err (or <name>.log for an .err input)
  is picked up automatically.

.PARAMETER Tail
  How many most-recent unique [menu] enter addresses to print (default 12).

.EXAMPLE
  tools\summarize_run.ps1 -Log E:\menu_capture\last_run.log

.EXAMPLE
  tools\summarize_run.ps1 -Log E:\frame_run\last_run.log -Log E:\menu_draw\last_run.log -Tail 20
#>
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Log,
    [ValidateRange(1, 1024)]
    [int]$Tail = 12
)

$ErrorActionPreference = 'Stop'

# `-File script.ps1 -Log a.log,b.log` passes one comma-joined string, while an
# interactive `-Log a.log,b.log` is a real two-element array. Normalize both.
$paths = New-Object 'System.Collections.Generic.List[string]'
foreach ($entry in $Log) {
    if ($entry -and $entry.Contains(',') -and -not [System.IO.File]::Exists($entry)) {
        foreach ($part in $entry.Split(',')) {
            if (-not [string]::IsNullOrWhiteSpace($part)) { $paths.Add($part.Trim()) }
        }
    } elseif (-not [string]::IsNullOrWhiteSpace($entry)) {
        $paths.Add($entry)
    }
}

$rxFault   = 'Guest fault|unmapped'
$rxDrawP   = '\bDrawPrimitive(?:UP)?\b'
$rxDrawI   = '\bDrawIndexedPrimitive(?:UP)?\b'
$rxPresent = '\bPresent\b'
$rxMenu    = '^\[menu\] enter\s+([0-9a-fA-F]+)'
$rxMemTag  = '\bmem tag=(fault|exit)\b'

function Read-Lines([string]$Path) {
    if (-not [System.IO.File]::Exists($Path)) { return $null }
    return ,([System.IO.File]::ReadAllLines($Path))
}

function Test-TailEquals([string[]]$Outer, [string[]]$Inner) {
    if ($null -eq $Outer -or $null -eq $Inner) { return $false }
    if ($Inner.Count -eq 0 -or $Inner.Count -gt $Outer.Count) { return $false }
    $off = $Outer.Count - $Inner.Count
    for ($i = 0; $i -lt $Inner.Count; $i++) {
        if ($Outer[$off + $i] -cne $Inner[$i]) { return $false }
    }
    return $true
}

function New-Records([string[]]$Lines, [string]$Source) {
    $list = New-Object 'System.Collections.Generic.List[object]'
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $list.Add([pscustomobject]@{ Text = $Lines[$i]; Src = $Source; Num = $i + 1 })
    }
    $list.ToArray()
}

function Format-Bytes([long]$Bytes) {
    if ($Bytes -lt 1024) { return "$Bytes B" }
    if ($Bytes -lt 1MB) { return "$Bytes B ({0:N1} KiB)" -f ($Bytes / 1KB) }
    return "$Bytes B ({0:N2} MiB)" -f ($Bytes / 1MB)
}

$totalLines = 0L
$totalBytes = 0L
$inputs = 0

foreach ($raw in $paths) {
    $inputs++

    try {
        $full = [System.IO.Path]::GetFullPath($raw)
    } catch {
        Write-Warning "skip invalid path '$raw': $($_.Exception.Message)"
        continue
    }
    $primary = $full
    if (-not [System.IO.File]::Exists($primary) -and
        $full.EndsWith('.log', [StringComparison]::OrdinalIgnoreCase)) {
        $cand = $full + '.err'
        if ([System.IO.File]::Exists($cand)) { $primary = $cand }
    }
    if (-not [System.IO.File]::Exists($primary)) {
        Write-Warning "skip missing log: $raw"
        continue
    }

    $partner = $null
    if ($primary.EndsWith('.err', [StringComparison]::OrdinalIgnoreCase)) {
        $cand = $primary.Substring(0, $primary.Length - 4)
        if ([System.IO.File]::Exists($cand)) { $partner = $cand }
    } else {
        $cand = $primary + '.err'
        if ([System.IO.File]::Exists($cand)) { $partner = $cand }
    }

    $mainLines = Read-Lines $primary
    if ($null -eq $mainLines) { Write-Warning "cannot read: $primary"; continue }
    $mainCount = $mainLines.Count
    $mainItem = Get-Item -LiteralPath $primary
    $bytes = $mainItem.Length
    $partnerItem = $null
    $note = 'no paired .err'
    $appendPartner = $false

    if ($partner) {
        $partnerLines = Read-Lines $partner
        $partnerItem = Get-Item -LiteralPath $partner
        $partnerCount = if ($null -eq $partnerLines) { 0 } else { $partnerLines.Count }
        if ($null -ne $partnerLines) {
            if (Test-TailEquals $mainLines $partnerLines) {
                $note = "paired $([System.IO.Path]::GetFileName($partner)) already embedded in baseline; not counted twice"
            } elseif (Test-TailEquals $partnerLines $mainLines) {
                $mainLines = $partnerLines
                $mainCount = $partnerLines.Count
                $primary = $partner
                $mainItem = $partnerItem
                $bytes = $mainItem.Length
                $note = "baseline is embedded in $([System.IO.Path]::GetFileName($partner)); using the superset"
            } else {
                $appendPartner = $true
                $bytes += $partnerItem.Length
                $note = "merged distinct $([System.IO.Path]::GetFileName($partner)) stream"
            }
        }
    }

    $records = @(New-Records $mainLines $primary)
    if ($appendPartner) {
        $records = @($records) + @(New-Records $partnerLines $partner)
    }

    $lineCount = $records.Count
    $totalLines += $lineCount
    $totalBytes += $bytes

    $nDrawP = 0; $nDrawI = 0; $nPresent = 0
    $menuOrder = New-Object 'System.Collections.Generic.List[string]'
    $menuSeen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $lastMem = $null
    $faultIndex = -1

    for ($i = 0; $i -lt $records.Count; $i++) {
        $text = $records[$i].Text
        if ($faultIndex -lt 0 -and $text -match $rxFault) { $faultIndex = $i }
        if ($text -match $rxDrawP) { $nDrawP++ }
        if ($text -match $rxDrawI) { $nDrawI++ }
        if ($text -match $rxPresent) { $nPresent++ }
        $m = [regex]::Match($text, $rxMenu)
        if ($m.Success) {
            $addr = $m.Groups[1].Value
            if ($menuSeen.Contains($addr)) { $null = $menuOrder.Remove($addr) }
            else { $null = $menuSeen.Add($addr) }
            $menuOrder.Add($addr)
        }
        if ($text -match $rxMemTag) { $lastMem = $records[$i] }
    }

    Write-Output "===== $($mainItem.FullName) ====="
    Write-Output "lines:  $lineCount"
    Write-Output "size:   $(Format-Bytes $bytes)"
    Write-Output "input:  $($mainItem.Name) ($($mainItem.Length) B, $mainCount lines)"
    if ($partnerItem) {
        Write-Output "pair:   $($partnerItem.Name) ($($partnerItem.Length) B, $partnerCount lines)"
    }
    Write-Output "note:   $note"
    Write-Output ''

    Write-Output 'first fault (Guest fault / unmapped):'
    if ($faultIndex -lt 0) {
        Write-Output '  none'
    } else {
        $lo = [Math]::Max(0, $faultIndex - 2)
        $hi = [Math]::Min($records.Count - 1, $faultIndex + 2)
        for ($i = $lo; $i -le $hi; $i++) {
            $r = $records[$i]
            $mark = if ($i -eq $faultIndex) { '>>' } else { '  ' }
            Write-Output ("  {0} {1}:{2}: {3}" -f $mark, [System.IO.Path]::GetFileName($r.Src), $r.Num, $r.Text)
        }
    }
    Write-Output ''

    Write-Output 'draw / frame line matches:'
    Write-Output "  DrawPrimitive*       $nDrawP"
    Write-Output "  DrawIndexed*         $nDrawI"
    Write-Output "  Present              $nPresent"
    Write-Output ''

    $shown = [Math]::Min($Tail, $menuOrder.Count)
    $start = $menuOrder.Count - $shown
    Write-Output ("last [menu] enter addresses (unique, {0} of {1}):" -f $shown, $menuOrder.Count)
    if ($shown -eq 0) {
        Write-Output '  none'
    } else {
        for ($i = $start; $i -lt $menuOrder.Count; $i++) {
            Write-Output ("  {0}" -f $menuOrder[$i])
        }
    }
    Write-Output ''

    Write-Output 'last mem tag=fault|exit:'
    if ($null -eq $lastMem) {
        Write-Output '  none'
    } else {
        Write-Output ("  {0}:{1}: {2}" -f [System.IO.Path]::GetFileName($lastMem.Src), $lastMem.Num, $lastMem.Text)
    }
    Write-Output ''
}

if ($inputs -gt 1) {
    Write-Output ("TOTAL: {0} input(s), {1} effective lines, {2}" -f $inputs, $totalLines, (Format-Bytes $totalBytes))
}
