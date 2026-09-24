#!/usr/bin/env bash
# After hsm-rotate-release-key.sh: which bundles does a device accept, with
# which keyring? Same method as test-bundle-verification.sh - `rauc info`,
# the signature and certificate check `rauc install` starts with, with the
# device's settings (check-purpose=codesign, and check-crl where it says so).
# Needs the Yocto build environment (rauc-native through oe-run-native).
#
#   test-key-rotation.sh <dev-signed.raucb> <release-1.raucb> <workdir>
#
# The release-2 bundle is made here, from the development-signed bundle,
# through the normal release step with the new key.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"
dev="$(realpath "$1")"; rel1="$(realpath "$2")"; work="$(realpath -m "$3")"
mkdir -p "$work"
u="$SIGNING_HOME/update"
rel2="$work/release-2.raucb"

RELEASE_CERT="$u/release-2.cert.pem" RELEASE_KEY_LABEL=update-release-2-p256 \
	"$here/sign-release-bundle.sh" "$dev" "$rel2" >/dev/null 2>&1 \
	|| { echo "re-signing with release-2 failed"; exit 1; }

signer() {  # CN of the certificate a bundle was signed with
	oe-run-native rauc-native rauc info -C keyring:check-purpose=codesign \
		--keyring="$u/update-ca.cert.pem" --dump-cert "$1" 2>/dev/null \
		| sed -n 's/.*Subject: .*CN *= *\(.*\)$/\1/p' | head -n 1
}
check() {  # check <bundle> <keyring> <check-crl> <description> <expected>
	local r verdict
	r="$(oe-run-native rauc-native rauc info -C keyring:check-purpose=codesign \
		-C "keyring:check-crl=$3" --keyring="$2" "$1" 2>&1 | grep -v 'Getting sysroot')"
	if grep -q '^Compatible:' <<<"$r"; then verdict=ACCEPTED; else verdict=REJECTED; fi
	printf '%-8s %-5s %s\n' "$verdict" "$([[ $verdict == "$5" ]] && echo ok || echo WRONG)" "$4"
	grep -iE 'revoked|expired|not yet valid|fail|error|invalid|warning' <<<"$r" | head -n 2 | sed 's/^/               /'
}

echo "release-1 bundle signed by: $(signer "$rel1")"
echo "release-2 bundle signed by: $(signer "$rel2")"
openssl x509 -noout -serial -subject -in "$u/release-1.cert.pem"
openssl x509 -noout -serial -subject -in "$u/release-2.cert.pem"
openssl crl -noout -text -in "$u/update-ca.crl.pem" | grep -E 'Last Update|Next Update|Serial Number|Revocation Date|keyCompromise' | sed 's/^ */  CRL: /'
echo
echo "-- rotation: the device's keyring is not touched (Update CA only)"
check "$rel1" "$u/update-ca.cert.pem" false "release-1 bundle, CA keyring"  ACCEPTED
check "$rel2" "$u/update-ca.cert.pem" false "release-2 bundle, CA keyring"  ACCEPTED
echo
echo "-- revocation: CA + CRL in the keyring, check-crl=true"
check "$rel1" "$u/keyring-with-crl.pem" true  "release-1 bundle (revoked key)"   REJECTED
check "$rel2" "$u/keyring-with-crl.pem" true  "release-2 bundle"                 ACCEPTED
echo
echo "-- revocation only works where the CRL is enforced"
check "$rel1" "$u/keyring-with-crl.pem" false "release-1, CRL in keyring but check-crl=false" ACCEPTED
check "$rel1" "$u/update-ca.cert.pem"   true  "release-1, check-crl=true but no CRL in keyring" REJECTED
echo
echo "-- an expired CRL on the device"
check "$rel2" "$u/keyring-with-expired-crl.pem" true "release-2 bundle, CRL expired 2026-09-01" REJECTED
