<#
.SYNOPSIS  Turn a user-owned disc image (.iso / .xiso.iso, optionally inside a .7z) into a plain game directory.
.EXAMPLE   .\scripts\extract-disc.ps1 -Image "D:\dumps\Def Jam - Fight for NY (USA).xiso.iso" -DataDir D:\Games\DefJam\Data
.NOTES     extract-xiso auto-detects Redump (full disc) and XISO (game partition) layouts. Output is never put in the repo.
#>
param(
    [Parameter(Mandatory)] [string]$Image,
    [string]$DataDir = $(if ($env:DEFJAM_DATA) { $env:DEFJAM_DATA } else { $(Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "defjam") }),
    [string]$SevenZip = "C:\Program Files\7-Zip\7z.exe"
)
$ErrorActionPreference = "Stop"
$xiso = Join-Path $DataDir "tools\extract-xiso\artifacts\extract-xiso.exe"
if (-not (Test-Path $xiso)) { throw "extract-xiso.exe not found at $xiso (download from https://github.com/XboxDev/extract-xiso/releases)" }
New-Item -ItemType Directory -Force (Join-Path $DataDir "xiso") | Out-Null
if ($Image -like "*.7z" -or $Image -like "*.zip") {
    & $SevenZip x -y -o"$(Join-Path $DataDir 'xiso')" $Image | Select-Object -Last 3
    $Image = Get-ChildItem (Join-Path $DataDir "xiso") -Filter *.iso | Select-Object -First 1 -ExpandProperty FullName
}
$out = Join-Path $DataDir "extracted"
New-Item -ItemType Directory -Force $out | Out-Null
& $xiso -x -d $out $Image | Select-Object -Last 3
python (Join-Path $PSScriptRoot "verify-dump.py") $out
