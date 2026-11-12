<#
.SYNOPSIS  Run the recompiled game from the repo root (so ./game resolves) and capture the boot log.
.PARAMETER WatchdogSecs  set RECOMP_WATCHDOG_SECS to get a stack dump when the guest hangs
#>
param([string]$Preset = "win-x64-debug", [int]$WatchdogSecs = 0, [int]$TimeoutSecs = 0)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$exe = Join-Path $repo "build\$Preset\defjam_recomp.exe"
if (-not (Test-Path $exe)) { throw "not built: $exe" }
python (Join-Path $PSScriptRoot "pipeline-state.py") verify-build --preset $Preset
if ($LASTEXITCODE) { throw "build freshness check failed" }
if (-not (Test-Path (Join-Path $repo "game\default.xbe"))) { throw "./game/default.xbe missing - create a junction to your extracted dump" }
New-Item -ItemType Directory -Force (Join-Path $repo "logs") | Out-Null
$log = Join-Path $repo ("logs\run-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
if ($WatchdogSecs -gt 0) { $env:RECOMP_WATCHDOG_SECS = "$WatchdogSecs" }
Push-Location $repo
$runtimeDefaults = @("RECOMP_VBLANK", "RECOMP_PB_EXEC", "RECOMP_PB_D3D11", "RECOMP_USB")
$addedDefaults = @()
try {
    # Match the harness's normal hardware path in a fresh PowerShell session.
    # Keep explicit diagnostic overrides and restore the caller's environment.
    foreach ($name in $runtimeDefaults) {
        if ($null -eq [Environment]::GetEnvironmentVariable($name, "Process")) {
            [Environment]::SetEnvironmentVariable($name, "1", "Process")
            $addedDefaults += $name
        }
    }
    $p = Start-Process -FilePath $exe -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
    if ($TimeoutSecs -gt 0) { if (-not $p.WaitForExit($TimeoutSecs * 1000)) { $p.Kill(); Write-Host "killed after $TimeoutSecs s" } }
    else { $p.WaitForExit() }
    Write-Host "exit code $($p.ExitCode); log: $log"
} finally {
    foreach ($name in $addedDefaults) {
        [Environment]::SetEnvironmentVariable($name, $null, "Process")
    }
    Pop-Location
}
