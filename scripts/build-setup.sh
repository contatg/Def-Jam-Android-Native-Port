#!/bin/bash
# Build the game-free macOS setup (Apple Silicon): Def Jam Setup.app inside a disk image.
# Never analyzes or builds a dump. Needs the Xcode Command Line Tools and Python 3.11+;
# pass --development for a local prototype from a dirty checkout (release workflows must not).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
DEVELOPMENT=()
[ "${1:-}" = "--development" ] && DEVELOPMENT=(--development)
VERSION=$(sed -n 's/^project(defjam_recomp VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
OUT="$ROOT/build/setup-macos"
rm -rf "$OUT"
mkdir -p "$OUT"
# Python 3.11 or newer (the system's python3 is older): $PYTHON, or the first one found.
if [ -z "${PYTHON:-}" ]; then
    for candidate in python3.14 python3.13 python3.12 python3.11 python3; do
        if command -v "$candidate" > /dev/null && "$candidate" -c 'import sys; sys.exit(sys.version_info < (3, 11))'; then
            PYTHON="$candidate"; break
        fi
    done
fi
[ -n "${PYTHON:-}" ] || { echo "Python 3.11 or newer is required" >&2; exit 1; }

# 1. The launcher app that setup copies next to the game.
LAUNCHER="$OUT/launcher/Def Jam Recompiled.app"
mkdir -p "$LAUNCHER/Contents/MacOS" "$LAUNCHER/Contents/Resources"
sed "s/@VERSION@/$VERSION/g" setup/macos/Launcher/Info.plist > "$LAUNCHER/Contents/Info.plist"
cp branding/icon/macos/defjam.icns "$LAUNCHER/Contents/Resources/defjam.icns"
swiftc -O -target arm64-apple-macos11.0 setup/macos/Launcher/main.swift -o "$LAUNCHER/Contents/MacOS/DefJamLauncher"
codesign --force --sign - "$LAUNCHER"

# 2. The payload: source, Python, CMake, Ninja, the runtime libraries and that launcher.
"$PYTHON" scripts/package-setup.py --platform macos-arm64 --launcher "$LAUNCHER" \
    --output "$OUT/setup-payload.zip" "${DEVELOPMENT[@]+"${DEVELOPMENT[@]}"}"

# 3. The wizard app, carrying the payload and its digest.
APP="$OUT/image/Def Jam Setup.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
SHA=$(shasum -a 256 "$OUT/setup-payload.zip" | cut -d' ' -f1)
echo "let payloadSHA256 = \"$SHA\"" > "$OUT/PayloadDigest.swift"
swiftc -O -target arm64-apple-macos11.0 setup/macos/Setup/main.swift "$OUT/PayloadDigest.swift" \
    -o "$APP/Contents/MacOS/DefJamSetup"
cp "$OUT/setup-payload.zip" "$APP/Contents/Resources/payload.zip"
sed "s/@VERSION@/$VERSION/g" setup/macos/Setup/Info.plist > "$APP/Contents/Info.plist"
cp branding/icon/macos/defjam.icns "$APP/Contents/Resources/defjam.icns"
codesign --force --sign - "$APP"

# 4. The disk image: the app, the first-run notes and the third-party licenses.
cp setup/macos/README-FIRST.txt "$OUT/image/READ ME FIRST.txt"
mkdir "$OUT/image/Licenses"
unzip -q -o "$OUT/setup-payload.zip" 'licenses/*' -d "$OUT/licenses-unpacked"
cp "$OUT"/licenses-unpacked/licenses/* "$OUT/image/Licenses/"
cp LICENSE "$OUT/image/Licenses/Def-Jam-Recompiled-MIT.txt"
hdiutil create -volname "Def Jam Recompiled Setup" -srcfolder "$OUT/image" -ov -format UDZO "$OUT/DefJamSetup.dmg" > /dev/null

# 5. Inventory, checksum and provenance of exactly what a release would attach.
"$PYTHON" scripts/check-setup-assets.py --payload "$OUT/setup-payload.zip" --installer "$OUT/DefJamSetup.dmg" \
    --output-dir "$OUT/setup-assets" "${DEVELOPMENT[@]+"${DEVELOPMENT[@]}"}"
echo "Built $OUT/setup-assets"
