#!/usr/bin/env bash
# Issue a fresh Operator CA CRL and put it on devices, before the old one's
# nextUpdate. Revocation fails closed (docs/security/remote-monitoring.md
# §4): a device whose CRL has expired refuses every client, so this has to
# run on a schedule shorter than CRL_DAYS (30). device-monitor exports
# device_monitor_crl_expiry_seconds to alert if it doesn't.
#
#   refresh-monitor-crl.sh SSH-HOST... [--check-with CERT KEY URL]
#
# After pushing, each device's /metrics is read back (with an operator
# certificate) to confirm the new expiry is what the device now enforces,
# not just what was copied.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../signing/hsm-env.sh"
hosts=()
while [[ $# -gt 0 && $1 != --check-with ]]; do hosts+=("$1"); shift; done
"$here/monitor-pki.sh" crl >/dev/null
crl="$SIGNING_HOME/operator/operator.crl"
echo "new CRL: $(openssl crl -in "$crl" -noout -nextupdate)"
for h in "${hosts[@]}"; do
	scp -q "$crl" "$h:/tmp/operator.crl"
	ssh "$h" 'sudo mv /tmp/operator.crl /data/monitor/operator.crl && sudo chown root:root /data/monitor/operator.crl && sudo chmod 600 /data/monitor/operator.crl && sudo systemctl restart device-monitor && systemctl is-active device-monitor'
done
if [[ ${1:-} == --check-with ]]; then
	cert=$2 key=$3 url=$4
	# The restart above takes a moment to listen again.
	for _ in $(seq 20); do curl -s -o /dev/null --max-time 2 --cacert "$SIGNING_HOME/device-ca.crt" \
		--cert "$cert" --key "$key" "$url/healthz" && break; sleep 0.5; done
	left=$(curl -sS --max-time 8 --cacert "$SIGNING_HOME/device-ca.crt" --cert "$cert" --key "$key" "$url/metrics" |
		sed -n 's/^device_monitor_crl_expiry_seconds //p')
	echo "device now reports CRL expiry in ${left}s ($((left / 86400)) days)"
fi
