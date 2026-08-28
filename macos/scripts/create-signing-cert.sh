#!/usr/bin/env bash
# Create a stable self-signed code-signing identity for local development.
#
# Why: an ad-hoc signature (`codesign -s -`) has no stable identity, so its
# cdhash changes with every rebuild and macOS treats each build as a different
# app. TCC then drops the Accessibility grant, silently — dictation appears to
# work while typing nothing, and the settings list fills with stale "whiz"
# entries.
#
# A self-signed certificate gives a designated requirement that stays constant
# across rebuilds, so the grant is made once. This is for local development
# only; distribution needs a Developer ID certificate and notarization.
#
# Run once:  macos/scripts/create-signing-cert.sh
set -euo pipefail

NAME="whiz-dev"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"

# Is the identity actually usable? Test by signing something, not by reading
# `security find-identity`: even with -v ("valid identities only") it still
# *lists* broken ones, annotated like
#     1) A470... "whiz-dev" (Invalid Key Usage for policy)
# so grepping for the name matches a certificate codesign will refuse. A trial
# signature is the only answer that means anything.
identity_is_usable() {
  local probe
  probe="$(mktemp -d)/probe"
  cp /bin/echo "$probe" 2>/dev/null || return 1
  local ok=1
  codesign --force --sign "$NAME" "$probe" >/dev/null 2>&1 && ok=0
  rm -rf "$(dirname "$probe")"
  return $ok
}

if identity_is_usable; then
  echo "usable identity '$NAME' already exists — nothing to do"
  echo "build with: macos/scripts/build-app.sh"
  exit 0
fi

if security find-certificate -c "$NAME" >/dev/null 2>&1; then
  echo "removing existing but unusable '$NAME' identity…"
  # delete-identity removes the certificate *and* its private key; older systems
  # only have delete-certificate, which orphans the key.
  security delete-identity -c "$NAME" >/dev/null 2>&1 \
    || while security delete-certificate -c "$NAME" >/dev/null 2>&1; do :; done
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "creating self-signed code-signing certificate '$NAME'…"
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
  -subj "/CN=$NAME" \
  -addext "basicConstraints=critical,CA:false" \
  -addext "keyUsage=critical,digitalSignature" \
  -addext "extendedKeyUsage=critical,codeSigning" \
  2>/dev/null

# The password must be non-empty. `security import` rejects an empty-password
# PKCS#12 with "MAC verification failed during PKCS12 import (wrong password?)",
# which blames the password in a way that sounds like a mismatch rather than a
# refusal to accept an empty one. The value is irrelevant — the bundle is
# deleted seconds later — but it cannot be blank.
#
# -legacy is also passed where supported: OpenSSL 3.x defaults to AES-256-CBC +
# SHA-256, which older Security framework builds cannot read. LibreSSL
# (/usr/bin/openssl) has no such flag and already writes the legacy encoding.
P12_PASSWORD="whiz-dev-transient"
PKCS12_COMPAT=""
if openssl pkcs12 -help 2>&1 | grep -q -- "-legacy"; then
  PKCS12_COMPAT="-legacy"
fi
openssl pkcs12 -export -out "$TMP/cert.p12" \
  -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -passout "pass:$P12_PASSWORD" \
  $PKCS12_COMPAT -macalg sha1

# -T /usr/bin/codesign lets codesign use the key without prompting each time.
security import "$TMP/cert.p12" -k "$KEYCHAIN" -P "$P12_PASSWORD" -T /usr/bin/codesign

# Trust it for code signing in the user domain. Without -d this does not need
# sudo; codesign only requires the identity to be present and trusted enough to
# build a chain locally.
security add-trusted-cert -r trustRoot -p codeSign -k "$KEYCHAIN" "$TMP/cert.pem" \
  || echo "note: could not set trust automatically — open Keychain Access, find" \
          "'$NAME', and set 'Code Signing' to 'Always Trust' if signing fails"

# Confirm by signing, for the same reason as above.
if identity_is_usable; then
  echo "identity '$NAME' verified — it can sign"
else
  echo "warning: '$NAME' imported but codesign will not use it:" >&2
  security find-identity -p codesigning 2>&1 | grep "$NAME" >&2 || true
  exit 1
fi

echo
echo "done. Build signed with it:"
echo "  WHIZ_SIGN_IDENTITY=$NAME macos/scripts/build-app.sh"
echo
echo "Grant Accessibility once after the first signed build; it will survive"
echo "subsequent rebuilds."
