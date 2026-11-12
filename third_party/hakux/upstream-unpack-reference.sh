#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
destination="${1:-$PWD/source}"
if [[ -e "$destination" ]]; then
    printf 'O destino já existe: %s\n' "$destination" >&2
    exit 1
fi

extract() {
    mkdir -p "$2"
    tar -xzf "$1" -C "$2" --strip-components=1
}

core="$destination/core"
deps="$destination/native-deps"
extract hakux-v1.3.0-source.tar.gz "$core"
extract dependencies/FEX.tar.gz "$core/external/fex"
for lib in fmt range-v3 rpmalloc unordered_dense xxhash; do
    extract "dependencies/fex-$lib.tar.gz" "$core/external/fex/External/$lib"
done
extract dependencies/fex-cpp-optparse.tar.gz "$core/external/fex/Source/Common/cpp-optparse"
for lib in volk glslang spirv_reflect vma SDL3 adrenotools libslirp; do
    extract "dependencies/$lib.tar.gz" "$deps/$lib"
done
extract dependencies/nv2a_vsh_cpu.tar.gz "$deps/root/subprojects/nv2a_vsh_cpu"
extract dependencies/linkernsbypass.tar.gz "$deps/adrenotools/lib/linkernsbypass"
git -C "$deps/adrenotools" apply "$(realpath "$core/tools/native/adrenotools-kgsl-context-priority.patch")"
printf 'Fontes do núcleo: %s\nDependências: %s\n' "$core" "$deps"
