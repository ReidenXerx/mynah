#!/usr/bin/env bash
# Build Mynah.app (release) and install it at one fixed place, keeping its
# Accessibility and Microphone grants across every rebuild.
#
#   make install-app            (or: macos/scripts/install-app.sh)
#   MYNAH_NO_LAUNCH=1 make install-app     install without launching
#
# Two things make a grant survive, and this script owns both:
#
#   1. The same signing identity every time. TCC keys a grant to the app's
#      designated requirement. Ad-hoc signed, that is the build's cdhash and
#      every rebuild is a stranger; signed with the mynah-dev certificate it
#      names the certificate, which every rebuild satisfies. The identity is
#      created on first run (create-signing-cert.sh), and an ad-hoc build is
#      refused rather than installed.
#
#   2. The same place every time. Start-at-login (SMAppService) registers the
#      app at the path it runs from, and build-app.sh deletes macos/build on
#      every build — so a login item enabled from a dev build points at a path
#      that keeps vanishing. The installed copy is replaced in place at one
#      path, so the login item and the grant both keep pointing at it.
#
# The grant itself is still yours to give — once: System Settings → Privacy &
# Security → Accessibility, enable mynah. Nothing here can or should do that.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILT="$ROOT/build/Mynah.app"
BUNDLE_ID="com.reidenxerx.mynah"
# shellcheck source=signing.sh
source "$ROOT/scripts/signing.sh"

# --- where it lives -------------------------------------------------------------
# /Applications when we may write there (an admin account can, without sudo);
# otherwise ~/Applications, which macOS treats as an applications folder too.
DEST="${MYNAH_INSTALL_DIR:-/Applications}"
if [ ! -w "$DEST" ]; then
  DEST="$HOME/Applications"
  mkdir -p "$DEST"
fi
TARGET="$DEST/Mynah.app"

# --- 1. a stable identity ---------------------------------------------------------
if [ -z "${MYNAH_SIGN_IDENTITY:-}" ] && ! mynah_identity_usable; then
  echo "no stable signing identity yet — creating one (a one-time step)"
  "$ROOT/scripts/create-signing-cert.sh"
fi

# --- 2. build, refusing anything that would lose the grant -----------------------------
MYNAH_REQUIRE_STABLE_SIGNATURE=1 "$ROOT/scripts/build-app.sh" release

NEW_REQUIREMENT="$(mynah_designated_requirement "$BUILT")"

# --- 3. will the grant carry over? ------------------------------------------------------
# The question TCC will ask the new build is exactly this one: does it satisfy
# the requirement recorded for the copy that was granted? Ask it now, before
# replacing anything, so a change of identity is announced rather than
# discovered as an app that silently types nothing.
if [ -d "$TARGET" ]; then
  OLD_REQUIREMENT="$(mynah_designated_requirement "$TARGET")"
  if codesign --verify -R="$OLD_REQUIREMENT" "$BUILT" >/dev/null 2>&1; then
    echo "same identity as the installed copy — its Accessibility grant carries over"
  else
    echo "note: the new build does NOT satisfy the installed copy's requirement:"
    echo "        installed: $OLD_REQUIREMENT"
    echo "        new:       $NEW_REQUIREMENT"
    echo "      macOS will ask for Accessibility once more; after that, rebuilds keep it."
  fi
fi

# --- 4. quit the running one, gracefully ---------------------------------------------------
# Through AppKit rather than a signal: applicationWillTerminate frees the
# whisper context, and ggml aborts at exit if a model is still loaded. Checked
# first, because `tell application id …` LAUNCHES an app that is not running.
quit_running() {
  pgrep -x MynahApp >/dev/null 2>&1 || return 0
  osascript -e "tell application id \"$BUNDLE_ID\" to quit" >/dev/null 2>&1 || true
  for _ in $(seq 1 50); do
    pgrep -x MynahApp >/dev/null 2>&1 || return 0
    sleep 0.1
  done
  echo "warning: MynahApp did not quit in 5 s — terminating it" >&2
  pkill -x MynahApp || true
}
quit_running

# --- 5. replace in place ---------------------------------------------------------------------
# Copy next to the target first, then swap: the installed app is never half
# written, and a failed copy leaves the old one where it was. ditto keeps the
# signature and bundle structure intact.
STAGING="$DEST/.Mynah.app.installing"
rm -rf "$STAGING"
ditto "$BUILT" "$STAGING"
codesign --verify --strict "$STAGING"
rm -rf "$TARGET"
mv "$STAGING" "$TARGET"
echo "installed $TARGET"
echo "  requirement: $NEW_REQUIREMENT"

# --- 6. launch ----------------------------------------------------------------------------------
if [ "${MYNAH_NO_LAUNCH:-0}" != 1 ]; then
  open "$TARGET"
  echo "launched — the bird is in the menu bar."
fi

echo
echo "First install only: System Settings → Privacy & Security → Accessibility → enable mynah"
echo "(or the menu's \"Grant Accessibility…\"). Later rebuilds keep it: run this again."
