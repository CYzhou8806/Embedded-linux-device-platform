#!/usr/bin/env bash
# Run qemu-demo.exp against an OP-TEE qemu_v8 build, then play the verifier:
# make the nonce here, check the device's answer here, with only the public
# key the device printed at provisioning.
#
#   OPTEE_ROOT=~/optee ./run-demo.sh [OUTDIR]
#
# OPTEE_ROOT is a checkout of OP-TEE's manifest (qemu_v8.xml) with
# optee_examples/device_identity -> this directory, built with `make all`.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
OPTEE_ROOT="${OPTEE_ROOT:-$HOME/optee}"
out="$(realpath -m "${1:-$here/demo-output}")"
mkdir -p "$out"

# Own directory, so QEMU's serial log can't collide with a `make check` run.
for f in "$OPTEE_ROOT"/out/bin/*; do ln -sf "$f" "$out/"; done
ln -sf "$OPTEE_ROOT/out-br/images/rootfs.cpio.gz" "$out/"

# Same QEMU command line OP-TEE's own `make check` uses.
eval "$(make -s -n -C "$OPTEE_ROOT/build" check-only 2>/dev/null \
	| grep -o 'export QEMU=[^&]*\|export QEMU_CHECK_ARGS="[^"]*"' \
	| sed 's/QEMU_CHECK_ARGS/QEMU_ARGS/; s/file:serial1.log/file:secure-world.log/')"
export QEMU QEMU_ARGS

NONCE="$(python3 "$here/verify.py" nonce)"
export NONCE
(cd "$out" && expect "$here/qemu-demo.exp") | tr -d '\r' > "$out/demo.txt"

grab() { grep -o "^@@$1=[0-9a-f]*" "$out/normal-world.log" | head -1 | cut -d= -f2; }
PUB="$(grab PUB)"; SIG="$(grab SIG)"
{
	echo "nonce:     $NONCE"
	echo "pubkey:    $PUB"
	echo "signature: $SIG"
	echo -n "verify (correct nonce): "; python3 "$here/verify.py" check "$PUB" "$NONCE" "$SIG" || true
	echo -n "verify (other nonce):   "; python3 "$here/verify.py" check "$PUB" "$(python3 "$here/verify.py" nonce)" "$SIG" || true
	echo -n "same key after TA panic: "; [[ "$PUB" == "$(grab PUB2)" ]] && echo yes || echo NO
	echo -n "re-provisioned after rollback: "; P3="$(grab PUB3)"
	if [[ -z "$P3" ]]; then echo "no (rollback detected or failed)"
	elif [[ "$P3" == "$PUB" ]]; then echo "same key (rollback not effective)"
	else echo "yes - a DIFFERENT identity: rollback accepted"; fi
} | tee "$out/verifier.txt"
