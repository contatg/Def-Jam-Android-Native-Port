<#
.SYNOPSIS  Feed runtime-discovered function addresses (unresolved ICALL targets, thread start routines) from a run log
           into config/seed_functions.json, then re-run analyze.ps1 + recomp.ps1 + build.ps1.
.EXAMPLE   .\scripts\seed-from-log.ps1                       # uses the newest logs\run-*.log.err
#>
param([string]$Log = "", [string]$DataDir = $(if ($env:DEFJAM_DATA) { $env:DEFJAM_DATA } else { $(Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "defjam") }))
$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $Log) { $Log = (Get-ChildItem (Join-Path $repo "logs") -Filter "run-*.log.err" | Sort-Object LastWriteTime | Select-Object -Last 1).FullName }
Write-Host "Seeding from $Log"
Push-Location (Join-Path $repo "tools\xboxrecomp")
try {
    python -m tools.seed_from_log $Log game_files/default.xbe --functions tools/disasm/output/functions.json --seeds (Join-Path $repo "config\seed_functions.json") --analysis-json game_files/default_analysis.json
} finally { Pop-Location }
