param(
    [string]$SDLZip,
    [string]$ImGuiZip,
    [string]$NdkVersion
)

$ErrorActionPreference = "Stop"
$androidDir = $PSScriptRoot
$vendor = Join-Path $androidDir "vendor"
$packages = Join-Path $vendor "packages"
New-Item -ItemType Directory -Force -Path $vendor, $packages | Out-Null

function Install-ZipSource {
    param(
        [string]$Name,
        [string]$ZipPath,
        [string]$Url,
        [string]$Marker
    )
    $target = Join-Path $vendor $Name
    if (Test-Path (Join-Path $target $Marker)) { return }
    if (-not $ZipPath) {
        $ZipPath = Join-Path $packages "$Name.zip"
        if (-not (Test-Path $ZipPath)) {
            Write-Host "Baixando $Name (ZIP, sem git)..."
            Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $ZipPath
        }
    }
    $ZipPath = (Resolve-Path $ZipPath).Path
    $temp = Join-Path $env:TEMP ("defjam-" + $Name + "-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        Expand-Archive -Path $ZipPath -DestinationPath $temp -Force
        $source = Get-ChildItem -Path $temp -Directory | Where-Object {
            Test-Path (Join-Path $_.FullName $Marker)
        } | Select-Object -First 1
        if (-not $source) { throw "$Name ZIP invalido: nao achei $Marker" }
        if (Test-Path $target) { Remove-Item -Recurse -Force $target }
        Move-Item -Path $source.FullName -Destination $target
    } finally {
        if (Test-Path $temp) { Remove-Item -Recurse -Force $temp }
    }
}

Install-ZipSource -Name "SDL" -ZipPath $SDLZip `
    -Url "https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.16.zip" `
    -Marker "CMakeLists.txt"
Install-ZipSource -Name "imgui" -ZipPath $ImGuiZip `
    -Url "https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip" `
    -Marker "imgui.cpp"

if ($env:ANDROID_SDK_ROOT) {
    $sdk = $env:ANDROID_SDK_ROOT
} elseif ($env:ANDROID_HOME) {
    $sdk = $env:ANDROID_HOME
} else {
    $sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk"
}
$ndkRoot = Join-Path $sdk "ndk"
if (-not (Test-Path $ndkRoot)) { throw "NDK nao encontrado em $ndkRoot" }
if ($NdkVersion) {
    $ndk = Join-Path $ndkRoot $NdkVersion
} else {
    $ndk = Get-ChildItem -Path $ndkRoot -Directory | Where-Object {
        Test-Path (Join-Path $_.FullName "sources\third_party\shaderc\Android.mk")
    } | Sort-Object {
        try { [version]$_.Name } catch { [version]"0.0" }
    } -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $ndk -or -not (Test-Path (Join-Path $ndk "ndk-build.cmd"))) {
    throw "NDK valido nao encontrado. Instale um NDK pelo Android Studio."
}
$NdkVersion = Split-Path $ndk -Leaf
$shaderSource = Join-Path $ndk "sources\third_party\shaderc"
$androidMk = Join-Path $shaderSource "Android.mk"
if (-not (Test-Path $androidMk)) {
    throw "Este NDK nao contem sources\third_party\shaderc. Instale NDK 27.2 ou 28.2."
}

$shaderTarget = Join-Path $vendor "shaderc"
$shaderLib = Join-Path $shaderTarget "lib\arm64-v8a\libshaderc.a"
if (-not (Test-Path $shaderLib)) {
    Write-Host "Compilando shaderc arm64 a partir do NDK $NdkVersion..."
    Push-Location $shaderSource
    try {
        $jobs = [Math]::Max(1, [Environment]::ProcessorCount)
        & (Join-Path $ndk "ndk-build.cmd") "NDK_PROJECT_PATH=." `
            "APP_BUILD_SCRIPT=Android.mk" "APP_STL:=c++_shared" `
            "APP_ABI=arm64-v8a" "APP_PLATFORM=android-29" `
            "libshaderc_combined" "-j$jobs"
        if ($LASTEXITCODE) { throw "ndk-build do shaderc falhou." }
    } finally {
        Pop-Location
    }
    $builtLib = Join-Path $shaderSource "libs\c++_shared\arm64-v8a\libshaderc.a"
    $builtInclude = Join-Path $shaderSource "include\shaderc"
    if (-not (Test-Path $builtLib)) { throw "shaderc compilou, mas nao achei $builtLib" }
    if (-not (Test-Path (Join-Path $builtInclude "shaderc.h"))) { throw "Headers do shaderc nao encontrados." }
    New-Item -ItemType Directory -Force -Path (Split-Path $shaderLib), (Join-Path $shaderTarget "include") | Out-Null
    Copy-Item $builtLib $shaderLib -Force
    Copy-Item $builtInclude (Join-Path $shaderTarget "include") -Recurse -Force
}

Set-Content -Path (Join-Path $vendor ".ndk-version") -Value $NdkVersion -NoNewline
Write-Host "OK: SDL, ImGui e shaderc locais prontos. NDK $NdkVersion"
