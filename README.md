# Def Jam Android Native Port

An experimental native Android ARM64 port of **Def Jam: Fight for NY**, based on
[vyanhursky/DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp) and its xboxrecomp runtime.

## Bring your own original game

**The original Xbox game is not included.** This source repository and its
patches do not distribute ISO/XISO disc images, `default.xbe`, the extracted game data directory,
music archives, or save files. This project provides a way to run the game using the data from **your own
original Xbox disc or an original ISO you own**. You must supply and extract that data yourself.
An APK by itself does not provide the original game.

Launcher and application artwork is included for identification only. Def Jam, Xbox, the original
game, characters, logos and artwork belong to their respective owners. This is an unofficial
community project and is not affiliated with or endorsed by the original publishers or platform owners.
The source license does not grant rights to the original game's artwork or content.

## Features

- Native Android ARM64 launcher and Vulkan rendering, with optional Turnip support.
- Custom touch controls and an in-game wrench shortcut for the layout editor.
- Separate menu controls, grouped D-pad editing, and START/ENTER navigation.
- Always-enabled asynchronous rendering, a simple FPS display, and saved graphics settings.
- One guest CPU for compatibility; native workers may use all permitted CPUs.
- English launcher, editor, status messages and project documentation entry points.

The user-provided launcher image is packaged unchanged. Existing custom control artwork,
button mappings, save paths, package ID and settings remain compatible.

## Build on your PC

See [Android build instructions](android/README.md). Use your own supported original game dump.
The local `tools/xboxrecomp` runtime is included; no submodule initialization is required for this snapshot.
SDL, ImGui and shaderc are prepared by the Android dependency script. Open-source loader sources
and their notices are retained in `android/vendor`.

From the project directory in PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
$env:Path = "C:\dev\gradle-8.7\bin;$env:Path"
.\android\Compilar-DefJam-Android.ps1 -DataDir "C:\Games\DefJamFFNY-data" -Variant Release
```

After a successful build, install without clearing app data:

```powershell
& "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe" install -r "android\app\build\outputs\apk\release\app-release.apk"
```

## APK downloads and validation

APKs are compiled on the maintainer's PC and can be uploaded to Releases separately.
GitHub Actions checks production control and runtime fixtures; it does not build an APK or require
private generated game sources. Passing code checks or compiling an APK does not prove gameplay,
background/resume behavior, or a particular frame rate on a device.

The latest source is **0.5.7-v21-fixed-render**. Asynchronous rendering is always enabled and parallel
vertex preparation is disabled. Neither setting is exposed in the launcher or an in-game menu.
Other native workers retain their automatic CPU policy. No particular gameplay frame rate is guaranteed.

## Credits and licenses

Original recompilation project: [vyanhursky/DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp).
See [LICENSE](LICENSE) and the notices accompanying each third-party component. Original game content
must be obtained from your own original disc/ISO and is not supplied by this project.
