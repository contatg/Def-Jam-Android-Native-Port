#!/bin/bash
# Build the game-free Linux setup (x86-64, Steam Deck included):
# DefJamSetup-<version>-linux-x64.tar.gz, a folder holding DefJamSetup.sh, the
# unpacked payload, a short "READ ME FIRST" and the third-party licenses.
# Never analyzes or builds a dump. Needs Python 3.11+ with pip; pass
# --development for a local prototype from a dirty checkout (release workflows must not).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
DEVELOPMENT=()
[ "${1:-}" = "--development" ] && DEVELOPMENT=(--development)
VERSION=$(sed -n 's/^project(defjam_recomp VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
OUT="$ROOT/build/setup-linux"
NAME="DefJamSetup-$VERSION-linux-x64"
rm -rf "$OUT"
mkdir -p "$OUT/image/$NAME"
PYTHON="${PYTHON:-python3}"
"$PYTHON" -c 'import sys; sys.exit(sys.version_info < (3, 11))' || { echo "Python 3.11 or newer is required" >&2; exit 1; }

# 1. The payload: source, Python, the wizard, the engine and the launcher template.
"$PYTHON" scripts/package-setup.py --platform linux-x64 --output "$OUT/setup-payload.zip" \
    "${DEVELOPMENT[@]+"${DEVELOPMENT[@]}"}"

# 2. The folder a player unpacks: the payload unpacked (execute bits kept), the
#    entry script, the notes and the licenses.
"$PYTHON" - "$OUT/setup-payload.zip" "$OUT/image/$NAME/payload" <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('macos_payload', 'scripts/macos_payload.py')
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
helpers.unpack(sys.argv[1], sys.argv[2])
EOF
install -m 755 setup/linux/DefJamSetup.sh "$OUT/image/$NAME/DefJamSetup.sh"
cp setup/linux/README-FIRST.txt "$OUT/image/$NAME/READ ME FIRST.txt"
mkdir "$OUT/image/$NAME/Licenses"
cp "$OUT/image/$NAME"/payload/licenses/* "$OUT/image/$NAME/Licenses/"
cp LICENSE "$OUT/image/$NAME/Licenses/Def-Jam-Recompiled-MIT.txt"
tar -C "$OUT/image" --owner=0 --group=0 --numeric-owner --sort=name -czf "$OUT/$NAME.tar.gz" "$NAME"

# 3. Inventory, checksum and provenance of exactly what a release would attach.
"$PYTHON" scripts/check-setup-assets.py --payload "$OUT/setup-payload.zip" --installer "$OUT/$NAME.tar.gz" \
    --output-dir "$OUT/setup-assets" "${DEVELOPMENT[@]+"${DEVELOPMENT[@]}"}"
echo "Built $OUT/setup-assets"
