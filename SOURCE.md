# Def Jam Android 1.4 source

This is the source snapshot for the Android 1.4 port, including the touch Special correction and explicit FFNY Online attribution in the app's About screen. The tree combines the project's historical runtime and tools with the 1.4 Android source files.

- [Android app, launcher and build](android/)
- [Native port and online client](src/)
- [xboxrecomp runtime and recompiler](tools/xboxrecomp/)
- [Room server source and build](server/)
- [Credits and applicable licenses](CREDITS.md)

## Build inputs

Use the [Android build guide](android/README.md) with a verified original Xbox USA dump, JDK 17, Gradle 8.7, Android SDK 34 and the NDK/CMake versions in [build.gradle](android/app/build.gradle). The [dependency preparation script](android/Preparar-Dependencias-Android.ps1) and [recompiler scripts](scripts/) set up the local inputs. The first Gradle setup may need network access to populate its dependency cache before the offline build script runs.

Generate the lifted C/H files locally from your disc. Game data, generated game code, signing keys, build outputs and the Lossless Scaling DLL or its extracted shaders are excluded. This tree uses a generic launcher icon. LSFG requires the user's own compatible Lossless Scaling DLL.

The source includes changes made after the release APK was built. A clean Android build and device test from this publication are still pending.


## FFNY Online Android development

The wire helpers target [build 81](https://github.com/sweeax-exe/ffny-online/releases/tag/v0.13.0-remaster.81), game commit `928b57e4de9964edf476387c048f014d17676f54`, toolkit pin `3dbfb058f19e87db912cde8794824db54e11a9f4`. This branch still uses the Android 1.4 game hooks. The public relay remains disabled while the build 81 match protocol and game hooks are being ported. The account screen uses the Discord browser sign-in on the partner account service.

Android now compiles the deterministic math sources from [build 80](https://github.com/sweeax-exe/ffny-online/tree/v0.13.0-remaster.80/src/detmath) and checks the guest thread's floating-point mode at each simulation step. The upstream vector test produced 55 matching hashes on x86-64 and emulated ARM64 (1,000,000 inputs per set). This tests math functions only. Gameplay checksum parity is pending.

Before enabling the partner relay:

- Port the tagged game hooks, lockstep, rollback and toolkit changes to Android, then validate gameplay and capabilities against the partner's `docs/android-client.md`. Keep the Android queue separate and ranked disabled.
- Supply the compiled candidate, SHA-256 and actual CAPS to the partner for activation and a two-device test. Publish `latest-android.json` to the partner update endpoint with the Android maintainer's signature. The helper `scripts/sign-android-update.py` produces the manifest and detached signature from a release APK, its HTTPS download URL, and the maintainer's P-256 key. Confirm the Android manifest format and signature format with the partner before deploying. The manifest's build is a minimum-version gate for Android online sessions.
- The Android build now uses the same 192 MiB contiguous window and 128 MiB reported RAM as the PC build. The Android rollback code locates the title's live heap at audio initialization, including when it has moved beyond the old fixed range. Rollback remains experimental. Measure Android peak process memory during map load and rollback fights. The reported PC reference is about 580 MB of private memory in a rollback fight and up to 32 MB of map textures with temporary duplication during loading.
- Gate PC cross-play on the shared math implementation and at least 200 checksum-verified matches. Build 81 relays pair clients on the same build.
