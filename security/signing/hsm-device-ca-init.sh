#!/usr/bin/env bash
# One-time: a second key in the same token, for a different job - the
# manufacturer's Device CA, which certifies each device's identity key at
# provisioning (tools/provision-device.sh). Deliberately NOT the boot-signing
# key: a key that can sign boot images must not also be usable to mint
# device identities, and vice versa.
#
# ECDSA P-256, generated in the token, non-extractable. The CA certificate
# (self-signed, public) is written next to the public boot key.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"
: "${DEVICE_CA_KEY_LABEL:=device-ca-p256}"
: "${DEVICE_CA_KEY_ID:=02}"

if p11 --list-objects --type privkey 2>/dev/null | grep -q "label: *$DEVICE_CA_KEY_LABEL\$"; then
	echo "key '$DEVICE_CA_KEY_LABEL' already exists" >&2
else
	p11 --keypairgen --key-type EC:prime256v1 --id "$DEVICE_CA_KEY_ID" \
		--label "$DEVICE_CA_KEY_LABEL" --usage-sign --sensitive --private
fi

# Self-signed CA certificate, signed by the token through OpenSSL's PKCS#11
# engine. The private key is referenced by URI and never leaves the token.
uri="pkcs11:token=$HSM_TOKEN_LABEL;object=$DEVICE_CA_KEY_LABEL;type=private"
openssl_p11 req -new -x509 -days 3650 \
	-engine pkcs11 -keyform engine -key "$uri" -sha256 \
	-subj "/O=device-platform/CN=device-platform Device CA" \
	-addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
	-addext "keyUsage=critical,keyCertSign,cRLSign" \
	-out "$SIGNING_HOME/device-ca.crt"
openssl x509 -in "$SIGNING_HOME/device-ca.crt" -noout -subject -fingerprint -sha256
