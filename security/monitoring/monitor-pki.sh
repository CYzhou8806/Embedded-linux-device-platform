#!/usr/bin/env bash
# Certificates for device-monitor's mutual TLS (docs/security/remote-monitoring.md),
# issued by keys in the HSM token, like every other key in this project.
#
#   monitor-pki.sh init-operator-ca          key 06 + Operator CA cert + empty CRL
#   monitor-pki.sh issue-device CSR NAME OUT [IP...]
#                                            device TLS cert from the Device CA (key 02);
#                                            DNS:NAME plus any IP SANs clients connect by
#   monitor-pki.sh issue-operator NAME DIR   operator key + client cert from the Operator CA
#   monitor-pki.sh revoke-operator CERT      revoke, and reissue the CRL
#   monitor-pki.sh crl                       reissue the CRL (it expires: default_crl_days)
#
# Two CAs, one job each, the same separation as the boot, update and
# device-identity keys: the Device CA vouches for devices, the Operator CA
# for whoever may look at them. A leaked operator credential is revoked by
# the Operator CA's CRL without touching any device certificate, and the
# Operator CA key can't mint a device identity.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../signing/hsm-env.sh"
: "${DEVICE_CA_KEY_LABEL:=device-ca-p256}"
: "${OPERATOR_CA_KEY_LABEL:=operator-ca-p256}"
: "${OPERATOR_CA_KEY_ID:=06}"
: "${CRL_DAYS:=30}"
op="$SIGNING_HOME/operator"
uri() { echo "pkcs11:token=$HSM_TOKEN_LABEL;object=$1;type=private"; }
serial() { openssl x509 -noout -serial -in "$1" | cut -d= -f2; }

ca_cnf() {
	cat > "$op/ca.cnf" <<CNF
[ca]
default_ca = operator_ca
[operator_ca]
database         = $op/index.txt
crlnumber        = $op/crlnumber
certificate      = $op/operator-ca.crt
default_md       = sha256
default_crl_days = $CRL_DAYS
crl_extensions   = crl_ext
[crl_ext]
authorityKeyIdentifier = keyid:always
CNF
}

gencrl() {
	openssl_p11 ca -config "$op/ca.cnf" -engine pkcs11 -keyform engine \
		-keyfile "$(uri "$OPERATOR_CA_KEY_LABEL")" -gencrl -out "$op/operator.crl" 2>/dev/null
	openssl crl -in "$op/operator.crl" -CAfile "$op/operator-ca.crt" -noout 2>&1
	echo "CRL: $op/operator.crl (next update in $CRL_DAYS days - ship a fresh one before then)"
}

case "${1:-}" in
init-operator-ca)
	install -d -m 700 "$op"
	if ! p11 --list-objects --type privkey 2>/dev/null | grep -q "label: *$OPERATOR_CA_KEY_LABEL\$"; then
		p11 --keypairgen --key-type EC:prime256v1 --id "$OPERATOR_CA_KEY_ID" \
			--label "$OPERATOR_CA_KEY_LABEL" --usage-sign --sensitive --private >/dev/null
	fi
	openssl_p11 req -new -x509 -days 3650 -engine pkcs11 -keyform engine \
		-key "$(uri "$OPERATOR_CA_KEY_LABEL")" -sha256 \
		-subj "/O=device-platform/CN=device-platform Operator CA" \
		-addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
		-addext "keyUsage=critical,keyCertSign,cRLSign" \
		-out "$op/operator-ca.crt" 2>/dev/null
	[[ -f "$op/index.txt" ]] || { : > "$op/index.txt"; echo 1000 > "$op/crlnumber"; }
	ca_cnf
	gencrl
	;;
issue-device)
	csr=$2 name=$3 out=$4
	san="DNS:$name"
	for ip in "${@:5}"; do san="$san,IP:$ip"; done
	# The key stays on the device; only its CSR came here. Proof of
	# possession is the CSR's own signature, checked first.
	openssl req -in "$csr" -verify -noout 2>/dev/null || { echo "CSR signature invalid" >&2; exit 1; }
	ext=$(mktemp); trap 'rm -f "$ext"' EXIT
	printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=%s\n' \
		"$san" > "$ext"
	openssl_p11 x509 -req -in "$csr" -CA "$SIGNING_HOME/device-ca.crt" \
		-engine pkcs11 -CAkeyform engine -CAkey "$(uri "$DEVICE_CA_KEY_LABEL")" \
		-set_serial "0x$(openssl rand -hex 16)" -days 730 -sha256 \
		-subj "/O=device-platform/CN=$name" -extfile "$ext" -out "$out" 2>/dev/null
	openssl verify -CAfile "$SIGNING_HOME/device-ca.crt" "$out"
	echo "$(date -u +%FT%TZ) monitor TLS cert for $name serial $(serial "$out")" >> "$SIGNING_HOME/signing-audit.log"
	;;
issue-operator)
	name=$2 dir=$3
	install -d -m 700 "$dir"
	# An operator's key is made where the operator is (here, for the
	# workstation that will use it), never on a device.
	(umask 077; openssl ecparam -name prime256v1 -genkey -noout -out "$dir/$name.key")
	openssl req -new -key "$dir/$name.key" -subj "/O=device-platform/CN=$name" -out "$dir/$name.csr"
	ext=$(mktemp); trap 'rm -f "$ext"' EXIT
	printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n' > "$ext"
	openssl_p11 x509 -req -in "$dir/$name.csr" -CA "$op/operator-ca.crt" \
		-engine pkcs11 -CAkeyform engine -CAkey "$(uri "$OPERATOR_CA_KEY_LABEL")" \
		-set_serial "0x$(openssl rand -hex 16)" -days 365 -sha256 -extfile "$ext" \
		-out "$dir/$name.crt" 2>/dev/null
	openssl_p11 ca -config "$op/ca.cnf" -engine pkcs11 -keyform engine \
		-keyfile "$(uri "$OPERATOR_CA_KEY_LABEL")" -valid "$dir/$name.crt" 2>&1 | grep -v '^Using configuration' || true
	openssl verify -CAfile "$op/operator-ca.crt" "$dir/$name.crt"
	echo "$(date -u +%FT%TZ) operator cert for $name serial $(serial "$dir/$name.crt")" >> "$SIGNING_HOME/signing-audit.log"
	;;
revoke-operator)
	openssl_p11 ca -config "$op/ca.cnf" -engine pkcs11 -keyform engine \
		-keyfile "$(uri "$OPERATOR_CA_KEY_LABEL")" -revoke "$2" -crl_reason keyCompromise 2>&1 \
		| grep -v '^Using configuration' || true
	echo "$(date -u +%FT%TZ) revoked operator cert serial $(serial "$2")" >> "$SIGNING_HOME/signing-audit.log"
	gencrl
	;;
crl)
	gencrl
	;;
*)
	sed -n '2,16p' "$0"; exit 2 ;;
esac
