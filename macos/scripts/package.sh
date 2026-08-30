#!/usr/bin/env bash
# Package Whiz.app into a zip you can hand to someone else.
#
# Ships as a zip rather than a dmg deliberately: a dmg picks up its own
# quarantine attribute, so the recipient has to clear it on the disk image *and*
# on the app inside. One archive, one command.
#
# The build is signed with the local `whiz-dev` identity (see
# create-signing-cert.sh), not a Developer ID, and is not notarized. That is
# fine for handing to a colleague — see INSTALL.txt in the archive — but means
# Gatekeeper blocks it until the quarantine attribute is removed. Notarization
# is what removes that step, and it needs a paid Apple Developer account.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/build/Whiz.app"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
           "$ROOT/Resources/Info.plist" 2>/dev/null || echo unknown)"
STAGE="$ROOT/build/Whiz-$VERSION"
ARCHIVE="$ROOT/build/Whiz-$VERSION.zip"

"$ROOT/scripts/build-app.sh" release

rm -rf "$STAGE" "$ARCHIVE"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"

cat > "$STAGE/INSTALL.txt" <<'TXT'
whiz — voice dictation for macOS
================================

Requires macOS 13 (Ventura) or later, on Apple Silicon.

1. Drag Whiz.app to /Applications.

2. Remove the download quarantine flag:

     xattr -dr com.apple.quarantine /Applications/Whiz.app

   This build is signed, but with a self-signed certificate rather than a paid
   Apple Developer ID, so macOS refuses to launch it until you do this. On
   macOS 15 and later the old right-click -> Open shortcut no longer works for
   unnotarized apps, so the command above is the way.

3. Open Whiz.app. A "W" appears in the menu bar. There is no Dock icon and no
   window - it is a menu bar app.

4. Click the W -> Settings... -> Recognition, and download a speech model.
   "Large v3 Turbo" is about 1.6 GB. It is stored in ~/.cache/whisper and is
   only downloaded once.

5. Click the W -> "Grant Accessibility...", then enable whiz in
   System Settings -> Privacy & Security -> Accessibility.

   Without this, whiz can hear you and transcribe correctly but cannot type the
   result into anything - it fails silently, which is confusing. The menu shows
   "Accessibility: granted" once it is working.

6. Focus any text field and press Cmd+Shift+. to start dictating. Press it
   again to stop. macOS asks for microphone access the first time.

Notes
-----
- The first dictation after launch pauses for a few seconds while the GPU
  shader library is compiled. This happens once.
- If you have to raise your voice, check System Settings -> Sound -> Input and
  make sure the input volume is not low. That matters far more than any setting
  inside whiz.
- The hotkey, language, microphone sensitivity and other options are under
  Settings... in the menu.
- Logs: log stream --predicate 'subsystem == "com.reidenxerx.whiz"'
TXT

# ditto rather than zip: it preserves the code signature and bundle structure.
# --keepParent so the archive expands into a named folder instead of scattering
# Whiz.app and INSTALL.txt loose into Downloads.
( cd "$ROOT/build" && ditto -c -k --sequesterRsrc --keepParent "Whiz-$VERSION" "$ARCHIVE" )
rm -rf "$STAGE"

echo
echo "packaged: $ARCHIVE"
du -h "$ARCHIVE" | awk '{print "  size: "$1}'
echo "  signed by: $(codesign -dvvv "$APP" 2>&1 | awk -F= '/^Authority=/{print $2; exit}')"
echo
echo "Send the zip. The recipient must run, once:"
echo "  xattr -dr com.apple.quarantine /Applications/Whiz.app"
