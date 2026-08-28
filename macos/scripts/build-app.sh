#!/usr/bin/env bash
# Assemble Whiz.app from the SwiftPM executable.
#
# SwiftPM emits a bare binary; macOS needs a bundle for LSUIElement, for the
# microphone usage string, and above all for a stable bundle identifier — which
# is what TCC keys permission grants to. Running the raw binary works for a
# smoke test but will re-prompt for Accessibility on every rebuild.
set -euo pipefail

CONFIGURATION="${1:-debug}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

swift build --package-path "$ROOT" -c "$CONFIGURATION"
BUILD_DIR="$(swift build --package-path "$ROOT" -c "$CONFIGURATION" --show-bin-path)"
APP="$ROOT/build/Whiz.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD_DIR/WhizApp" "$APP/Contents/MacOS/WhizApp"
cp "$ROOT/Resources/Info.plist" "$APP/Contents/Info.plist"

# Ad-hoc signature. Enough for local runs; real distribution needs a Developer
# ID signature plus notarization, and every bundled dylib signed individually
# once the Python runtime is embedded.
codesign --force --deep --sign - "$APP" 2>/dev/null || \
  echo "warning: ad-hoc codesign failed; permissions may not stick"

echo "built $APP"
