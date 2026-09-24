#!/usr/bin/env bash
# Station-side tests for tools/provision-device.sh against the fake-devid
# test double (no board, no QEMU). Uses the real HSM token for the CA key,
# but a throwaway provisioning log. Each case prints PASS/FAIL.
set -uo pipefail
repo="$(cd "$(dirname "$0")/../../../.." && pwd)"
fake="$repo/security/optee-ta/device_identity/testing/fake-devid"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
export PROVISION_DIR="$tmp/out" DEVID="$fake" SERIAL_CMD="$fake serial"
fails=0
expect() {  # expect <0|1> <description> -- env...
	local want=$1 what=$2; shift 3
	env "$@" "$repo/tools/provision-device.sh" > "$tmp/last.txt" 2>&1; local rc=$?
	if { [[ $want == 0 && $rc == 0 ]] || [[ $want == 1 && $rc != 0 ]]; }; then
		echo "PASS  $what"
	else
		echo "FAIL  $what (rc=$rc)"; cat "$tmp/last.txt"; fails=$((fails + 1))
	fi
	sed 's/^/        /' "$tmp/last.txt" | grep -v "^        certificate:\|recorded in:"
}
expect 0 "new device is provisioned and certified"            -- FAKE_DEVID_DIR="$tmp/d1" FAKE_DEVID_SERIAL=fake0001
expect 1 "same serial a second time is refused (log)"         -- FAKE_DEVID_DIR="$tmp/d1" FAKE_DEVID_SERIAL=fake0001
expect 1 "device with an identity we never issued is refused" -- FAKE_DEVID_DIR="$tmp/d1" FAKE_DEVID_SERIAL=fake0002
expect 1 "device that can't prove possession is refused"      -- FAKE_DEVID_DIR="$tmp/d3" FAKE_DEVID_SERIAL=fake0003 FAKE_DEVID_CHEAT=1
if diff <(openssl x509 -in "$tmp/out/fake0001.crt" -noout -pubkey) \
	<(openssl ec -in "$tmp/d1/key.pem" -pubout 2>/dev/null) >/dev/null; then
	echo "PASS  certificate carries the device's key, not the throwaway CSR key"
else
	echo "FAIL  certificate public key"; fails=$((fails + 1))
fi
n="$(wc -l < "$tmp/out/provisioning-log.jsonl")"
[[ $n == 1 ]] && echo "PASS  exactly one log record" || { echo "FAIL  $n log records"; fails=$((fails + 1)); }
exit $fails
