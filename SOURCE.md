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


## FFNY Online Android development

The transport work uses the PC reference [build 78](https://github.com/sweeax-exe/ffny-online/releases/tag/v0.13.0-remaster.78), game commit `6c0ca28bbf0ab7c8f5ee99f10e4f6b0460c18c98`, toolkit pin `864e4ff6eec58f7a18d708387aff3398cf262ef9`. This branch still builds the 1.4 game code. The build number in the packet helpers identifies the wire format under test. The public relay and its account HTTP requests remain disabled in this branch.

Before enabling the partner relay:

- Port the tagged game hooks, lockstep, rollback and toolkit changes to Android, then validate gameplay and capabilities against the partner's `docs/android-client.md`. Keep the Android queue separate and ranked disabled.
- Confirm relay and account endpoints, the fair-play report requirements and signed `latest-android.json` format with FFNY Online. Publish it using the maintainer's update-signing key and supply the compiled candidate and its actual CAPS to the partner for activation and a two-device test.
- For build 81, use the partner's exact game and toolkit commits. Build both sides with `XBOXRECOMP_CONTIG_MB=192` and `XBOXRECOMP_RAM_REPORT_MB=128`. Check the larger guest window across kernel, graphics, audio and rollback. Measure Android peak process memory during map load and rollback fights. The reported PC reference is about 580 MB of private memory in a rollback fight and up to 32 MB of map textures with temporary duplication during loading.
- Gate PC cross-play on the shared math implementation and at least 200 checksum-verified matches. Build 81 relays pair clients on the same build.
