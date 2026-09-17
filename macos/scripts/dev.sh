#!/usr/bin/env bash
# One command from a fresh checkout to a running debug Mynah.app with live logs.
#
#   make dev            (or: macos/scripts/dev.sh)
#
# Ctrl+C stops the log stream and quits the app, the way a dev server stops.
#
# What it does, in order:
#   1. checks out the whisper.cpp submodule if it is missing
#   2. works around a toolchain whose linker cannot read the active SDK
#   3. warns about anything else holding the dictation hotkey
#   4. builds via build-app.sh (debug)
#   5. quits the previous build, launches the new bundle, streams its log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
APP="$ROOT/build/Mynah.app"
BUNDLE_ID="com.reidenxerx.mynah"

# --- 1. vendored whisper.cpp -------------------------------------------------
if [ ! -f "$ROOT/vendor/whisper.cpp/CMakeLists.txt" ]; then
  echo "checking out whisper.cpp submodule…"
  git -C "$REPO" submodule update --init --recursive
fi

# --- 2. SDK / linker skew ----------------------------------------------------
# With Command Line Tools newer than Xcode, plain `xcrun --show-sdk-path` can
# resolve to the CLT SDK while the linker comes from Xcode. The older ld then
# rejects the SDK's .tbd stubs ("tapi error: malformed file … unknown
# architecture"), and every native link fails — first visible as CMake's
# compiler check for whisper.cpp. `xcrun --sdk macosx` resolves the SDK that
# belongs to the selected developer directory, which that ld can read.
# Probed rather than assumed, so a healthy toolchain is left untouched.
can_link() {
  local dir rc=0
  dir="$(mktemp -d)"
  printf 'int main(void){return 0;}\n' > "$dir/t.c"
  cc "$dir/t.c" -o "$dir/t" >/dev/null 2>&1 || rc=1
  rm -rf "$dir"
  return $rc
}

if [ -z "${SDKROOT:-}" ] && ! can_link; then
  SDKROOT="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
  export SDKROOT
  if [ -z "$SDKROOT" ] || ! can_link; then
    echo "error: the C toolchain cannot link a trivial program, even against" >&2
    echo "       '${SDKROOT:-<no SDK found>}'. Check: xcode-select -p" >&2
    exit 1
  fi
  echo "note: default SDK does not match the linker — building with SDKROOT=$SDKROOT"
fi

# --- 3. hotkey conflicts -----------------------------------------------------
# Both the Python agent and a whiz build with dictation register the same
# global hotkey, and whichever registers first wins — which makes a fresh
# Mynah build look broken. Warn only: stopping someone's agent is their call.
conflicts="$(launchctl list 2>/dev/null | awk '/(whiz|mynah)\.dictate/ {print $3}' || true)"
if pgrep -x WhizApp >/dev/null 2>&1; then
  conflicts="$(printf '%s\n%s' "$conflicts" "WhizApp (running)" | sed '/^$/d')"
fi
if [ -n "$conflicts" ]; then
  echo "warning: these may already hold the dictation hotkey:"
  printf '%s\n' "$conflicts" | sed 's/^/  /'
  echo "  stop an agent with: launchctl unload ~/Library/LaunchAgents/<label>.plist"
fi

# --- 4. build ----------------------------------------------------------------
"$ROOT/scripts/build-app.sh" debug

# --- 5. relaunch and stream --------------------------------------------------
# Quit through AppKit rather than a signal: applicationWillTerminate frees the
# whisper context, and ggml aborts at exit if a model is still loaded.
quit_app() {
  pgrep -x MynahApp >/dev/null 2>&1 || return 0
  osascript -e "tell application id \"$BUNDLE_ID\" to quit" >/dev/null 2>&1 || true
  for _ in $(seq 1 50); do
    pgrep -x MynahApp >/dev/null 2>&1 || return 0
    sleep 0.1
  done
  echo "warning: MynahApp did not quit in 5 s — terminating it" >&2
  pkill -x MynahApp || true
}

quit_app
# Run the bundle, not the raw binary: TCC keys permissions to the bundle.
open "$APP"
echo "launched $APP — look for the bird in the menu bar. Ctrl+C quits."
echo

trap 'echo; echo "quitting Mynah…"; quit_app; exit 0' INT TERM
log stream --style compact --level debug \
  --predicate "subsystem == \"$BUNDLE_ID\"" &
wait $!
