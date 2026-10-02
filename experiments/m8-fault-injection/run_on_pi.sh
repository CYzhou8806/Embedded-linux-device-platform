#!/bin/bash
# Host side: push scenarios.sh to the board, run it, fetch the results and
# analyze them. Needs the M8 driver (custom_acq tracepoints) and the M5
# device-service (device-ctl) on the board, i.e. an image built from this
# tree, and an MCU that answers (device-ctl status says Running).
#
#   bash run_on_pi.sh [ssh-host] [scenario...]
set -euo pipefail
HOST=${1:-RaspberryPi5-prod}; shift || true
# The production image has no root login: admin + sudo. Empty SUDO for a
# board where the SSH user is root.
SUDO=${SUDO-sudo -n}
HERE=$(cd "$(dirname "$0")" && pwd)
STAMP=$(date +%Y%m%d-%H%M%S)
DEST=$HERE/../../results/m8-fault-injection/$STAMP

ssh "$HOST" "$SUDO /opt/device-service/device-ctl status" | grep -q '"state":"Running"' || {
	echo "device-service is not Running on $HOST - fix that first (MCU reset?)" >&2; exit 1; }
scp -q "$HERE/scenarios.sh" "$HOST:/tmp/m8-scenarios.sh"
ssh "$HOST" "$SUDO rm -rf /tmp/m8 && $SUDO sh /tmp/m8-scenarios.sh /tmp/m8 $*"
mkdir -p "$DEST"
ssh "$HOST" "$SUDO tar -C /tmp -cf - m8" | tar -C "$DEST" --strip-components=1 -xf -
python3 "$HERE/analyze.py" "$DEST" | tee "$DEST/report.md"
