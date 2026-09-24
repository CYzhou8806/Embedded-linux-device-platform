#!/usr/bin/env bash
# Provisioning station: give one device its identity, certify it, record it.
#
#   DEVICE_EXEC="ssh admin@192.168.178.173 sudo" tools/provision-device.sh
#
# The device makes its own key (device identity TA, docs/security/optee.md);
# the station never sees a private key. What the station does:
#
#   1. read the device's serial number, refuse if it was provisioned before
#   2. ask the device to generate its identity key -> public key only
#   3. proof of possession: send a fresh nonce, check the signature with
#      that public key - the device can't produce a CSR, because the TA
#      deliberately refuses to sign anything but identity challenges
#   4. issue a device certificate for that public key, signed by the Device
#      CA key in the HSM (security/signing/hsm-device-ca-init.sh)
#   5. append a record to the provisioning log: who, when, which station,
#      which device, which key, which certificate
#
# Environment:
#   DEVICE_EXEC   command prefix that runs a command on the device ("" = local)
#   DEVID         the devid client on the device            (default: devid)
#   SERIAL_CMD    how to read the serial number (default: the Pi's device tree)
#   PROVISION_DIR where certificates and the log go
#                 (default: $SIGNING_HOME/provisioned)
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
source "$repo/security/signing/hsm-env.sh"
verify="$repo/security/optee-ta/device_identity/verify.py"

DEVICE_EXEC="${DEVICE_EXEC:-}"
DEVID="${DEVID:-devid}"
SERIAL_CMD="${SERIAL_CMD:-$DEVICE_EXEC cat /proc/device-tree/serial-number}"
PROVISION_DIR="${PROVISION_DIR:-$SIGNING_HOME/provisioned}"
: "${DEVICE_CA_KEY_LABEL:=device-ca-p256}"
ca_cert="$SIGNING_HOME/device-ca.crt"
log="$PROVISION_DIR/provisioning-log.jsonl"

die() { echo "provision: $*" >&2; exit 1; }
on_device() { $DEVICE_EXEC $DEVID "$@"; }

[[ -f "$ca_cert" ]] || die "no Device CA certificate - run security/signing/hsm-device-ca-init.sh"
mkdir -p -m 700 "$PROVISION_DIR"
touch "$log"

# 1. Which device, and has it been here before?
serial="$($SERIAL_CMD | tr -d '\0\r\n ')"
[[ "$serial" =~ ^[0-9A-Za-z]{4,32}$ ]] || die "unusable serial number '$serial'"
if grep -q "\"serial\": \"$serial\"" "$log"; then
	die "device $serial is already in the provisioning log - refusing to issue a second identity"
fi
echo "device serial: $serial"

# 2. The device generates its key. If it already has one that isn't in our
#    log, that's a device we didn't provision: stop and let a human look.
pub="$(on_device provision)" || die "device refused to provision (existing identity not in our log?)"
[[ "$pub" =~ ^04[0-9a-f]{128}$ ]] || die "unexpected public key format"
pub_fp="$(printf '%s' "$pub" | xxd -r -p | sha256sum | cut -d' ' -f1)"
echo "device key:    sha256 $pub_fp"

# 3. Proof of possession, with a nonce only this station knows.
nonce="$(python3 "$verify" nonce)"
sig="$(on_device sign "$nonce")"
python3 "$verify" check "$pub" "$nonce" "$sig" >/dev/null \
	|| die "proof of possession FAILED: the device does not hold the key it reported"
echo "proof of possession: OK"

# 4. Certificate for the device's key. OpenSSL wants a request to build a
#    certificate from; the request is signed by a throwaway key and only
#    carries the subject - -force_pubkey puts the device's key in instead.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 - "$pub" "$work/device-pub.pem" <<'PY'
import sys
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), bytes.fromhex(sys.argv[1]))
open(sys.argv[2], "wb").write(key.public_bytes(serialization.Encoding.PEM,
                              serialization.PublicFormat.SubjectPublicKeyInfo))
PY
openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
	-keyout "$work/throwaway.key" -subj "/O=device-platform/CN=$serial" \
	-out "$work/device.csr" 2>/dev/null
cat > "$work/ext.cnf" <<EOF
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = clientAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
EOF
cert="$PROVISION_DIR/$serial.crt"
openssl_p11 x509 -req -in "$work/device.csr" -force_pubkey "$work/device-pub.pem" \
	-CA "$ca_cert" -engine pkcs11 -CAkeyform engine \
	-CAkey "pkcs11:token=$HSM_TOKEN_LABEL;object=$DEVICE_CA_KEY_LABEL;type=private" \
	-set_serial "0x$(openssl rand -hex 16)" -days 3650 -sha256 \
	-extfile "$work/ext.cnf" -out "$cert" 2>/dev/null \
	|| die "certificate signing failed"
openssl verify -CAfile "$ca_cert" "$cert" >/dev/null || die "issued certificate does not verify"
cert_serial="$(openssl x509 -in "$cert" -noout -serial | cut -d= -f2)"
echo "certificate:   $cert (serial $cert_serial)"

# 5. The record. One JSON object per line, append-only.
printf '{"time": "%s", "station": "%s", "operator": "%s", "serial": "%s", "device_key_sha256": "%s", "cert_serial": "%s", "cert_sha256": "%s", "ca_sha256": "%s"}\n' \
	"$(date -Is)" "$(hostname)" "$(id -un)" "$serial" "$pub_fp" "$cert_serial" \
	"$(openssl x509 -in "$cert" -outform DER | sha256sum | cut -d' ' -f1)" \
	"$(openssl x509 -in "$ca_cert" -outform DER | sha256sum | cut -d' ' -f1)" \
	>> "$log"
echo "recorded in:   $log"
