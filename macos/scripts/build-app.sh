#!/usr/bin/env bash
# Assemble Whiz.app.
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
APP="$ROOT/build/Whiz.app"
VENDOR="$ROOT/vendor/install"

# Match Package.swift and build-whisper.sh. Keep in sync.
DEPLOYMENT_TARGET="13.0"

# Build the vendored whisper.cpp if it is not already there. Statically linking
# our own build is what makes the app distributable: no Homebrew requirement, no
# absolute /opt/homebrew paths, and a deployment target we control.
"$ROOT/scripts/build-whisper.sh" || exit 1

if [ ! -f "$VENDOR/lib/libwhisper.a" ]; then
  echo "error: vendored whisper.cpp not built at $VENDOR" >&2
  exit 1
fi

# Link order matters for static archives: dependents before dependencies.
# whisper -> ggml -> ggml-metal/cpu -> ggml-base.
#
# -lc++ is also required. whisper.cpp and ggml are C++; Swift links libc++ only
# when it knows C++ is involved, and a static archive reached through a C module
# map does not tell it. Without this the link fails on std:: symbols and
# ___gxx_personality_v0.
WHISPER_LIBS=(
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
    -I "$ROOT/Sources/CWhisper" \
    "${WHISPER_LIBS[@]}" \
    -lc++ \
    -framework Metal -framework MetalKit -framework Accelerate \
    -framework Foundation -framework CoreML \
    $(find "$ROOT/Sources/WhizApp" -name '*.swift') \
    -o "$out/WhizApp" || return 1
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
cp "$BIN_DIR/WhizApp" "$APP/Contents/MacOS/WhizApp"
cp "$ROOT/Resources/Info.plist" "$APP/Contents/Info.plist"

# Signing. An ad-hoc signature changes on every rebuild, so macOS sees a
# different app each time and silently drops the Accessibility grant. Set
# WHIZ_SIGN_IDENTITY to a stable self-signed identity to avoid that — create one
# with scripts/create-signing-cert.sh. Distribution needs a Developer ID
# certificate plus notarization, and every bundled dylib signed individually
# once the Python runtime is embedded.
IDENTITY="${WHIZ_SIGN_IDENTITY:-}"
if [ -z "$IDENTITY" ] && security find-certificate -c whiz-dev >/dev/null 2>&1; then
  IDENTITY="whiz-dev"   # use it automatically once it exists
fi

if [ -n "$IDENTITY" ]; then
  codesign --force --sign "$IDENTITY" "$APP" \
    && echo "signed with '$IDENTITY' (stable across rebuilds)" \
    || echo "warning: signing with '$IDENTITY' failed"
else
  codesign --force --sign - "$APP" 2>/dev/null \
    || echo "warning: ad-hoc codesign failed; permissions may not stick"
  echo "note: ad-hoc signed — Accessibility must be re-granted after each rebuild."
  echo "      run scripts/create-signing-cert.sh once to stop that."
fi

echo "built $APP"
