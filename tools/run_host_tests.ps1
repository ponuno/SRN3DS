#requires -Version 5.1
<#
.SYNOPSIS
  Build and run every host-side smoke test with clang + MSVC runtime.
.DESCRIPTION
  Uses the Visual Studio environment for Windows headers/libraries and clang
  for GNU11 C. Each test is compiled into its own directory under build_host
  and executed from a clean run directory so fixtures do not leak into the
  repository. Returns a non-zero exit code if any test fails.
.PARAMETER Only
  Restrict to one or more test names (comma separated).
.PARAMETER Sanitize
  Add -fsanitize=address,undefined where supported.
#>
param(
    [string[]]$Only = @(),
    [switch]$Sanitize,
    [string]$Python = "",
    [string]$PilotGen = "",
    [string]$PilotDll = "C:\Windows\SysWOW64\D3DX9_42.dll",
    [string]$OutDir = "",
    [string]$TesvSliceGen = "",
    [string]$TesvIrc = "",
    [string]$TesvExe = "E:\skyr\TESV.exe",
    [switch]$AllowDiagnosticFault,
    [int]$TestTimeoutSec = 300
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $TesvIrc) { $TesvIrc = $env:N3DS_IRC_PATH }

$vsRoot = "C:\Program Files\Microsoft Visual Studio\18\Community"
$vsVars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
$clang = "C:\Program Files\LLVM\bin\clang.exe"
if (-not (Test-Path $clang)) { throw "clang not found at $clang" }
if (-not (Test-Path $vsVars)) { throw "vcvars64.bat not found at $vsVars" }

