#!/usr/bin/env bash
# One-time: the update-signing hierarchy for RAUC bundles, in the same token.
#
#   Update CA (id 03)            self-signed; its certificate is the keyring
#     │                          installed on every device
#     └─ release signing (id 04) certificate issued by the Update CA; signs
#                                bundles (rauc resign)
#
# Two levels instead of one key so the signing key can be replaced - rotated
# on schedule or revoked after a leak - by issuing a new certificate, without
# touching the keyring on devices already in the field. The CA key is only
# used here and to issue that replacement.
#
# Both keys are ECDSA P-256, generated in the token, non-extractable. They
# are separate from the boot-signing key (id 01) and the Device CA (id 02).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"
out="$SIGNING_HOME/update"
mkdir -p -m 700 "$out"

gen() {  # gen <id> <label>
	if p11 --list-objects --type privkey 2>/dev/null | grep -q "label: *$2\$"; then
		echo "key '$2' already exists"
	else
		p11 --keypairgen --key-type EC:prime256v1 --id "$1" --label "$2" \
			--usage-sign --sensitive --private >/dev/null
	fi
}
gen 03 update-ca-p256
gen 04 update-release-p256
uri() { echo "pkcs11:token=$HSM_TOKEN_LABEL;object=$1;type=private"; }

openssl_p11 req -new -x509 -days 3650 -engine pkcs11 -keyform engine \
	-key "$(uri update-ca-p256)" -sha256 \
	-subj "/O=device-platform/CN=device-platform Update CA" \
	-addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
	-addext "keyUsage=critical,keyCertSign,cRLSign" \
	-out "$out/update-ca.cert.pem" 2>/dev/null

# The signing certificate's request is made by the token too (-key is the
# release key), then signed by the CA key.
openssl_p11 req -new -engine pkcs11 -keyform engine -key "$(uri update-release-p256)" \
	-subj "/O=device-platform/CN=device-platform release signing 1" \
	-out "$out/release-1.csr" 2>/dev/null
cat > "$out/release.ext" <<EXT
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = codeSigning
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
EXT
openssl_p11 x509 -req -in "$out/release-1.csr" -CA "$out/update-ca.cert.pem" \
	-engine pkcs11 -CAkeyform engine -CAkey "$(uri update-ca-p256)" \
	-set_serial "0x$(openssl rand -hex 16)" -days 730 -sha256 \
	-extfile "$out/release.ext" -out "$out/release-1.cert.pem" 2>/dev/null
openssl verify -CAfile "$out/update-ca.cert.pem" "$out/release-1.cert.pem"
