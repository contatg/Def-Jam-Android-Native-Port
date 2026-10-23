param(
    [Parameter(Mandatory = $true)][string]$Apk,
    [string]$Adb = "adb"
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Apk)) { throw "APK nao encontrado: $Apk" }
& $Adb install -r $Apk
if ($LASTEXITCODE) { throw "Falha ao instalar o APK." }
& $Adb logcat -c
& $Adb shell am force-stop org.defjamffny.android
& $Adb shell monkey -p org.defjamffny.android 1 | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out = Join-Path ([Environment]::GetFolderPath("UserProfile")) "Downloads\defjam-android-$stamp-logcat.txt"
& $Adb logcat -v threadtime | Tee-Object -FilePath $out
