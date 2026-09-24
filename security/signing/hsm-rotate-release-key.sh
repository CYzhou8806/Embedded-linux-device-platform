#!/usr/bin/env bash
# Rotate the release-signing key, and revoke the old one.
#
#   Update CA (id 03) ─┬─ release signing 1 (id 04)   revoked by the CRL below
#                      └─ release signing 2 (id 05)   new, issued here
#
# Devices trust the Update CA, not a signing key, so bundles signed with
# release-2 are accepted by every device without touching its keyring.
# Revocation is the opposite: a device only learns that release-1 is
# revoked when the CRL is in its keyring and `check-crl=true` is set - i.e.
# the CRL has to reach the device, normally inside an update.
#
# The CRL is issued by the CA key in the token through `openssl ca`, which
# needs a small CA database (index.txt) that records what the CA issued and
# revoked. It is kept next to the certificates in $SIGNING_HOME/update/ca/.
#
#   hsm-rotate-release-key.sh             new key + release-2 certificate + CRL
#   CRL_DAYS=30 hsm-rotate-release-key.sh CRL validity (default 30 days)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"
out="$SIGNING_HOME/update"
ca="$out/ca"
: "${CRL_DAYS:=30}"
uri() { echo "pkcs11:token=$HSM_TOKEN_LABEL;object=$1;type=private"; }

# 1. New release key in the token (non-extractable, like id 04).
if p11 --list-objects --type privkey 2>/dev/null | grep -q "label: *update-release-2-p256\$"; then
	echo "key 'update-release-2-p256' already exists"
else
	p11 --keypairgen --key-type EC:prime256v1 --id 05 --label update-release-2-p256 \
		--usage-sign --sensitive --private >/dev/null
fi

# 2. Its certificate, issued by the Update CA - same profile as release-1.
if [[ ! -f "$out/release-2.cert.pem" ]]; then
	openssl_p11 req -new -engine pkcs11 -keyform engine -key "$(uri update-release-2-p256)" \
		-subj "/O=device-platform/CN=device-platform release signing 2" \
		-out "$out/release-2.csr" 2>/dev/null
	openssl_p11 x509 -req -in "$out/release-2.csr" -CA "$out/update-ca.cert.pem" \
		-engine pkcs11 -CAkeyform engine -CAkey "$(uri update-ca-p256)" \
		-set_serial "0x$(openssl rand -hex 16)" -days 730 -sha256 \
		-extfile "$out/release.ext" -out "$out/release-2.cert.pem" 2>/dev/null
fi
openssl verify -CAfile "$out/update-ca.cert.pem" "$out/release-2.cert.pem"

# 3. CA database: both issued certificates, then release-1 revoked.
mkdir -p -m 700 "$ca"
if [[ ! -f "$ca/index.txt" ]]; then
	: > "$ca/index.txt"
	echo 1000 > "$ca/crlnumber"
fi
cat > "$ca/ca.cnf" <<CNF
[ca]
default_ca = update_ca
[update_ca]
database        = $ca/index.txt
crlnumber       = $ca/crlnumber
certificate     = $out/update-ca.cert.pem
default_md      = sha256
default_crl_days = $CRL_DAYS
crl_extensions  = crl_ext
[crl_ext]
authorityKeyIdentifier = keyid:always
CNF
# openssl ca -revoke both adds a certificate it never issued through this
# database and marks it revoked; -valid adds one as valid.
serial() { openssl x509 -noout -serial -in "$1" | cut -d= -f2; }
if ! grep -q "$(serial "$out/release-1.cert.pem")" "$ca/index.txt"; then
	openssl_p11 ca -config "$ca/ca.cnf" -engine pkcs11 -keyform engine \
		-keyfile "$(uri update-ca-p256)" -revoke "$out/release-1.cert.pem" \
		-crl_reason keyCompromise 2>&1 | grep -v '^Using configuration'
fi
if ! grep -q "$(serial "$out/release-2.cert.pem")" "$ca/index.txt"; then
	openssl_p11 ca -config "$ca/ca.cnf" -engine pkcs11 -keyform engine \
		-keyfile "$(uri update-ca-p256)" -valid "$out/release-2.cert.pem" 2>&1 \
		| grep -v '^Using configuration'
fi

# 4. The CRL, signed by the CA key in the token.
openssl_p11 ca -config "$ca/ca.cnf" -engine pkcs11 -keyform engine \
	-keyfile "$(uri update-ca-p256)" -gencrl -out "$out/update-ca.crl.pem" 2>/dev/null
openssl crl -in "$out/update-ca.crl.pem" -CAfile "$out/update-ca.cert.pem" -noout 2>&1

# 4b. For the test only: the same CRL, but already expired - what a device
#     holds when nobody shipped a fresh CRL in time (RAUC's docs warn that
#     this blocks all further updates; test-key-rotation.sh checks it).
openssl_p11 ca -config "$ca/ca.cnf" -engine pkcs11 -keyform engine \
	-keyfile "$(uri update-ca-p256)" -gencrl \
	-crl_lastupdate 20260801000000Z -crl_nextupdate 20260901000000Z \
	-out "$out/update-ca.crl-expired.pem" 2>/dev/null
cat "$out/update-ca.cert.pem" "$out/update-ca.crl-expired.pem" > "$out/keyring-with-expired-crl.pem"

# 5. The keyring a device needs to enforce the revocation: CA + CRL in one file.
cat "$out/update-ca.cert.pem" "$out/update-ca.crl.pem" > "$out/keyring-with-crl.pem"
printf '%s %s rotate: release-2 issued (%s), release-1 revoked (%s), CRL valid %s days\n' \
	"$(date -Is)" "$(id -un)" "$(serial "$out/release-2.cert.pem")" \
	"$(serial "$out/release-1.cert.pem")" "$CRL_DAYS" >> "$SIGNING_HOME/signing-audit.log"
