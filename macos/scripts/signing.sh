# Signing, shared by build-app.sh, install-app.sh and create-signing-cert.sh.
# Sourced, not run.
#
# The point of all of it is one property: every local build carries the SAME
# designated requirement, so the Accessibility grant — which TCC keys to that
# requirement — survives rebuilds. See create-signing-cert.sh for why a
# certificate gives that and an ad-hoc signature cannot.

MYNAH_KEYCHAIN="${MYNAH_KEYCHAIN:-$HOME/Library/Keychains/mynah-dev.keychain-db}"
# Not a secret — see create-signing-cert.sh ("How, and what it does NOT do").
MYNAH_KEYCHAIN_PASSWORD="mynah-dev"
MYNAH_IDENTITY_NAME="mynah-dev"

# The app's designated requirement, as TCC will record it.
mynah_designated_requirement() {
  codesign -d -r- "$1" 2>&1 | sed -n 's/^designated => //p'
}

# True when the dedicated identity exists and codesign can really sign with
# it — tested by signing a throwaway binary, the only answer that means
# anything.
mynah_identity_usable() {
  [ -f "$MYNAH_KEYCHAIN" ] || return 1
  security unlock-keychain -p "$MYNAH_KEYCHAIN_PASSWORD" "$MYNAH_KEYCHAIN" >/dev/null 2>&1 || return 1
  local probe
  probe="$(mktemp -d)/probe"
  cp /bin/echo "$probe" 2>/dev/null || return 1
  local ok=1
  codesign --force --keychain "$MYNAH_KEYCHAIN" --sign "$MYNAH_IDENTITY_NAME" "$probe" \
    >/dev/null 2>&1 && ok=0
  rm -rf "$(dirname "$probe")"
  return $ok
}

# mynah_sign <bundle> [stable]
#
# Signs with, in order: $MYNAH_SIGN_IDENTITY (an explicit choice, e.g. a
# Developer ID); the dedicated mynah-dev keychain; a mynah-dev identity left in
# the login keychain by the old version of create-signing-cert.sh; and only
# then ad-hoc. With "stable", ad-hoc is an error rather than a fallback — an
# install that would lose the grant on the next rebuild is not an install.
mynah_sign() {
  local app="$1" mode="${2:-any}"
  local how
  if [ -n "${MYNAH_SIGN_IDENTITY:-}" ]; then
    codesign --force --sign "$MYNAH_SIGN_IDENTITY" "$app" || return 1
    how="'$MYNAH_SIGN_IDENTITY'"
  elif [ -f "$MYNAH_KEYCHAIN" ]; then
    security unlock-keychain -p "$MYNAH_KEYCHAIN_PASSWORD" "$MYNAH_KEYCHAIN" || return 1
    codesign --force --keychain "$MYNAH_KEYCHAIN" --sign "$MYNAH_IDENTITY_NAME" "$app" || return 1
    how="'$MYNAH_IDENTITY_NAME' ($MYNAH_KEYCHAIN)"
  elif security find-certificate -c "$MYNAH_IDENTITY_NAME" >/dev/null 2>&1 &&
       codesign --force --sign "$MYNAH_IDENTITY_NAME" "$app" 2>/dev/null; then
    how="'$MYNAH_IDENTITY_NAME' (login keychain, from the old setup script)"
  else
    if [ "$mode" = stable ]; then
      echo "error: no stable signing identity — run macos/scripts/create-signing-cert.sh" >&2
      return 1
    fi
    codesign --force --sign - "$app" || return 1
    how="ad-hoc"
  fi

  codesign --verify --strict "$app" || { echo "error: the signature does not verify" >&2; return 1; }

  local requirement
  requirement="$(mynah_designated_requirement "$app")"
  case "$requirement" in
    *cdhash*)
      # Keyed to this exact build: the grant will not survive a rebuild.
      if [ "$mode" = stable ]; then
        echo "error: signed, but the requirement is build-specific ($requirement)" >&2
        return 1
      fi
      echo "signed $how — Accessibility must be re-granted after each rebuild."
      echo "  run macos/scripts/create-signing-cert.sh once to stop that."
      ;;
    *)
      echo "signed with $how"
      echo "  requirement: $requirement"
      ;;
  esac
}
