# hakuX in the Def Jam Android port

The emulator mode uses **hakuX by rfandango and contributors**, version 1.3.0, under [GPL-3.0-or-later](LICENSE). The Android glue in this repository includes the hakuX activity, relocated SDL3 Java bindings, the launcher integration, and the XISO builder. [Bundled notices](../../android/app/src/main/assets/hakux36/licenses/)

## Core sources

Check out the [exact upstream revision](https://github.com/rfandango/hakuX/tree/a90f01ee4629977b36a76175cbd781d383056293) and follow its [Android build instructions](https://github.com/rfandango/hakuX/blob/a90f01ee4629977b36a76175cbd781d383056293/android/README.md). The pinned upstream tree specifies the FEX submodules and native dependency revisions. Apply its `tools/native/adrenotools-kgsl-context-priority.patch` as described there.

```sh
git clone https://github.com/rfandango/hakuX.git
cd hakuX
git checkout a90f01ee4629977b36a76175cbd781d383056293
git submodule update --init external/fex
git -C external/fex submodule update --init External/fmt External/range-v3 \
  External/rpmalloc External/unordered_dense External/xxhash Source/Common/cpp-optparse
```

The source snapshot used FEX `52018bf46a8279b22bce738548ab3f40db467064`, SDL3 `release-3.4.18`, Volk `1.4.304`, glslang `16.1.0`, SPIRV-Reflect `vulkan-sdk-1.4.328.0`, VMA `v3.3.0`, adrenotools `8fae8ce254dfc1344527e05301e43f37dea2df80`, liblinkernsbypass `aa3975893d83ef1bc84c321ec60c65fbf1287887`, nv2a_vsh_cpu `561fe80da57a881f89000256b459440c0178a7ce`, and libslirp `26be815b86e8d49add8c9a8b320239b9594ff03d`.

## Android integration

Use `tools/native/app-libs.sh` in the hakuX checkout to stage the rebuilt native libraries. Run [Recriar-HakuX36.py](Recriar-HakuX36.py) with `--input <staged-libraries> --output <port>/android/app/src/main/jniLibs/arm64-v8a --allow-rebuilt`. It isolates the emulator's SDL library name and JNI package from the native port. Stage the hakuX hook libraries separately in `android/app/src/main/assets/hakux36/hooks`, as required by the launcher. The Android Vulkan path can be built separately from the emulator libraries.

The upstream executable, extracted game data, and Android build outputs are generated or supplied locally. The Android integration was published from the 1.4 source snapshot. A clean build of the combined APK from this publication has not been verified.
