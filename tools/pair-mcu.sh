#!/usr/bin/env bash
# Pair the acquisition MCU with this system: derive its device-authentication
# key from its unique ID and write it into the MCU's last flash page over SWD
# (docs/security/device-authentication.md §3).
#
#   tools/pair-mcu.sh            # read UID, derive key, write, verify, log
#
#   K = HMAC-SHA256(master, "acq-pairing-v1" || UID)   (UID = 12 bytes, as stored)
#
# One key per MCU: extracting K from one board helps with no other board.
# STAND-IN: in the design, "master" is the Raspberry Pi's OTP device key used
# through the firmware HMAC service (rpi-fw-crypto), which never releases it.
# Writing that OTP key is irreversible and is not done on this project's only
# board, so master is a random 32-byte file here (0600), and the verifier that
# checks the MCU's answers runs where that file is.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
source "$repo/security/signing/hsm-env.sh"
dir="$SIGNING_HOME/mcu"
master="$dir/pairing-master.key"
log="$dir/pairing-log.jsonl"
key_addr=0x0807F800
ocd=(openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg)

mkdir -p -m 700 "$dir"
[[ -f "$master" ]] || (umask 077; head -c 32 /dev/urandom > "$master")

uid=$(timeout 30 "${ocd[@]}" -c init -c "mdb 0x1FFFF7E8 12" -c shutdown 2>&1 \
	| awk '/^0x1ffff7e8:/ {for (i = 2; i <= 13; i++) printf "%s", $i}')
[[ "$uid" =~ ^[0-9a-f]{24}$ ]] || { echo "could not read the MCU unique ID" >&2; exit 1; }
echo "MCU UID: $uid"

keyfile="$dir/$uid.key"
(umask 077; python3 - "$master" "$uid" "$keyfile" <<'PY'
import hashlib, hmac, sys
master = open(sys.argv[1], "rb").read()
k = hmac.new(master, b"acq-pairing-v1" + bytes.fromhex(sys.argv[2]), hashlib.sha256).digest()
open(sys.argv[3], "wb").write(k)
PY
)
echo "key: $keyfile (sha256 $(sha256sum "$keyfile" | cut -c1-16)...)"

# Only the key page is erased and written; the firmware is untouched.
timeout 60 "${ocd[@]}" -c init -c "reset halt" \
	-c "flash write_image erase $keyfile $key_addr bin" \
	-c "verify_image $keyfile $key_addr bin" \
	-c "reset run" -c shutdown 2>&1 | grep -E "wrote|verified|Error" || true

printf '{"time": "%s", "operator": "%s", "mcu_uid": "%s", "key_sha256": "%s", "master": "stand-in file"}\n' \
	"$(date -Is)" "$(id -un)" "$uid" "$(sha256sum "$keyfile" | cut -d' ' -f1)" >> "$log"
echo "logged in $log"
