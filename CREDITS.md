# Credits and third-party notices

**Android port maintainer: contatg.**

Project: https://github.com/contatg/Def-Jam-Android-Native-Port

## Online multiplayer

**sweeax-exe and the FFNY Online contributors created FFNY Online, the upstream online multiplayer implementation.** This Android port adapts their networking code and language-pack tooling. contatg maintains the Android integration and the [room server adaptation](server/). [Original FFNY Online project](https://github.com/sweeax-exe/ffny-online) · [MIT license](licenses/FFNY-Online-MIT.txt)

The Android transport preparation in `src/hooks/ffny_android_protocol.h` is derived from [FFNY Online build 78](https://github.com/sweeax-exe/ffny-online/releases/tag/v0.13.0-remaster.78) and carries the upstream GPL-3.0-or-later notice. The existing private relay is based on the earlier MIT implementation. The build-78 transport is linked into the native game client, whose combined distribution is under GPL-3.0-or-later.

The Android simulation uses the deterministic math sources from [FFNY Online build 80](https://github.com/sweeax-exe/ffny-online/tree/v0.13.0-remaster.80/src/detmath). The copied sources preserve their MIT and fdlibm notices.

## Recompilation and runtime

Original static recompilation project: Vlad Yanhursky and contributors,
https://github.com/vyanhursky/DefJamFFNY-recomp.

Runtime/toolkit: xboxrecomp and its contributors. Parts of the runtime derive
from xemu and retain their own licenses. See the original notices below.

The original game and its content remain the property of their respective rights
holders. This project's Android-port credit does not claim authorship of the
original Xbox game, upstream recompilation work or third-party components.

## Preserved license texts

- [FFNY Online GPL-3.0 license text](licenses/FFNY-Online-GPL-3.0.txt)
- [FFNY Online MIT license](licenses/FFNY-Online-MIT.txt)
- [Original project license](LICENSE)
- [xboxrecomp license](licenses/xboxrecomp-MIT.txt)
- [xboxrecomp third-party notice](licenses/xboxrecomp-NOTICE.txt)
- [GNU LGPL 2.1](licenses/LGPL-2.1.txt)
- [SDL3 zlib license](licenses/SDL3-zlib.txt)

## Emulator, frame generation and graphics

- **The412Banner and contributors** developed Win-FG. The port adapts it and the AMD FidelityFX optical-flow code. [Win-FG MIT license](tools/xboxrecomp/src/d3d/fg34/vendor/LICENSE) · [AMD notice](tools/xboxrecomp/src/d3d/fg34/vendor/NOTICE_FIDELITYFX_OPTICALFLOW.md)
- **lsfg-vk and lsfg-vk-android contributors** wrote the MIT-licensed Vulkan LSFG integration used by the port. [Upstream and license details](tools/xboxrecomp/src/d3d/lsfg27/UPSTREAM.md)
- **Rangga Casper and contributors** developed MostWanted-Android, credited for the FSR1/Vulkan presentation code used as a reference. [Bundled notices](android/app/src/main/assets/about/THIRD-PARTY-NOTICES.txt)

Third-party components retain their respective licenses. See the [source guide](SOURCE.md) for the paths to the Android port and build tools.
