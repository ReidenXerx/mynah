#!/usr/bin/env bash
# Assemble Mynah.app.
#
# SwiftPM emits a bare binary; macOS needs a bundle for LSUIElement, for the
# microphone usage string, and above all for a stable bundle identifier — which
# is what TCC keys permission grants to. Running the raw binary works for a
# smoke test but re-prompts for Accessibility on every rebuild.
#
# Two build paths. SwiftPM is preferred, but it needs full Xcode: with Command
# Line Tools alone its manifest fails to link against libPackageDescription
# (even a three-line package fails), so we fall back to invoking swiftc over the
# sources directly. The fallback produces an identical binary — it just cannot
# run the test suite.
set -euo pipefail

CONFIGURATION="${1:-debug}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/build/Mynah.app"
VENDOR="$ROOT/vendor/install"

# Match Package.swift and build-core.sh. Keep in sync.
DEPLOYMENT_TARGET="13.0"

# Build the core and the pinned whisper.cpp if they are not already there.
# Statically linking our own build is what makes the app distributable: no
# Homebrew requirement, no absolute /opt/homebrew paths, and a deployment
# target we control.
"$ROOT/scripts/build-core.sh" || exit 1

if [ ! -f "$VENDOR/lib/libmynah.a" ]; then
  echo "error: the core is not built at $VENDOR" >&2
  exit 1
fi

# Link order matters for static archives: dependents before dependencies.
# mynah -> whisper -> ggml -> ggml-metal/cpu -> ggml-base.
#
# -lc++ is also required. the core, whisper.cpp and ggml are C++; Swift links
# libc++ only when it knows C++ is involved, and a static archive reached
# through a C module map does not tell it. Without this the link fails on
# std:: symbols and ___gxx_personality_v0.
MYNAH_LIBS=(
  "$VENDOR/lib/libmynah.a"
  "$VENDOR/lib/libwhisper.a"
  "$VENDOR/lib/libggml.a"
  "$VENDOR/lib/libggml-metal.a"
  "$VENDOR/lib/libggml-cpu.a"
  "$VENDOR/lib/libggml-base.a"
)

build_with_swiftpm() {
  swift build --package-path "$ROOT" -c "$CONFIGURATION" >/dev/null 2>&1 || return 1
  swift build --package-path "$ROOT" -c "$CONFIGURATION" --show-bin-path 2>/dev/null
}

# Returns the directory containing the built binary, or fails.
#
# The `echo` at the end used to mask a compiler failure: a function's exit
# status is that of its last command, so a swiftc error still returned 0 and the
# caller happily copied the *previous* binary into the bundle. That shipped a
# stale build that looked successful — the worst possible failure mode.
build_with_swiftc() {
  local out="$ROOT/build/obj"
  mkdir -p "$out"
  local opt=""
  [ "$CONFIGURATION" = "release" ] && opt="-O"
  swiftc \
    -sdk "$(xcrun --show-sdk-path)" \
    -target "arm64-apple-macosx$DEPLOYMENT_TARGET" \
    -swift-version 6 -parse-as-library $opt \
    -Xcc "-I$VENDOR/include" \
    -I "$ROOT/Sources/CMynah" \
    "${MYNAH_LIBS[@]}" \
    -lc++ \
    -framework Metal -framework MetalKit -framework Accelerate \
    -framework Foundation -framework CoreML \
    $(find "$ROOT/Sources/MynahApp" -name '*.swift') \
    -o "$out/MynahApp" || return 1
  echo "$out"
}

echo "building ($CONFIGURATION)…"
if BIN_DIR="$(build_with_swiftpm)" && [ -n "$BIN_DIR" ]; then
  echo "  via SwiftPM"
else
  echo "  SwiftPM unavailable (needs full Xcode) — falling back to swiftc"
  if ! BIN_DIR="$(build_with_swiftc)"; then
    echo "error: build failed — not updating $APP" >&2
    exit 1
  fi
fi

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN_DIR/MynahApp" "$APP/Contents/MacOS/MynahApp"
cp "$ROOT/Resources/Info.plist" "$APP/Contents/Info.plist"

# The app icon, rendered from the repo's mark. A bundle without CFBundleIconFile
# shows the generic placeholder tile in Finder and System Settings — which is how
# a build gets mistaken for somebody else's.
ICON_SRC="$ROOT/docs/assets/mynah-mark.svg"
if [ -f "$ICON_SRC" ]; then
  if swift "$ROOT/scripts/make-icon.swift" "$ICON_SRC" "$APP/Contents/Resources/mynah.icns"; then
    /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string mynah" "$APP/Contents/Info.plist" 2>/dev/null \
      || /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile mynah" "$APP/Contents/Info.plist"
  else
    echo "warning: icon rendering failed — the bundle will use the placeholder tile" >&2
  fi
fi

# The build stamp: CFBundleVersion carries the build time, so the running app
# is identifiable in the menu and in the log — the question "is this the build
# I just made?" should never need a filesystem check to answer.
BUILD_STAMP="$(date '+%Y.%m.%d %H:%M')"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $BUILD_STAMP" "$APP/Contents/Info.plist" 2>/dev/null \
  || /usr/libexec/PlistBuddy -c "Add :CFBundleVersion string $BUILD_STAMP" "$APP/Contents/Info.plist"
echo "build stamp: $BUILD_STAMP"

# Signing (scripts/signing.sh): a stable identity when there is one, so the
# Accessibility grant survives this rebuild; ad-hoc with a warning when there
# is not. MYNAH_REQUIRE_STABLE_SIGNATURE=1 (install-app.sh sets it) makes an
# ad-hoc signature a failure instead — a build that loses the grant on the
# next rebuild must never be installed.
# shellcheck source=signing.sh
source "$ROOT/scripts/signing.sh"
if ! mynah_sign "$APP" "$([ "${MYNAH_REQUIRE_STABLE_SIGNATURE:-0}" = 1 ] && echo stable || echo any)"; then
  echo "error: signing failed — $APP is not usable" >&2
  exit 1
fi

echo "built $APP"
