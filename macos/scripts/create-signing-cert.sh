#!/usr/bin/env bash
# Create the stable code-signing identity local builds are signed with.
#
# Why: an ad-hoc signature (`codesign -s -`) has no stable identity. Its
# designated requirement is the build's cdhash, which changes with every
# rebuild, so macOS treats each build as a different app — TCC drops the
# Accessibility grant, silently: dictation appears to work while typing
# nothing, and the settings list fills with stale "mynah" entries.
#
# Signed with a certificate instead, the requirement becomes
#     identifier "com.reidenxerx.mynah" and certificate leaf = H"…"
# which every rebuild signed with the same certificate satisfies. The grant
# is made once and survives rebuilds and reinstalls.
#
# How, and what it does NOT do:
#   - The key and certificate live in a keychain of their own,
#     ~/Library/Keychains/mynah-dev.keychain-db — not the login keychain, and
#     not on the keychain search list. Nothing else on the machine sees it.
#   - NO trust settings are changed. codesign does not need the certificate to
#     be trusted to sign with it, and TCC does not evaluate trust — it checks
#     that the signature satisfies the recorded requirement, which names the
#     certificate's hash. (An earlier version marked the certificate as a
#     trusted root in the login keychain, which would have let anything
#     holding the key sign code the Mac accepts; that was never needed.)
#   - The keychain's password is a fixed string in scripts/signing.sh. That
#     is not a secret and is not meant to be one: anything running as you
#     could already ask codesign to use an identity allowed for it. What
#     protects this key is that it can sign exactly one thing — builds of
#     this app, for this user's TCC grants.
#
# This is for LOCAL builds only. Anything handed to another person needs a
# Developer ID certificate and notarization (see scripts/package.sh).
#
# Run once — `make install-app` runs it for you when the identity is missing:
#   macos/scripts/create-signing-cert.sh
#
# To remove it (the next build falls back to ad-hoc signing):
#   security delete-keychain ~/Library/Keychains/mynah-dev.keychain-db
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=signing.sh
source "$ROOT/scripts/signing.sh"

if mynah_identity_usable; then
  echo "signing identity '$MYNAH_IDENTITY_NAME' is ready in $MYNAH_KEYCHAIN — nothing to do"
  exit 0
fi

if [ -e "$MYNAH_KEYCHAIN" ]; then
  echo "the keychain at $MYNAH_KEYCHAIN exists but cannot sign — recreating it"
  security delete-keychain "$MYNAH_KEYCHAIN" 2>/dev/null || rm -f "$MYNAH_KEYCHAIN"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "creating the code-signing identity '$MYNAH_IDENTITY_NAME'…"
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
  -subj "/CN=$MYNAH_IDENTITY_NAME" \
  -addext "basicConstraints=critical,CA:false" \
  -addext "keyUsage=critical,digitalSignature" \
  -addext "extendedKeyUsage=critical,codeSigning" \
  2>/dev/null

# The PKCS#12 bundle is only a carrier into the keychain and is deleted on
# exit, but its password cannot be empty: `security import` rejects an
# empty-password bundle with a "wrong password?" message that reads like a
# mismatch rather than a refusal. -legacy where supported: OpenSSL 3 defaults
# to AES-256 + SHA-256, which older Security framework builds cannot read.
# LibreSSL (/usr/bin/openssl) has no such flag and writes the legacy form.
P12_PASSWORD="mynah-dev-transient"
PKCS12_COMPAT=""
if openssl pkcs12 -help 2>&1 | grep -q -- "-legacy"; then
  PKCS12_COMPAT="-legacy"
fi
openssl pkcs12 -export -out "$TMP/cert.p12" \
  -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -passout "pass:$P12_PASSWORD" \
  $PKCS12_COMPAT -macalg sha1

security create-keychain -p "$MYNAH_KEYCHAIN_PASSWORD" "$MYNAH_KEYCHAIN"
# No auto-lock timeout: signing unlocks it anyway, but a build that runs past
# the default five minutes should not find it locked halfway.
security set-keychain-settings "$MYNAH_KEYCHAIN"
security unlock-keychain -p "$MYNAH_KEYCHAIN_PASSWORD" "$MYNAH_KEYCHAIN"
security import "$TMP/cert.p12" -k "$MYNAH_KEYCHAIN" -P "$P12_PASSWORD" -T /usr/bin/codesign >/dev/null
# Let codesign use the key without a GUI "allow access?" prompt — which a
# build run from a terminal or an agent could not answer.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s \
  -k "$MYNAH_KEYCHAIN_PASSWORD" "$MYNAH_KEYCHAIN" >/dev/null

# Confirm by signing something, not by listing identities: a listing shows
# identities codesign will still refuse.
if mynah_identity_usable; then
  echo "identity '$MYNAH_IDENTITY_NAME' created and verified — it can sign"
else
  echo "error: the identity was created but codesign cannot use it" >&2
  exit 1
fi

if security find-certificate -c "$MYNAH_IDENTITY_NAME" "$HOME/Library/Keychains/login.keychain-db" \
    >/dev/null 2>&1; then
  echo
  echo "note: an older '$MYNAH_IDENTITY_NAME' identity is in your login keychain, from the"
  echo "      previous version of this script (which also marked it trusted). It is no"
  echo "      longer used. To remove it:  security delete-identity -c $MYNAH_IDENTITY_NAME"
fi

echo
echo "Builds are now signed with it. Grant Accessibility once to the installed app;"
echo "every rebuild signed with this identity keeps the grant."
