#!/bin/bash
# End-to-end test of device-monitor's mutual TLS: a throwaway PKI, a fake
# device-service control socket, and curl / openssl s_client as clients.
# Every way a client should be turned away is tried, not just the happy
# path.   Usage: bash test_mtls.sh path/to/device-monitor
set -uo pipefail
BIN=$(readlink -f "$1")
W=$(mktemp -d /tmp/devmon-test.XXXXXX)
trap 'kill $MON_PID $SOCK_PID 2>/dev/null; rm -rf "$W"' EXIT
cd "$W"
PORT=$(( 20000 + RANDOM % 20000 ))
FAILS=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILS=$((FAILS + 1)); }
q() { openssl "$@" >/dev/null 2>&1; }

# --- PKI -------------------------------------------------------------------
# Device CA -> this device's identity (server). Operator CA -> people and
# scrapers allowed to look (clients). A rogue CA nobody trusts.
ca() { # name
	q ecparam -name prime256v1 -genkey -noout -out $1.key
	q req -x509 -new -key $1.key -days 30 -subj "/CN=$1" -out $1.crt \
		-addext basicConstraints=critical,CA:true -addext keyUsage=critical,keyCertSign,cRLSign
}
leaf() { # name ca eku [san]
	q ecparam -name prime256v1 -genkey -noout -out $1.key
	q req -new -key $1.key -subj "/CN=$1" -out $1.csr
	printf 'basicConstraints=CA:false\nextendedKeyUsage=%s\n%s\n' "$3" "${4:-}" > $1.ext
	q x509 -req -in $1.csr -CA $2.crt -CAkey $2.key -CAcreateserial -days 30 -extfile $1.ext -out $1.crt
}
ca device-ca; ca operator-ca; ca rogue-ca
leaf device-0001 device-ca serverAuth "subjectAltName=DNS:localhost,IP:127.0.0.1"
leaf ops-alice operator-ca clientAuth
leaf ops-mallory operator-ca clientAuth      # revoked below
leaf ops-server-only operator-ca serverAuth  # wrong purpose
leaf intruder rogue-ca clientAuth

# A CRL from the operator CA revoking mallory (the same openssl ca flow as
# security/signing's release-key CRL, docs/security/update-and-provisioning.md §6).
mkdir -p ca-db && touch ca-db/index.txt && echo 01 > ca-db/crlnumber
cat > ca.cnf <<CNF
[ca]
default_ca = op
[op]
database = ca-db/index.txt
crlnumber = ca-db/crlnumber
certificate = operator-ca.crt
private_key = operator-ca.key
default_md = sha256
default_crl_days = 7
CNF
q ca -config ca.cnf -revoke ops-mallory.crt
q ca -config ca.cnf -gencrl -out operator.crl
openssl crl -in operator.crl -noout -text | grep -q "Serial Number" && pass "CRL revokes ops-mallory" || fail "CRL generation"

# --- a fake device-service --------------------------------------------------
# Answers status from state.txt, logs every command it receives.
echo Running > state.txt
cat > fake_ds.py <<'PY'
import json, os, socket, sys
path = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path); s.listen(4)
while True:
    c, _ = s.accept()
    cmd = c.recv(256).decode().strip()
    open("commands.log", "a").write(cmd + "\n")
    state = open("state.txt").read().strip()
    c.sendall((json.dumps({"ok": True, "state": state, "samples_read": 1000, "gaps": 0, "faults": 0,
                           "recoveries": 0, "recovery_attempts": 0, "in_state_s": 5,
                           "sample_age_ewma_us": 973}) + "\n").encode())
    c.close()
PY
python3 fake_ds.py "$W/control.sock" & SOCK_PID=$!

cat > monitor.json <<JSON
{"listen_address": "127.0.0.1", "listen_port": $PORT, "cert_file": "$W/device-0001.crt",
 "key_file": "$W/device-0001.key", "client_ca_file": "$W/operator-ca.crt", "crl_file": "$W/operator.crl",
 "control_socket": "$W/control.sock", "timeout_ms": 2000}
JSON
"$BIN" monitor.json 2> monitor.log & MON_PID=$!
for _ in $(seq 50); do grep -q listening monitor.log 2>/dev/null && break; sleep 0.1; done

get() { # client path -> body, exit status of curl
	curl -sS --max-time 5 --cacert device-ca.crt ${1:+--cert $1.crt --key $1.key} "https://localhost:$PORT$2"
}

# --- the allowed client ------------------------------------------------------
body=$(get ops-alice /status) && echo "$body" | grep -q '"state": "Running"' \
	&& pass "alice: GET /status returns device-service's status" || fail "alice /status: $body"
get ops-alice /metrics | grep -q 'device_state{state="Running"} 1' \
	&& pass "alice: GET /metrics is Prometheus format" || fail "alice /metrics"
