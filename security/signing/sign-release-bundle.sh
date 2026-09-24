#!/usr/bin/env bash
# Release step: re-sign a development-signed RAUC bundle with the release key
# in the HSM. The build server never sees a release key; this runs on the
# signing host, and the only input it trusts is a bundle whose development
# signature verifies.
#
#   sign-release-bundle.sh <dev-signed.raucb> <release.raucb>
#
# Needs: rauc (here: Yocto's rauc-native through oe-run-native, so the Yocto
# build environment must be sourced), OpenSSL's pkcs11 engine (libp11).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"
in="$(realpath "$1")"; out="$(realpath -m "$2")"
: "${RAUC_DEV_KEYRING:=/opt/yocto/local-config/rauc-dev/dev-ca.cert.pem}"
: "${RELEASE_CERT:=$SIGNING_HOME/update/release-1.cert.pem}"
: "${UPDATE_CA:=$SIGNING_HOME/update/update-ca.cert.pem}"
# After a rotation (hsm-rotate-release-key.sh):
#   RELEASE_CERT=.../release-2.cert.pem RELEASE_KEY_LABEL=update-release-2-p256
: "${RELEASE_KEY_LABEL:=update-release-p256}"
: "${OPENSSL_ENGINES:=/usr/lib/x86_64-linux-gnu/engines-3}"

# RAUC reads the module and the PIN from its environment, not from argv.
OPENSSL_ENGINES="$OPENSSL_ENGINES" RAUC_PKCS11_MODULE="$PKCS11_MODULE" \
RAUC_PKCS11_PIN="$(cat "$HSM_PIN_FILE")" \
	oe-run-native rauc-native rauc resign \
	-C keyring:check-purpose=codesign \
	--keyring="$RAUC_DEV_KEYRING" \
	--signing-keyring="$UPDATE_CA" \
	--cert="$RELEASE_CERT" \
	--key="pkcs11:token=$HSM_TOKEN_LABEL;object=$RELEASE_KEY_LABEL;type=private" \
	"$in" "$out"

printf '%s %s resign %s -> %s sha256=%s\n' "$(date -Is)" "$(id -un)" "$(basename "$in")" \
	"$(basename "$out")" "$(sha256sum "$out" | cut -d' ' -f1)" >> "$SIGNING_HOME/signing-audit.log"
