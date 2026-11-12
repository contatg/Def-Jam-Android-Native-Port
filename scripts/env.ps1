# Dot-source this to get MSVC + CMake + Ninja on PATH in the current PowerShell:
#   . .\scripts\env.ps1
# Prefers VS 2022 Build Tools, falls back to VS 2019 Build Tools.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio Build Tools" }
$vs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -sort -latest -property installationPath
if (-not $vs) { throw "No Visual Studio with the C++ toolset found" }
$devcmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
Write-Host "Using $vs"
cmd /c "`"$devcmd`" -arch=amd64 -host_arch=amd64 -no_logo && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$cmakeBin = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaBin = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$env:PATH = "$cmakeBin;$ninjaBin;$env:PATH"
$env:DEFJAM_DATA = if ($env:DEFJAM_DATA) { $env:DEFJAM_DATA } else { $(Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "defjam") }
Write-Host ("cl: " + (Get-Command cl.exe).Source)
Write-Host ("cmake: " + (cmake --version | Select-Object -First 1))
Write-Host ("ninja: " + (ninja --version))
