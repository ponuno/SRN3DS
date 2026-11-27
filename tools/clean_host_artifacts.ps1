# Remove host-test run directories (host_*) from the usual roots and report
# the space freed.  Generated build output only: no source, no game data.
#
#   powershell -File tools\clean_host_artifacts.ps1
#   powershell -File tools\clean_host_artifacts.ps1 -Roots C:\Skyrim_N3DS_build,E:\

param(
    [string[]]$Roots = @("C:\Skyrim_N3DS_build", "E:\",
                        (Join-Path $env:USERPROFILE "Documents\Skyrim_N3DS")),
    [switch]$WhatIfOnly
)

$freed = 0
$removed = 0
# Never touch directories that hold regenerated inputs rather than run output.
$keep = @('host_sm3')
foreach ($root in $Roots) {
    if (-not (Test-Path $root)) { continue }
    Get-ChildItem $root -Directory -Filter 'host_*' -ErrorAction SilentlyContinue |
        ForEach-Object {
            if ($keep -contains $_.Name) { return }
            if (Test-Path (Join-Path $_.FullName '.keep')) { return }
            $bytes = (Get-ChildItem $_.FullName -Recurse -File -ErrorAction SilentlyContinue |
                      Measure-Object Length -Sum).Sum
            if ($WhatIfOnly) {
                Write-Host ("would remove {0} ({1:N1} MB)" -f $_.FullName,
                            ($bytes / 1MB))
            } else {
                Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
                $freed += $bytes
                $removed += 1
            }
        }
}

if (-not $WhatIfOnly) {
    Write-Host ("removed {0} host run director{1}, freed {2:N2} GB" -f
                $removed, $(if ($removed -eq 1) { "y" } else { "ies" }),
                ($freed / 1GB))
}
