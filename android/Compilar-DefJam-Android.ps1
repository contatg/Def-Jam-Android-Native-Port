param(
    [Parameter(Mandatory = $true)][string]$DataDir,
    [ValidateSet("Debug", "Release")][string]$Variant = "Debug",
    [switch]$Regenerate
)

$ErrorActionPreference = "Stop"
$androidDir = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $androidDir "..")).Path
$extracted = Join-Path $DataDir "extracted"
$xbe = Join-Path $extracted "default.xbe"

if (-not (Test-Path $xbe)) {
    throw "Nao achei $xbe. DataDir deve conter extracted\default.xbe."
}
python (Join-Path $repo "scripts\verify-dump.py") $extracted
if ($LASTEXITCODE) { throw "O dump nao corresponde a versao USA suportada." }

$gameLink = Join-Path $repo "game"
if (-not (Test-Path $gameLink)) {
    New-Item -ItemType Junction -Path $gameLink -Target $extracted | Out-Null
}
$env:DEFJAM_DATA = (Resolve-Path $DataDir).Path

$dispatch = Join-Path $repo "src\recomp\gen\recomp_dispatch.c"
if ($Regenerate -or -not (Test-Path $dispatch)) {
    & (Join-Path $repo "scripts\analyze.ps1") -DataDir $DataDir
    if ($LASTEXITCODE) { throw "A analise falhou." }
    & (Join-Path $repo "scripts\recomp.ps1")
    if ($LASTEXITCODE) { throw "A recompilacao falhou." }
}

$requiredFiles = @(
    "vendor\SDL\CMakeLists.txt",
    "vendor\imgui\imgui.cpp",
    "vendor\shaderc\include\shaderc\shaderc.h",
    "vendor\shaderc\lib\arm64-v8a\libshaderc.a"
)
$missing = @($requiredFiles | Where-Object { -not (Test-Path (Join-Path $androidDir $_)) })
if ($missing.Count -gt 0) {
    Write-Host "Preparando dependencias locais (sem git)..."
    & (Join-Path $androidDir "Preparar-Dependencias-Android.ps1")
    if ($LASTEXITCODE) { throw "Falha ao preparar as dependencias Android." }
}
foreach ($required in $requiredFiles) {
    if (-not (Test-Path (Join-Path $androidDir $required))) {
        throw "Dependencia ausente depois da preparacao: android\$required"
    }
}

$ndkVersionFile = Join-Path $androidDir "vendor\.ndk-version"
if (-not (Test-Path $ndkVersionFile)) { throw "Nao achei $ndkVersionFile" }
$ndkVersion = (Get-Content $ndkVersionFile -Raw).Trim()

$task = if ($Variant -eq "Release") { "assembleRelease" } else { "assembleDebug" }
$wrapper = Join-Path $androidDir "gradlew.bat"
Push-Location $androidDir
try {
    if (Test-Path $wrapper) {
        & $wrapper --offline --no-daemon --max-workers=1 "-Dorg.gradle.jvmargs=-Xmx2048m -XX:MaxMetaspaceSize=512m -Dfile.encoding=UTF-8" "-PdefjamNdkVersion=$ndkVersion" $task
    } elseif (Get-Command gradle -ErrorAction SilentlyContinue) {
        gradle --offline --no-daemon --max-workers=1 "-Dorg.gradle.jvmargs=-Xmx2048m -XX:MaxMetaspaceSize=512m -Dfile.encoding=UTF-8" "-PdefjamNdkVersion=$ndkVersion" $task
    } else {
        throw "Gradle nao encontrado. Gere o wrapper uma vez com 'gradle wrapper' ou instale Gradle localmente."
    }
    if ($LASTEXITCODE) { throw "Build Android falhou." }
} finally {
    Pop-Location
}

$name = if ($Variant -eq "Release") { "app-release.apk" } else { "app-debug.apk" }
$apk = Join-Path $androidDir "app\build\outputs\apk\$($Variant.ToLower())\$name"
if (-not (Test-Path $apk)) { throw "Gradle terminou, mas o APK nao foi encontrado em $apk" }
$hash = (Get-FileHash $apk -Algorithm SHA256).Hash
Write-Host "APK: $apk"
Write-Host "SHA256: $hash"
