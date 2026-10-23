# Android ARM64 build

## Requirements

- Windows PowerShell, Python, Git, JDK 17, Gradle 8.7, Android SDK and a compatible Android NDK.
- The project's existing Android dependencies, or internet access to prepare them.
- Your own supported original Xbox game dump. No game disc, ISO, XBE or extracted game content is included.

Keep your extracted game at `C:\Games\DefJamFFNY-data\extracted`, including your own
`default.xbe`. `scripts/verify-dump.py` checks that it matches the supported original USA version.
The local compiler can generate the private game C/H files from your own dump when they are missing.
Do not upload your disc, game data, saves or generated game C/H files to the source repository.

## Compile

Open PowerShell in the project directory:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
$env:Path = "C:\dev\gradle-8.7\bin;$env:Path"
.\android\Compilar-DefJam-Android.ps1 -DataDir "C:\Games\DefJamFFNY-data" -Variant Release
```

The build script checks the original dump, reuses or regenerates your local game sources,
prepares missing Android dependencies and invokes Gradle. Its default Gradle invocation uses
one worker and a bounded Java heap. It keeps your existing settings and saves.

Output: `android/app/build/outputs/apk/release/app-release.apk`.

## Install

Connect your Android device with USB debugging authorized, then run:

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r "android\app\build\outputs\apk\release\app-release.apk"
```

The package ID remains `org.defjamffny.android`. The app is named **Def Jam Android Native Port**.
The supplied launcher image is referenced by both legacy and adaptive Android icons.

## Game data and controls

The device data root stays `/storage/emulated/0/DefJamFFNY`. Place the extracted game in its `game`
subdirectory using your existing installation procedure. Allow storage access in the launcher.
Use the wrench icon to edit controls in menus or during a fight. Pause with START before editing.
Control layouts, button mappings and existing saves remain compatible with previous builds.

## Driver and diagnostics

Asynchronous rendering is always enabled. Parallel vertex preparation is disabled; both settings are
fixed and have no editable launcher or in-game option. Old saved choices are overridden at launch.
System Vulkan is the default. Optional Turnip packages are for compatible Snapdragon/Adreno devices;
a driver import failure falls back to the system driver. Graphics options persist in `settings.ini`.
The FPS counter displays measured application frames and is not a promise of unlocked gameplay FPS.

The game runs in `org.defjamffny.android:game`, separately from the launcher. Pull the real phone log:

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" pull /storage/emulated/0/DefJamFFNY/defjam-native.log "$env:USERPROFILE\Downloads\defjam-native.log"
```

A PC-side log may be stale and is not a substitute for the current phone log.
A successful build is not proof that opening, loading, gameplay and background/resume all work.
Test those stages on the intended device and report the actual log and version.
