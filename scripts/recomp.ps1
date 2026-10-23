<#
.SYNOPSIS  Lift default.xbe to C into src/recomp/gen (requires scripts/analyze.ps1 to have run).
.PARAMETER Split  functions per generated file (default 1000 -> ~17 files for 16k functions)
#>
param([int]$Split = 1000, [string[]]$ExtraArgs = @(), [string]$ToolkitDir = "")
$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$tk = if ($ToolkitDir) { (Resolve-Path $ToolkitDir).Path } else { Join-Path $repo "tools\xboxrecomp" }
$gen = Join-Path $repo "src\recomp\gen"
if (-not (Test-Path (Join-Path $tk "tools\abi_analysis\output\abi_functions.json"))) { throw "Run scripts/analyze.ps1 first" }
# recomp_manual.c is the single source of truth for what this project
# implements by hand: a plain definition replaces the generated body, and a
# definition plus an "extern void sub_X_gen(void);" retains the generated body
# under that name. Direct calls use sub_X_gen and bypass the wrapper; use an
# entry hook to intercept every caller (see AGENTS.md).
python (Join-Path $PSScriptRoot "pipeline-state.py") lift --toolkit $tk --split $Split -- @ExtraArgs
if ($LASTEXITCODE) { throw "recompilation failed; previous generated sources preserved" }
Write-Host ("Generated: " + (Get-ChildItem $gen -Filter *.c).Count + " C files, " + [math]::Round((Get-ChildItem $gen | Measure-Object Length -Sum).Sum/1MB) + " MB")
