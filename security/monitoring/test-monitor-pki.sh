#!/usr/bin/env bash
# The whole HSM-backed chain on a scratch SoftHSM token - never the real one
# in ~/.device-platform-signing: create the token, the Device CA (02) and the
# Operator CA (06), issue a device certificate from a CSR and two operator
# certificates, revoke one, then run device-monitor with exactly those files
# and connect as each operator.
#
#   test-monitor-pki.sh path/to/device-monitor
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin=$(readlink -f "$1")
export SIGNING_HOME=$(mktemp -d /tmp/monitor-pki-test.XXXXXX)
unset SOFTHSM2_CONF HSM_PIN_FILE
trap 'kill $MON $SOCK 2>/dev/null; rm -rf "$SIGNING_HOME"' EXIT
fails=0
ok() { echo "PASS  $1"; }
no() { echo "FAIL  $1"; fails=$((fails + 1)); }

"$here/../signing/hsm-init.sh" >/dev/null 2>&1 || { echo "token init failed"; exit 1; }
"$here/../signing/hsm-device-ca-init.sh" >/dev/null 2>&1 || { echo "device CA failed"; exit 1; }
"$here/monitor-pki.sh" init-operator-ca >/dev/null 2>&1 || { echo "operator CA failed"; exit 1; }
source "$here/../signing/hsm-env.sh"
p11 --list-objects --type privkey 2>/dev/null | grep -q 'label: *operator-ca-p256$' && ok "Operator CA key is in the token" || no "operator CA key"
p11 --list-objects --type privkey 2>/dev/null | grep -A4 'operator-ca-p256' | grep -q 'never extractable' \
	&& ok "Operator CA key is never extractable" || no "operator CA key extractable?"

w=$SIGNING_HOME/w; mkdir -p "$w"; cd "$w"
openssl ecparam -name prime256v1 -genkey -noout -out device.key 2>/dev/null
openssl req -new -key device.key -subj /CN=localhost -out device.csr 2>/dev/null
"$here/monitor-pki.sh" issue-device device.csr localhost device.crt >/dev/null 2>&1 && ok "device certificate from the Device CA" || no "issue-device"
openssl x509 -in device.crt -noout -ext extendedKeyUsage 2>/dev/null | grep -q 'TLS Web Server' && ok "device cert is serverAuth" || no "device EKU"
# A CSR whose signature doesn't match its key (no proof of possession) is refused.
sed 's/^\(.\{20\}\)./\1A/' device.csr > forged.csr
"$here/monitor-pki.sh" issue-device forged.csr localhost forged.crt >/dev/null 2>&1 && no "forged CSR was signed" || ok "CSR without proof of possession: refused"

"$here/monitor-pki.sh" issue-operator alice ops >/dev/null 2>&1 && ok "operator alice issued" || no "alice"
"$here/monitor-pki.sh" issue-operator bob ops >/dev/null 2>&1 && ok "operator bob issued" || no "bob"
"$here/monitor-pki.sh" revoke-operator ops/bob.crt >/dev/null 2>&1 && ok "bob revoked, CRL reissued" || no "revoke"
grep -q 'operator cert for alice' "$SIGNING_HOME/signing-audit.log" && ok "issuance is in the audit log" || no "audit log"

python3 - "$w/control.sock" <<'PY' & SOCK=$!
import socket, sys
s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(4)
while True:
    c, _ = s.accept(); c.recv(64); c.sendall(b'{"ok":true,"state":"Running"}\n'); c.close()
PY
port=$(( 20000 + RANDOM % 20000 ))
cat > m.json <<JSON
{"listen_address":"127.0.0.1","listen_port":$port,"cert_file":"$w/device.crt","key_file":"$w/device.key",
 "client_ca_file":"$SIGNING_HOME/operator/operator-ca.crt","crl_file":"$SIGNING_HOME/operator/operator.crl",
 "control_socket":"$w/control.sock"}
JSON
"$bin" m.json 2> m.log & MON=$!
for _ in $(seq 50); do grep -q listening m.log 2>/dev/null && break; sleep 0.1; done
c() { curl -sS --max-time 5 --cacert "$SIGNING_HOME/device-ca.crt" --cert ops/$1.crt --key ops/$1.key "https://localhost:$port/status"; }
c alice 2>/dev/null | grep -q Running && ok "alice (Operator CA) reads status from a Device-CA-certified device" || no "alice: $(c alice 2>&1)"
c bob >/dev/null 2>&1 && no "revoked bob got in" || ok "bob (revoked through the HSM-signed CRL): refused"
echo "---"; [ $fails = 0 ] && echo "all passed" || { echo "$fails failed"; cat m.log; }
exit $fails
