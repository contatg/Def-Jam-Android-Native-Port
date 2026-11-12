# Def Jam Android 1.4 source

This is the source snapshot for the Android 1.4 port, including the touch Special correction and explicit FFNY Online attribution in the app's About screen. The tree combines the project's historical runtime and tools with the 1.4 Android source files.

- [Android app, launcher and build](android/)
- [Native port and online client](src/)
- [xboxrecomp runtime and recompiler](tools/xboxrecomp/)
- [Room server source and build](server/)
- [hakuX integration and pinned upstream source](third_party/hakux/)
- [Credits and applicable licenses](CREDITS.md)

## Build inputs

Use the [Android build guide](android/README.md) with a verified original Xbox USA dump, JDK 17, Gradle 8.7, Android SDK 34 and the NDK/CMake versions in [build.gradle](android/app/build.gradle). The [dependency preparation script](android/Preparar-Dependencias-Android.ps1) and [recompiler scripts](scripts/) set up the local inputs. The first Gradle setup may need network access to populate its dependency cache before the offline build script runs.

Generate the lifted C/H files locally from your disc. Game data, generated game code, signing keys, build outputs and the Lossless Scaling DLL or its extracted shaders are excluded. This tree uses a generic launcher icon. The emulator libraries require the separate hakuX build above, and LSFG requires the user's own compatible Lossless Scaling DLL.

The source includes changes made after the release APK was built. A clean Android build and device test from this publication are still pending.
