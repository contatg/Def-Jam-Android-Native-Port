<#
.SYNOPSIS  Run the xboxrecomp analysis pipeline on default.xbe (Python only, no compiler needed).
.NOTES     Steps: xbe_parser -> disasm -> func_id -> abi_analysis [-> ghidra_naming if -Ghidra]. Outputs copied to $DataDir\analysis.
#>
param(
    [string]$DataDir = $(if ($env:DEFJAM_DATA) { $env:DEFJAM_DATA } else { $(Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "defjam") }),
    [switch]$Ghidra,
    [string]$ToolkitDir = ""
)
$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$tk = if ($ToolkitDir) { (Resolve-Path $ToolkitDir).Path } else { Join-Path $repo "tools\xboxrecomp" }
$xbe = Join-Path $DataDir "extracted\default.xbe"
python (Join-Path $PSScriptRoot "verify-dump.py") (Join-Path $DataDir "extracted"); if ($LASTEXITCODE) { throw "dump check failed" }
New-Item -ItemType Directory -Force (Join-Path $tk "game_files") | Out-Null
Copy-Item $xbe (Join-Path $tk "game_files\default.xbe") -Force
python (Join-Path $PSScriptRoot "pipeline-state.py") begin-analysis --toolkit $tk
if ($LASTEXITCODE) { throw "analysis input snapshot failed" }
Push-Location $tk
try {
    python -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json
    if ($LASTEXITCODE) { throw "XBE parser failed" }
    $seedArgs = @()
    if (Test-Path (Join-Path $repo "config\seed_functions.json")) { $seedArgs = @("--seed-functions", (Join-Path $repo "config\seed_functions.json")) }
    python -m tools.disasm     game_files/default.xbe --force --extra-sections D3D,D3DX,DSOUND,XPP,XGRPH,DOLBY @seedArgs
    if ($LASTEXITCODE) { throw "disassembly failed" }
    python -m tools.func_id    game_files/default.xbe -v
    if ($LASTEXITCODE) { throw "function identification failed" }
    python -m tools.abi_analysis game_files/default.xbe -v
    if ($LASTEXITCODE) { throw "ABI analysis failed" }
    if ($Ghidra) {
        $env:XBE = "game_files/default.xbe"
        $env:GHIDRA_INSTALL_DIR = (Get-ChildItem (Join-Path $DataDir "tools\ghidra") -Directory | Select-Object -First 1).FullName
        Write-Host "Ghidra naming: see tools/ghidra_naming/README (run_ghidra.sh is bash; use Git Bash or WSL)"
    }
} finally { Pop-Location }
python (Join-Path $PSScriptRoot "pipeline-state.py") record-analysis --toolkit $tk
if ($LASTEXITCODE) { throw "analysis state validation failed" }
$an = Join-Path $DataDir "analysis"
New-Item -ItemType Directory -Force $an | Out-Null
foreach ($d in "tools\disasm\output","tools\func_id\output","tools\abi_analysis\output","tools\recomp\output") {
    # -replace takes a regular expression, so a literal backslash is '\\'. The
    # single one here threw "The regular expression pattern \ is not valid" and
    # took the whole script's exit code with it, long after the analysis itself
    # had finished and written its output.
    $src = Join-Path $tk $d; if (Test-Path $src) { Copy-Item $src (Join-Path $an ($d -replace '\\','_')) -Recurse -Force }
}
Copy-Item (Join-Path $tk "game_files\default_analysis.json") $an -Force
Write-Host "Analysis outputs archived to $an"