# Import the MSVC environment once instead of calling vcvars64.bat per test.
# Two concurrent vcvars invocations can deadlock, and each costs several seconds.
$vsDump = & C:\Windows\System32\cmd.exe /c "call `"$vsVars`" >nul && set" 2>$null
$vsEnv = @{}
foreach ($line in $vsDump) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { $vsEnv[$line.Substring(0, $i)] = $line.Substring($i + 1) }
}
if (-not $vsEnv.ContainsKey('INCLUDE') -or -not $vsEnv.ContainsKey('LIB')) {
    throw "vcvars64.bat did not produce INCLUDE/LIB"
}
foreach ($k in $vsEnv.Keys) { Set-Item -Path "env:$k" -Value $vsEnv[$k] }

if (-not $Python) {
    $candidate = Join-Path $root ".venv\Scripts\python.exe"
    $Python = if (Test-Path $candidate) { $candidate } else { "python" }
}

$outDir = if ($OutDir) { $OutDir } else { Join-Path $root "build_host" }
# TEMP/TMP must be absolute: relative paths break the CRT's tmpfile_s in the
# steam/registry smokes. Resolve a relative -OutDir against the caller's cwd.
if (-not [System.IO.Path]::IsPathRooted($outDir)) {
    $outDir = Join-Path (Get-Location).Path $outDir
}
$outDir = [System.IO.Path]::GetFullPath($outDir)
$logDir = Join-Path $outDir "logs"
$tmpDir = Join-Path $outDir "tmp"
New-Item -ItemType Directory -Force -Path $outDir, $logDir, $tmpDir | Out-Null
$env:TEMP = $tmpDir
$env:TMP = $tmpDir
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runId = "$stamp-$PID"
$log = Join-Path $logDir "host_tests_$runId.log"

function Write-Log([string]$text) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $text
    Write-Host $line
    Add-Content -Path $log -Value $line
}

function Invoke-TestProcess([string]$file, [string[]]$argv, [string]$workDir, [int]$timeoutSec) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $file
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($argv.Count -gt 0) {
        $psi.Arguments = ($argv | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' '
    }
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $null = $proc.Start()
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($timeoutSec * 1000)) {
        try { $proc.Kill() } catch { }
        $proc.WaitForExit(5000) | Out-Null
        return @{ Code = -1; Output = @("TIMEOUT after ${timeoutSec}s"); TimedOut = $true }
    }
    $lines = @()
    $lines += ($outTask.Result -split "`r?`n")
    $lines += ($errTask.Result -split "`r?`n")
    $lines = $lines | Where-Object { $_ -ne "" }
    return @{ Code = $proc.ExitCode; Output = $lines; TimedOut = $false }
}

function Invoke-ClangParallel([string]$Clang, [string[]]$Flags, [string[]]$Sources, [string]$OutFile, [string]$WorkDir, [int]$Jobs) {
    $objDir = Join-Path $WorkDir 'parobj'
    Remove-Item -Recurse -Force $objDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $compileFlags = @($Flags | Where-Object { $_ -notmatch '^-Wl,' })
    $flagText = ($compileFlags -join ' ')
    $active = New-Object System.Collections.Generic.List[object]
    $objs = New-Object System.Collections.Generic.List[string]
    $allOut = New-Object System.Collections.Generic.List[string]
    $failed = $false
    $idx = 0

    function Start-Clang([string]$ArgText) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Clang
        $psi.Arguments = $ArgText
        $psi.WorkingDirectory = (Get-Location).Path
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $p = New-Object System.Diagnostics.Process
        $p.StartInfo = $psi
        $null = $p.Start()
        return [pscustomobject]@{
            Proc = $p
            OutTask = $p.StandardOutput.ReadToEndAsync()
            ErrTask = $p.StandardError.ReadToEndAsync()
        }
    }
    function Collect-Clang($r, [ref]$FailedRef) {
        $r.Proc.WaitForExit()
        foreach ($t in @($r.OutTask,$r.ErrTask)) {
            $txt = $t.Result
            if ($txt) { $allOut.AddRange([string[]]($txt -split "`r?`n")) }
        }
        if ($r.Proc.ExitCode -ne 0) {
            $FailedRef.Value = $true
            $allOut.Add("COMPILE/LINK FAILED (exit $($r.Proc.ExitCode))")
        }
    }

    foreach ($src in $Sources) {
        while ($active.Count -ge $Jobs) {
            $r = $active[0]; $active.RemoveAt(0); Collect-Clang $r ([ref]$failed)
        }
        $obj = Join-Path $objDir ("{0:D3}_{1}.o" -f $idx, ([IO.Path]::GetFileNameWithoutExtension($src)))
        $argText = "$flagText `"$src`" -c -o `"$obj`""
        $active.Add((Start-Clang $argText))
        $objs.Add($obj)
        ++$idx
    }
    while ($active.Count -gt 0) {
        $r = $active[0]; $active.RemoveAt(0); Collect-Clang $r ([ref]$failed)
    }
    if ($failed) { return @{ Code = 1; Output = @($allOut) } }

    $objText = (($objs | ForEach-Object { "`"$_`"" }) -join ' ')
    $linkText = (($Flags -join ' ') + ' ' + $objText + ' -o "' + $OutFile + '"')
    $lr = Start-Clang $linkText
    Collect-Clang $lr ([ref]$failed)
    return @{ Code = if($failed){1}else{0}; Output = @($allOut) }
}

$common = @("-std=gnu11", "-O1", "-g", "-Isource", "-Wno-deprecated-declarations", "-Wno-format", "-Wno-unused-function", "-Wno-unused-variable")
if ($Sanitize) {
    $rt = Get-ChildItem "C:\Program Files\LLVM\lib\clang\*\lib\windows" -Directory -ErrorAction SilentlyContinue |
        Sort-Object { try { [version]$_.Parent.Parent.Name } catch { [version]"0.0" } } -Descending |
        Select-Object -First 1
    if ($rt) { $env:PATH = $rt.FullName + ";" + $env:PATH }
    $common += @("-fsanitize=address", "-fno-omit-frame-pointer")
}

$liftCommon = @("-std=gnu11", "-O1", "-g", "-Istaged", "-Isource", "-fwrapv", "-fno-strict-aliasing", "-Wno-deprecated-declarations", "-Wno-format", "-Wl,/STACK:67108864")
if ($env:N3DS_WATCH_STORE_CHECKS -eq '1') { $liftCommon += '-DN3DS_WATCH_STORE_CHECKS' }
if ($Sanitize) { $liftCommon = $common + @("-Istaged", "-fwrapv", "-fno-strict-aliasing") }

$legacySources = @("source/guest.c", "source/guest_heap.c", "source/pe_image.c", "source/pe_imports.c", "source/win32_shims.c", "source/win32_sync.c", "source/win32_user.c", "source/win32_locale.c", "source/win32_shell.c", "source/win32_audio.c", "source/win32_audio_3ds.c", "source/win32_thread.c", "source/win32_hostctx.c", "source/win32_input.c", "source/win32_steam.c", "source/win32_registry.c", "source/n3ds_lift_runtime.c", "source/ir_core.c", "source/ir_decode.c", "source/ir_exec.c", "source/ir_container.c", "source/rt_prof.c", "staged/fpu.c")
$comSources = $legacySources + @("source/guest_process.c", "source/n3ds_boot.c")
$d3dSources = @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c")
$d3dFull = $d3dSources + @("source/d3d9_shader.c", "source/d3d9_tev.c")
$tevSources = $d3dSources + @("source/d3d9_tev.c", "source/d3d9_menu_light.c")
$picaSources = @("source/d3d9_pica_tev.c")
$pipelineSources = @("source/d3d9_pipeline_match.c", "source/d3d9_pipeline_registry.c", "source/d3d9_menu_light.c")

$tests = [ordered]@{}
function Add-Test($name, $flags, $sources, $testArgs) {
    $tests[$name] = @{ Flags = $flags; Sources = $sources; Args = $testArgs }
}

Add-Test "host" $common @("source/probe.c", "source/guest.c", "source/pe_image.c", "source/guest_heap.c", "source/guest_call.c", "tests/host.c") @()
Add-Test "tesv_tls_policy" $common @("tests/tesv_tls_policy.c") @()
Add-Test "tesv_sm3_probe" $common @("source/guest.c", "source/tesv_sm3_probe.c", "tests/tesv_sm3_probe_smoke.c") @()
Add-Test "tesv_cube_probe" $common @("source/guest.c", "source/tesv_cube_probe.c", "tests/tesv_cube_probe_smoke.c") @()
Add-Test "tesv_cecdc0_probe" $common @("source/guest.c", "source/tesv_cecdc0_probe.c", "tests/tesv_cecdc0_probe_smoke.c") @()
Add-Test "tesv_fault_regs" $liftCommon ($legacySources + @("tests/tesv_fault_regs_smoke.c")) @()
Add-Test "tesv_cube_desc_probe" $liftCommon ($legacySources + @("tests/tesv_cube_desc_probe_smoke.c")) @()
Add-Test "pe_imports" $common @("source/guest.c", "source/pe_image.c", "source/pe_imports.c", "tests/pe_imports_smoke.c") @()
Add-Test "lift" $liftCommon ($legacySources + @("tests/lift_smoke.c", "tests/win32_smoke.c")) @()
Add-Test "boot" $liftCommon ($comSources + @("tests/boot_smoke.c", "tests/win32_smoke.c")) @()
Add-Test "graphics" $common ($d3dSources + @("source/d3d9_demo_scene.c", "tests/graphics_smoke.c")) @()
Add-Test "vertex" $common ($d3dSources + @("tests/vertex_smoke.c")) @()
Add-Test "texture_residency" $common @("source/d3d9_texcache.c", "tests/texture_residency_smoke.c") @()
Add-Test "texture_residency_upload" $common @("source/d3d9_texcache.c", "source/d3d9_texture_data.c", "tests/texture_residency_upload_smoke.c") @()
Add-Test "texture_container" $common @("source/d3d9_texture_data.c", "tests/texture_container_smoke.c") @()
Add-Test "texture_runtime" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "tests/texture_runtime_smoke.c") @()
Add-Test "bc_shader" $common ($d3dFull + @("tests/bc_shader_smoke.c")) @()
Add-Test "tev_translate" $common ($tevSources + @("tests/tev_translate_smoke.c")) @()
Add-Test "tev_accuracy" $common ($tevSources + @("tests/tev_accuracy_smoke.c")) @()
Add-Test "menu_light" $common ($tevSources + @("tests/menu_light_smoke.c")) @()
Add-Test "pica_tev" $common @("source/d3d9_tev.c", "source/d3d9_menu_light.c", "source/d3d9_pica_tev.c", "tests/pica_tev_smoke.c") @()
Add-Test "tev_com" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/tev_com_smoke.c")) @()
Add-Test "uicopy_host" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_capture.c") + $picaSources + $pipelineSources + @("tests/isolated/uicopy/host_smoke.c")) @()
Add-Test "com" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/com_smoke.c")) @()
Add-Test "device_extra_methods" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/device_extra_methods_smoke.c")) @()
Add-Test "patch_process_refusals" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/patch_process_refusals_smoke.c")) @()
Add-Test "device_ps_constants" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/device_ps_constants_smoke.c")) @()
Add-Test "device_vs_constants" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/device_vs_constants_smoke.c")) @()
Add-Test "query" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/query_smoke.c")) @()
Add-Test "device_misc_state" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/device_misc_state_smoke.c")) @()
Add-Test "state_block" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/state_block_smoke.c")) @()
Add-Test "menu_text_draw" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c", "source/menu_swf_batch.c", "source/d3d9_menu_text.c", "tests/menu_text_draw_smoke.c") + $picaSources) @()
Add-Test "tesv_init_contract" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/tesv_init_contract_smoke.c")) @()
Add-Test "private_data" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/private_data_smoke.c")) @()
Add-Test "tev_text_plan" $common @("source/d3d9_tev.c", "source/d3d9_pica_tev.c", "source/d3d9_adapter.c", "tests/tev_text_plan_smoke.c") @()
Add-Test "menu_text_plan_draw" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/menu_text_plan_draw_smoke.c")) @()
Add-Test "cube_rt" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/cube_rt_smoke.c")) @()
# RT-cube sampling readiness: rendered faces are sampleable; incomplete/no-draw_cube refused.
Add-Test "cube_rt_sampling" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/cube_rt_sampling_smoke.c")) @()
Add-Test "swap_chain" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/swap_chain_smoke.c")) @()
Add-Test "device_cursor_palette" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/device_cursor_palette_smoke.c")) @()
Add-Test "com_postcaps" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/com_postcaps_smoke.c")) @()
Add-Test "render_texture" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/render_texture_smoke.c")) @()
Add-Test "render_texture_hdr" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/render_texture_hdr_smoke.c")) @()
Add-Test "adapter" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/adapter_smoke.c")) @()
Add-Test "device_create" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/device_create_smoke.c")) @()
Add-Test "d3dx_texture_shims" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/d3dx_texture_shims_smoke.c")) @()
# D3DX cube-map loader + IDirect3DCubeTexture9 COM path; null backend face pixels.
Add-Test "d3dx_cube_texture_shims" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/d3dx_cube_texture_smoke.c")) @()
# D3DX volume loader + IDirect3DVolumeTexture9 LockBox/UnlockBox; null slices.
Add-Test "d3dx_volume_texture_shims" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/d3dx_volume_texture_smoke.c")) @()
# Cube/volume D9TexCache residency, accounting, retryable release and no-evict.
Add-Test "cube_volume_residency" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_null.c", "tests/cube_volume_residency_smoke.c")) @()
# File-path cube/volume D3DX exports through the sandbox + shared DDS parser.
Add-Test "d3dx_file_cube_volume" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/d3dx_file_cube_volume_smoke.c")) @()
# Cube-map draw sampling via the optional draw_cube backend callback.
Add-Test "cube_sampling" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/cube_sampling_smoke.c")) @()
# PICA cube backend contract through the core path (recording backend).
Add-Test "pica_cube_texture" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_null.c", "tests/pica_cube_texture_smoke.c")) @()
# UpdateTexture / UpdateSurface / ColorFill CPU-store family.
Add-Test "texture_update" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/texture_update_smoke.c")) @()
# GetRenderTargetData readback into a CPU offscreen surface; front-buffer refusal.
Add-Test "rt_readback" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/rt_readback_smoke.c")) @()
# Measurement-only shader-skip probe (unknown shader -> non-executable object, draw refusal).
Add-Test "shader_skip_probe" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/shader_skip_probe_smoke.c")) @()
Add-Test "render_target" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "tests/render_target_smoke.c") @()
Add-Test "render_target_bounded" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_null.c", "tests/render_target_bounded_smoke.c") @()
Add-Test "render_target_budget" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "tests/render_target_budget_smoke.c") @()
Add-Test "scaled_target_blit" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_null.c", "tests/scaled_target_blit_smoke.c") @()
Add-Test "scaled_readback" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_null.c", "tests/scaled_readback_smoke.c") @()
Add-Test "render_texture_sample" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "tests/render_texture_sample_smoke.c") @()
Add-Test "stretchrect" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "tests/stretchrect_smoke.c") @()
Add-Test "up_draw" $common ($d3dSources + @("tests/up_draw_smoke.c")) @()
Add-Test "null_backend" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_null.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/null_backend_smoke.c")) @()
Add-Test "back_buffer_lockable" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_null.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/back_buffer_lockable_smoke.c")) @()
Add-Test "depth_rt" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_null.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/depth_rt_smoke.c")) @()
Add-Test "tnl_proto_off" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_null.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/tnl_proto_smoke.c")) @("off")
Add-Test "tnl_proto" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_null.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/tnl_proto_smoke.c")) @("on")
# Plain D3DX9_42 surface/shader exports: FromMemory/FromSurface pixel copies and
# the named E_NOTIMPL CompileShader path, driven through win32_resolve + COM.
Add-Test "d3dx_surface_shims" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_null.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3dx_surface_shims.c", "source/d3dx_texture_shims.c") + $picaSources + @("tests/d3dx_surface_shims_smoke.c")) @()
Add-Test "stretchrect_com" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_null.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/stretchrect_com_smoke.c")) @()
Add-Test "stretchrect_com_sw" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_null.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c") + $picaSources + @("tests/stretchrect_com_sw_smoke.c")) @()
Add-Test "gfxmesh_core" $common @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "tests/gfxmesh_core_smoke.c") @()
Add-Test "gfxmesh_com" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/gfxmesh_com_smoke.c")) @()
# Vertex-colour PICA half: stage-0 -> single TEV stage against a CPU reference,
# exact null-backend RGBA capture, and the COM-level refusal HRESULT.
Add-Test "color_tev" $common ($d3dSources + @("source/d3d9_color_tev.c", "tests/d3d9_color_tev_smoke.c")) @()
Add-Test "two_stage" $common ($d3dSources + @("tests/two_stage_smoke.c")) @()
Add-Test "texture_surface" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("tests/texture_surface_smoke.c")) @()
Add-Test "color_com" $liftCommon ($legacySources + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_null.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_demo_scene.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_com_demo.c") + $picaSources + @("source/d3d9_color_tev.c", "tests/d3d9_color_com_smoke.c")) @()
$menuClosureFixture = Join-Path $root "tests/fixtures/menu_closure_real_shaders.h"
if (Test-Path $menuClosureFixture) {
    Add-Test "menu_closure" $common @("source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_menu_light.c", "source/d3d9_pica_tev.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "tests/menu_closure_contract_smoke.c") @()
} else {
    Write-Log "menu_closure: tests/fixtures/menu_closure_real_shaders.h is absent; run tools/extract_closure_fixtures.py with your Shaders.bsa to enable this test"
}

# Optional: measure the TEV translator over chunks extracted from the retail
# archive.  The dump is a build artefact (tools/shaders/report.py dump).
$tevChunks = $env:SKYRIM_N3DS_TEV_CHUNKS
if (-not $tevChunks) {
    $candidate = "E:\Skyrim_N3DS_build\assets\shaders\tev_chunks.bin"
    if (Test-Path $candidate) { $tevChunks = $candidate }
}
if ($tevChunks -and (Test-Path $tevChunks)) {
    Add-Test "tev_real" $common ($tevSources + @("tests/tev_real_smoke.c")) @($tevChunks)
    Write-Log "TEV real-chunk measurement uses $tevChunks"
}
Add-Test "sync" $liftCommon @("source/guest.c", "source/win32_sync.c", "tests/win32_sync_smoke.c") @()
Add-Test "thread" $liftCommon ($legacySources + @("tests/win32_thread_smoke.c")) @()
Add-Test "thread_coop" $liftCommon ($legacySources + @("tests/thread_coop.c")) @()
Add-Test "thread_coop_asm" ($liftCommon + @("-DWIN32_HOSTCTX_BACKEND=3")) ($legacySources + @("tests/thread_coop.c")) @()
Add-Test "input" $liftCommon ($legacySources + @("tests/win32_input_smoke.c")) @()
Add-Test "user" $liftCommon @("source/guest.c", "source/win32_user.c", "tests/win32_user_smoke.c") @()
Add-Test "locale" $liftCommon @("source/guest.c", "source/win32_locale.c", "tests/win32_locale_smoke.c") @()
Add-Test "shell" $liftCommon @("source/guest.c", "source/win32_shell.c", "tests/win32_shell_smoke.c") @()
Add-Test "audio" $liftCommon @("source/guest.c", "source/win32_audio.c", "tests/audio_smoke.c") @()
Add-Test "steam" $liftCommon @("source/guest.c", "source/win32_steam.c", "tests/win32_steam_smoke.c") @()
Add-Test "registry" $liftCommon @("source/guest.c", "source/win32_registry.c", "tests/win32_registry_smoke.c") @()
Add-Test "renderer_requirements" $common @("source/d3d9_renderer_requirements.c", "tests/renderer_requirements_smoke.c") @()
Add-Test "menu_bsa" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_bsa_runtime.c", "source/guest.c", "source/guest_heap.c", "tests/menu_bsa_smoke.c") @()
Add-Test "menu_swf" $common @("source/bsa_reader.c", "source/menu_swf.c", "tests/menu_swf_smoke.c") @()
Add-Test "menu_swf_display" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_display.c", "tests/menu_swf_display_smoke.c") @()
Add-Test "menu_swf_display_list" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_display.c", "source/menu_swf_display_list.c", "tests/menu_swf_display_list_smoke.c") @()
Add-Test "menu_swf_clip" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_display.c", "source/menu_swf_display_list.c", "source/menu_swf_clip.c", "tests/menu_swf_clip_smoke.c") @()
Add-Test "menu_swf_shape" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_shape.c", "tests/menu_swf_shape_smoke.c") @()
Add-Test "menu_swf_tess" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_shape.c", "source/menu_swf_tess.c", "tests/menu_swf_tess_smoke.c") @()
Add-Test "menu_swf_text" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_display.c", "source/menu_swf_shape.c", "source/menu_swf_tess.c", "source/menu_swf_text.c", "tests/menu_swf_text_smoke.c") @()
Add-Test "menu_swf_raster" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_display.c", "source/menu_swf_shape.c", "source/menu_swf_tess.c", "source/menu_swf_raster.c", "tests/menu_swf_raster_smoke.c") @()
Add-Test "menu_swf_atlas" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_display.c", "source/menu_swf_shape.c", "source/menu_swf_tess.c", "source/menu_swf_raster.c", "source/menu_swf_atlas.c", "tests/menu_swf_atlas_smoke.c") @()
Add-Test "menu_swf_batch" $common @("source/bsa_reader.c", "source/menu_swf.c", "source/menu_swf_display.c", "source/menu_swf_shape.c", "source/menu_swf_tess.c", "source/menu_swf_text.c", "source/menu_swf_raster.c", "source/menu_swf_atlas.c", "source/menu_swf_batch.c", "tests/menu_swf_batch_smoke.c") @()
Add-Test "d3d9_capture" $common @("source/d3d9_capture.c", "source/guest.c", "tests/d3d9_capture_smoke.c") @()
$irPilotGen = Join-Path $root "pilot_out_mult\D3DXMatrixMultiply\gen"
if ((Test-Path (Join-Path $irPilotGen "pilot_00.c")) -and (Test-Path (Join-Path $irPilotGen "pilot_table.c"))) {
    $irPilotSrc = Get-ChildItem -Path $irPilotGen -Filter "pilot_*.c" | ForEach-Object { $_.FullName }
    Add-Test "ir_equiv" ($liftCommon + @("-I`"$irPilotGen`"")) ($legacySources + @(
        "tests/ir_equiv.c", "tests/ir_aot_fixture.c") + $irPilotSrc) @()
} else {
    Write-Log "IR equivalence test SKIPPED (pilot fixture missing)"
}