m=$(get ops-alice /metrics)
crl_left=$(echo "$m" | sed -n 's/^device_monitor_crl_expiry_seconds //p')
cert_left=$(echo "$m" | sed -n 's/^device_monitor_certificate_expiry_seconds //p')
# The test CRL is valid for 7 days (default_crl_days), the certificate 30.
[ -n "$crl_left" ] && [ "$crl_left" -gt $((6 * 86400)) ] && [ "$crl_left" -le $((7 * 86400)) ] \
	&& pass "metrics: CRL expiry in ~7 days ($crl_left s), alertable before it fails closed" || fail "crl expiry metric: '$crl_left'"
[ -n "$cert_left" ] && [ "$cert_left" -gt $((29 * 86400)) ] && [ "$cert_left" -le $((30 * 86400)) ] \
	&& pass "metrics: device certificate expiry in ~30 days" || fail "cert expiry metric: '$cert_left'"
code=$(curl -s -o /dev/null -w '%{http_code}' --cacert device-ca.crt --cert ops-alice.crt --key ops-alice.key "https://localhost:$PORT/healthz")
[ "$code" = 200 ] && pass "healthz 200 while Running" || fail "healthz while Running: $code"
echo Fault > state.txt
code=$(curl -s -o /dev/null -w '%{http_code}' --cacert device-ca.crt --cert ops-alice.crt --key ops-alice.key "https://localhost:$PORT/healthz")
[ "$code" = 503 ] && pass "healthz 503 in Fault" || fail "healthz in Fault: $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --cacert device-ca.crt --cert ops-alice.crt --key ops-alice.key "https://localhost:$PORT/status")
[ "$code" = 405 ] && pass "POST refused (read-only)" || fail "POST: $code"

# --- clients that must be turned away ---------------------------------------
get "" /status >/dev/null 2>&1 && fail "no client certificate was accepted" || pass "no client certificate: refused"
get intruder /status >/dev/null 2>&1 && fail "rogue-CA certificate was accepted" || pass "certificate from an untrusted CA: refused"
get ops-mallory /status >/dev/null 2>&1 && fail "revoked certificate was accepted" || pass "revoked certificate: refused"
get ops-server-only /status >/dev/null 2>&1 && fail "serverAuth-only certificate was accepted" || pass "certificate without clientAuth: refused"
echo | openssl s_client -connect 127.0.0.1:$PORT -tls1_2 -cert ops-alice.crt -key ops-alice.key \
	-CAfile device-ca.crt >/dev/null 2>&1 && fail "TLS 1.2 was accepted" || pass "TLS 1.2: refused"
# The client side too: the device proves who it is.
curl -sS --max-time 5 --cacert rogue-ca.crt --cert ops-alice.crt --key ops-alice.key "https://localhost:$PORT/status" >/dev/null 2>&1 \
	&& fail "client accepted a device it should not trust" || pass "client rejects a device certificate from the wrong CA"

# --- what the device saw ------------------------------------------------------
sort -u commands.log | grep -qvx status && fail "monitor sent something other than status" \
	|| pass "only 'status' ever reached the control socket"
grep -q 'CN=ops-alice "GET /status HTTP/1.1" 200' monitor.log && pass "access log names the client" || fail "access log"
grep -q 'certificate revoked' monitor.log && pass "log says why mallory was refused (revoked)" || fail "revocation reason not logged"
grep -q 'unsuitable certificate purpose' monitor.log && pass "log says why the serverAuth cert was refused" || fail "purpose reason not logged"

kill $MON_PID; wait $MON_PID 2>/dev/null
grep -q stopped monitor.log && pass "SIGTERM: clean exit" || fail "no clean exit"

# An expired CRL fails closed: nobody gets in, not even alice. That is the
# operational cost of revocation - a fresh CRL has to reach the device
# before nextUpdate.
q ca -config ca.cnf -gencrl -crl_lastupdate 20260101000000Z -crl_nextupdate 20260201000000Z -out expired.crl
sed "s#$W/operator.crl#$W/expired.crl#" monitor.json > monitor-expired.json
"$BIN" monitor-expired.json 2> monitor-expired.log & MON_PID=$!
for _ in $(seq 50); do grep -q listening monitor-expired.log 2>/dev/null && break; sleep 0.1; done
get ops-alice /status >/dev/null 2>&1 && fail "expired CRL: alice still got in" || pass "expired CRL: every client refused (fails closed)"
grep -q 'CRL has expired' monitor-expired.log && pass "log says the CRL has expired" || fail "expired-CRL reason not logged"
kill $MON_PID; wait $MON_PID 2>/dev/null
echo "---"; [ $FAILS = 0 ] && echo "all passed" || { echo "$FAILS failed"; cat monitor.log; }
exit $FAILS
