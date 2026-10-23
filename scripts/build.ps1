<#
.SYNOPSIS  Configure + build with a CMake preset.  .\scripts\build.ps1 [-Preset win-x64-debug] [-Clean]
#>
param([string]$Preset = "win-x64-debug", [switch]$Clean, [string]$ToolkitDir = "")
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "env.ps1")
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $repo
try {
    $tk = if ($ToolkitDir) { (Resolve-Path $ToolkitDir).Path } else { Join-Path $repo "tools\xboxrecomp" }
    $runtimeOnly = $Preset -eq "ci-runtime-only"
    if (-not $runtimeOnly) {
        $targetExe = [IO.Path]::GetFullPath((Join-Path $repo "build\$Preset\defjam_recomp.exe"))
        Get-Process defjam_recomp -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $targetExe } | Stop-Process -Force
        python (Join-Path $PSScriptRoot "pipeline-state.py") verify-lift --toolkit $tk
        if ($LASTEXITCODE) { throw "generated code freshness check failed" }
    }
    if (-not $runtimeOnly) {
        python (Join-Path $PSScriptRoot "pipeline-state.py") begin-build --toolkit $tk --preset $Preset
        if ($LASTEXITCODE) { throw "build input snapshot failed" }
    }
    if ($Clean) {
        $buildRoot = [IO.Path]::GetFullPath((Join-Path $repo "build")) + [IO.Path]::DirectorySeparatorChar
        $cleanTarget = [IO.Path]::GetFullPath((Join-Path $repo "build\$Preset"))
        if (-not $cleanTarget.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) { throw "clean target is outside build directory" }
        Remove-Item -LiteralPath $cleanTarget -Recurse -Force -ErrorAction SilentlyContinue
    }
    $buildGame = if ($runtimeOnly) { "OFF" } else { "ON" }
    cmake --preset $Preset "-DXBOXRECOMP_DIR=$tk" "-DDEFJAM_BUILD_GAME=$buildGame"; if ($LASTEXITCODE) { throw "configure failed" }
    $targetArgs = if ($runtimeOnly) { @() } else { @("--target", "defjam_recomp") }
    cmake --build --preset $Preset @targetArgs 2>&1 | Tee-Object -FilePath (Join-Path $repo "build\last-build.log")
    if ($LASTEXITCODE) { throw "build failed (see build\last-build.log)" }
    if (-not $runtimeOnly) {
        python (Join-Path $PSScriptRoot "pipeline-state.py") record-build --toolkit $tk --preset $Preset
        if ($LASTEXITCODE) { throw "build state validation failed" }
    }
} finally { Pop-Location }