if (-not $PilotGen) {
    $preferred = Join-Path $root "pilot_out_mult\D3DXMatrixMultiply\gen"
    if (Test-Path (Join-Path $preferred "pilot_config.h")) {
        $PilotGen = $preferred
    } else {
        $candidate = Get-ChildItem -Path $root -Filter "pilot_manifest.json" -Recurse -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($candidate) { $PilotGen = $candidate.DirectoryName }
    }
}
if ($PilotGen -and (Test-Path (Join-Path $PilotGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $pilotSrc = Get-ChildItem -Path $PilotGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_real" ($liftCommon + @("-I`"$PilotGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $pilotSrc) @($PilotDll)
    Write-Log "Pilot test uses $PilotGen"
} elseif ($PilotDll -and (Test-Path $PilotDll)) {
    Write-Log "PILOT TEST SKIPPED (no generated pilot_config.h found)"
}
$transposeGen = Join-Path $root "pilot_out_mult\D3DXMatrixTranspose\gen"
if ((Test-Path (Join-Path $transposeGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $transposeSrc = Get-ChildItem -Path $transposeGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_transpose" ($liftCommon + @("-I`"$transposeGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $transposeSrc) @($PilotDll)
    Write-Log "Transpose pilot test uses $transposeGen"
}
$multransposeGen = Join-Path $root "pilot_out_mult\D3DXMatrixMultiplyTranspose\gen"
if ((Test-Path (Join-Path $multransposeGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $multransposeSrc = Get-ChildItem -Path $multransposeGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_multranspose" ($liftCommon + @("-I`"$multransposeGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $multransposeSrc) @($PilotDll)
    Write-Log "MultiplyTranspose pilot test uses $multransposeGen"
}
$planetransformGen = Join-Path $root "pilot_out_mult\D3DXPlaneTransform\gen"
if ((Test-Path (Join-Path $planetransformGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $planetransformSrc = Get-ChildItem -Path $planetransformGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_planetransform" ($liftCommon + @("-I`"$planetransformGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $planetransformSrc) @($PilotDll)
    Write-Log "PlaneTransform pilot test uses $planetransformGen"
}
$vec3coordGen = Join-Path $root "pilot_out_mult\D3DXVec3TransformCoord\gen"
if ((Test-Path (Join-Path $vec3coordGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $vec3coordSrc = Get-ChildItem -Path $vec3coordGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_vec3coord" ($liftCommon + @("-I`"$vec3coordGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $vec3coordSrc) @($PilotDll)
    Write-Log "Vec3TransformCoord pilot test uses $vec3coordGen"
}
$vec3normalGen = Join-Path $root "pilot_out_mult\D3DXVec3TransformNormal\gen"
if ((Test-Path (Join-Path $vec3normalGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $vec3normalSrc = Get-ChildItem -Path $vec3normalGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_vec3normal" ($liftCommon + @("-I`"$vec3normalGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $vec3normalSrc) @($PilotDll)
    Write-Log "Vec3TransformNormal pilot test uses $vec3normalGen"
}
$vec3normalizeGen = Join-Path $root "pilot_out_mult\D3DXVec3Normalize\gen"
if ((Test-Path (Join-Path $vec3normalizeGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $vec3normalizeSrc = Get-ChildItem -Path $vec3normalizeGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_vec3normalize" ($liftCommon + @("-I`"$vec3normalizeGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $vec3normalizeSrc) @($PilotDll)
    Write-Log "Vec3Normalize pilot test uses $vec3normalizeGen"
}
$planenormalizeGen = Join-Path $root "pilot_out_mult\D3DXPlaneNormalize\gen"
if ((Test-Path (Join-Path $planenormalizeGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $planenormalizeSrc = Get-ChildItem -Path $planenormalizeGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_planenormalize" ($liftCommon + @("-I`"$planenormalizeGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $planenormalizeSrc) @($PilotDll)
    Write-Log "PlaneNormalize pilot test uses $planenormalizeGen"
}
$matrixinverseGen = Join-Path $root "pilot_out_mult\D3DXMatrixInverse\gen"
if ((Test-Path (Join-Path $matrixinverseGen "pilot_config.h")) -and (Test-Path $PilotDll)) {
    $matrixinverseSrc = Get-ChildItem -Path $matrixinverseGen -Filter "*.c" | ForEach-Object { $_.FullName }
    Add-Test "pilot_matrixinverse" ($liftCommon + @("-I`"$matrixinverseGen`"")) ($legacySources + @("source/guest_process.c", "source/n3ds_pilot.c", "tests/pilot_host.c") + $matrixinverseSrc) @($PilotDll)
    Write-Log "MatrixInverse pilot test uses $matrixinverseGen"
}

if (Test-Path $TesvExe) {
    Add-Test "tesv_probe" $liftCommon ($legacySources + @("tests/tesv_probe.c")) @($TesvExe)
    if ($TesvSliceGen -and (Test-Path (Join-Path $TesvSliceGen "tesv_table.c"))) {
        $sliceSrc = Get-ChildItem -Path $TesvSliceGen -Filter "tesv_*.c" | ForEach-Object { $_.FullName }
        $sliceFlags = $liftCommon + @("-O0", "-I`"$TesvSliceGen`"")
        $sliceD3dx = @()
        if ($PilotGen -and (Test-Path (Join-Path $PilotGen "pilot_00.c")) -and (Test-Path (Join-Path $PilotGen "pilot_table.c"))) {
            $sliceFlags += @("-I`"$PilotGen`"")
            $sliceD3dx = Get-ChildItem -Path $PilotGen -Filter "pilot_*.c" | ForEach-Object { $_.FullName }
        } else {
            Write-Log "tesv_slice: no generated D3DX table; hotpatch bodies will fail explicitly"
        }
        $sliceCubeProbe = @()
        if (Select-String -Path (Join-Path $TesvSliceGen "tesv_*.c") -SimpleMatch -Pattern "tesv_cube_probe_fb5c20" -List -ErrorAction SilentlyContinue) {
            $sliceCubeProbe = @("source/tesv_cube_probe_slice.c")
        }
        Add-Test "tesv_slice" $sliceFlags ($legacySources + @("source/guest_process.c", "source/n3ds_boot.c", "source/n3ds_pilot.c", "source/tesv_slice_main.c", "source/tesv_sm3_probe.c", "source/tesv_cube_probe.c", "source/tesv_cecdc0_probe.c") + $sliceCubeProbe + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_null.c", "source/d3dx_texture_shims.c", "source/d3dx_surface_shims.c") + $picaSources + $sliceSrc + $sliceD3dx) @($TesvExe, ".", "200000", "167772160")
        # Measurement-only variant: the generated cube call sites are renamed to
        # tesv_cube_probe_fb5c20() for the census build, so this spec links the
        # AOT/IR shadow that the standard slice intentionally keeps out.
        if ($sliceCubeProbe.Count -gt 0 -or $sliceCecdc0Probe.Count -gt 0) { Add-Test "tesv_slice_cube_probe" $sliceFlags ($legacySources + @("source/guest_process.c", "source/n3ds_boot.c", "source/n3ds_pilot.c", "source/tesv_slice_main.c", "source/tesv_sm3_probe.c", "source/tesv_cube_probe.c") + $sliceCubeProbe + $sliceCecdc0Probe + @("source/d3d9_core.c", "source/d3d9_texcache.c", "source/d3d9_texture_data.c", "source/d3d9_bc.c", "source/d3d9_vertex.c", "source/d3d9_adapter.c", "source/d3d9_shader.c", "source/d3d9_tev.c", "source/d3d9_com.c", "source/d3d9_menu_closure.c", "source/d3d9_menu_shader_runtime.c", "source/d3d9_menu_light.c", "source/d3d9_capture.c", "source/d3d9_null.c", "source/d3dx_texture_shims.c", "source/d3dx_surface_shims.c") + $picaSources + $sliceSrc + $sliceD3dx) @($TesvExe, ".", "200000", "167772160") }
    }
}

if ($Only.Count -gt 0) {
    $Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    $filtered = [ordered]@{}
    foreach ($key in $tests.Keys) {
        if ($Only -contains $key) { $filtered[$key] = $tests[$key] }
    }
    $missing = $Only | Where-Object { -not $tests.Contains($_) }
    if ($missing) { throw "Unknown test(s): $($missing -join ', ')" }
    $tests = $filtered
}

$failed = @()
$passed = @()
$diagnosed = @()
foreach ($name in $tests.Keys) {
    $spec = $tests[$name]
    $runDir = Join-Path $outDir "run_${name}_$PID"
    $exe = Join-Path $runDir "$name.exe"
    Remove-Item -Recurse -Force $runDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
    if ($name -eq "tesv_slice" -or $name -eq "tesv_slice_cube_probe") {
        # Give the guest the same sandbox the 3DS title sees: the game's own
        # default INI and the two profile INIs (port display 400x240). Without
        # them the engine falls back to PC defaults and the renderer gate fails
        # on a resolution the port never intended to provide.
        $seed = Join-Path $env:APPDATA "Azahar\sdmc\Skyrim"
        foreach ($rel in @("Skyrim_default.ini", "Documents\My Games\Skyrim\SkyrimPrefs.ini", "LocalAppData\Skyrim\Skyrim.ini")) {
            $src = Join-Path $seed $rel
            if (Test-Path $src) {
                $dst = Join-Path $runDir $rel
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
                Copy-Item $src $dst -Force
            }
        }
    }
    $compileLog = Join-Path $logDir "${runId}_$name.build.log"
    $srcQuoted = $spec.Sources | ForEach-Object { "`"$_`"" }
    $clangArgs = @($spec.Flags) + $srcQuoted + @("-o", "`"$exe`"")
    $cmd = "call `"$vsVars`" >nul && `"$clang`" " + ($clangArgs -join " ")
    $jobs = if ($env:HOST_CLANG_JOBS) { [int]$env:HOST_CLANG_JOBS } else { 6 }
    $useParallel = (($name -eq "tesv_slice") -or ($name -eq "tesv_slice_cube_probe")) -and (-not $Sanitize) -and ($jobs -gt 1) -and ($spec.Sources.Count -gt 2)
    if ($useParallel) {
        Write-Log "BUILD $name (parallel clang jobs=$jobs)"
        $pb = Invoke-ClangParallel $clang $spec.Flags $spec.Sources $exe $runDir $jobs
        $buildOut = @($pb.Output)
        $buildCode = [int]$pb.Code
    } else {
        Write-Log "BUILD $name"
        $buildOut = & C:\Windows\System32\cmd.exe /c $cmd 2>&1
        $buildCode = [int]$LASTEXITCODE
    }
    $buildOut | Out-File -FilePath $compileLog -Encoding utf8
    if ($buildCode -ne 0 -or -not (Test-Path $exe)) {
        Write-Log "COMPILE FAIL: $name (see $compileLog)"
        $failed += $name
        continue
    }
    $savedIrc = $env:N3DS_IRC_PATH
    if (($name -eq "tesv_slice" -or $name -eq "tesv_slice_cube_probe") -and $TesvIrc) { $env:N3DS_IRC_PATH = $TesvIrc }
    else { Remove-Item Env:N3DS_IRC_PATH -ErrorAction SilentlyContinue }
    try {
        $run = Invoke-TestProcess $exe @($spec.Args) $runDir $TestTimeoutSec
    } finally {
        if ($savedIrc) { $env:N3DS_IRC_PATH = $savedIrc }
        else { Remove-Item Env:N3DS_IRC_PATH -ErrorAction SilentlyContinue }
    }
    $rc = $run.Code
    $runOut = $run.Output
    $runOut | ForEach-Object { Write-Log "  $($_)" }
    if ($run.TimedOut) {
        Write-Log "HOST FAIL: $name timed out after ${TestTimeoutSec}s"
        $failed += $name
        continue
    }
    if ($name -eq "tesv_slice" -or $name -eq "tesv_slice_cube_probe") {
        $diag = ($runOut -join "`n") -match 'first fault: Guest fault at'
        $exit = ($runOut -join "`n") -match '\[guest\] ExitProcess\((\d+)\) from ([0-9a-fA-F]+)'
        if ($diag) {
            Write-Log "HOST DIAG: tesv_slice reached a documented guest fault (not a PASS)"
            $diagnosed += $name
        } elseif ($exit) {
            $code = $Matches[1]; $from = $Matches[2]
            $mb = ($runOut -join "`n") -match '\[messagebox\] (.+)'
            $msg = if ($mb) { $Matches[1].Trim() } else { "" }
            Write-Log "HOST DIAG: guest called ExitProcess($code) from 0x$from$(if($msg){" after messagebox '$msg'"})"
            $diagnosed += $name
        } else {
            Write-Log "HOST FAIL: tesv_slice ended without a diagnosable guest fault (exit $rc)"
            $failed += $name
        }
        continue
    }
    if ($rc -eq 0) {
        Write-Log "HOST PASS: $name"
        $passed += $name
    } else {
        Write-Log "HOST FAIL: $name (exit $rc)"
        $failed += $name
    }
}

Write-Log "PYTHON tests"
$pyTests = @("tools/check_shim_bases.py", "tests/hotpatch_resolver_test.py", "tests/import_bindings_test.py", "tests/slice_pe_test.py", "tests/skyrim_assets_test.py", "tests/texture_pipeline_test.py", "tests/ir_partition_test.py", "tests/ir_select_rank_test.py", "tests/ir_container_test.py")
$realContainer = "E:\Skyrim_N3DS_build\ir_container\tesv.irc"
if (Test-Path $realContainer) { $pyTests += "tests/ir_container_growth_test.py" }
foreach ($script in $pyTests) {
    if (-not (Test-Path $script)) { continue }
    $out = & $Python $script 2>&1
    $rc = $LASTEXITCODE
    $out | ForEach-Object { Write-Log "  $($_)" }
    if ($rc -eq 0) { $passed += "py:$script" } else { $failed += "py:$script" }
}

Write-Log ("Summary: {0} passed, {1} diagnostic, {2} failed" -f $passed.Count, $diagnosed.Count, $failed.Count)
if ($diagnosed.Count -gt 0) { Write-Log ("Diagnostic (not PASS): " + ($diagnosed -join ", ")) }
if ($failed.Count -gt 0) {
    Write-Log ("Failed: " + ($failed -join ", "))
    exit 1
}
if ($diagnosed.Count -gt 0 -and -not $AllowDiagnosticFault) {
    Write-Log "Failing because a diagnostic fault was produced without -AllowDiagnosticFault"
    exit 2
}
exit 0
