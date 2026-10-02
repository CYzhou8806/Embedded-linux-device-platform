#!/usr/bin/env bash
# Put device-monitor's credentials on a board (A/B image) and start it.
# The device's TLS key is generated ON the device, into the encrypted /data
# partition, and never leaves it: only the CSR comes back to be signed.
#
#   provision-monitor.sh SSH-HOST DEVICE-NAME [IP...]
#
# IPs become subjectAltNames, for clients that connect by address. Needs
# the Device CA and the Operator CA in the token (hsm-device-ca-init.sh,
# monitor-pki.sh init-operator-ca). The production image has no root
# login: everything on the board goes through sudo, and only BusyBox
# applets the image has (no `install` - see case 14).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../signing/hsm-env.sh"
host=$1 name=$2; shift 2
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

ssh "$host" "set -e; mountpoint -q /data
	sudo mkdir -p /data/monitor && sudo chmod 700 /data/monitor
	sudo test -f /data/monitor/device.key || sudo sh -c 'umask 077; openssl ecparam -name prime256v1 -genkey -noout -out /data/monitor/device.key'
	sudo openssl req -new -key /data/monitor/device.key -subj /CN=$name" > "$work/device.csr"
"$here/monitor-pki.sh" issue-device "$work/device.csr" "$name" "$work/device.crt" "$@"
scp -q "$work/device.crt" "$SIGNING_HOME/operator/operator-ca.crt" "$SIGNING_HOME/operator/operator.crl" "$host:/tmp/"
# Moved into place as root, and the /data/monitor/* glob is expanded by a
# root shell: admin cannot list the 0700 directory, so an admin-side glob
# would reach chown unexpanded.
ssh "$host" 'set -e; for f in device.crt operator-ca.crt operator.crl; do sudo mv /tmp/$f /data/monitor/$f; done
	sudo sh -c "chown root:root /data/monitor/*; chmod 600 /data/monitor/*"
	sudo systemctl restart device-monitor; sleep 1; systemctl is-active device-monitor'
echo "clients connect with an Operator CA certificate and trust $SIGNING_HOME/device-ca.crt:"
echo "  curl --cacert device-ca.crt --cert ops.crt --key ops.key https://$name:8443/status"
