#!/usr/bin/env bash
# Which bundles does a device accept? Runs `rauc info` - the same signature
# check `rauc install` starts with - for each combination of bundle and
# keyring, plus two tampered copies. Needs the Yocto build environment
# (rauc-native through oe-run-native).
#
#   test-bundle-verification.sh <dev-signed.raucb> <release.raucb> <workdir>
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"
dev="$(realpath "$1")"; rel="$(realpath "$2")"; work="$(realpath -m "$3")"
mkdir -p "$work"
prod_keyring="$SIGNING_HOME/update/update-ca.cert.pem"
dev_keyring="${RAUC_DEV_KEYRING:-/opt/yocto/local-config/rauc-dev/dev-ca.cert.pem}"

size="$(stat -c %s "$rel")"
cp "$rel" "$work/tampered-payload.raucb"
printf '\xff' | dd of="$work/tampered-payload.raucb" bs=1 seek=$((size / 2)) conv=notrunc status=none
# A verity bundle ends with the CMS signature and an 8-byte length; 300
# bytes from the end is inside the signature/certificate data.
cp "$rel" "$work/tampered-signature.raucb"
printf '\xff' | dd of="$work/tampered-signature.raucb" bs=1 seek=$((size - 300)) conv=notrunc status=none

check() {  # check <bundle> <keyring> <description> <expected ACCEPTED|REJECTED>
	local r verdict
	r="$(oe-run-native rauc-native rauc info -C keyring:check-purpose=codesign \
		--keyring="$2" "$1" 2>&1 | grep -v 'Getting sysroot')"
	if grep -q '^Compatible:' <<<"$r"; then verdict=ACCEPTED; else verdict=REJECTED; fi
	printf '%-8s %-8s %s\n' "$verdict" "$([[ $verdict == "$4" ]] && echo ok || echo WRONG)" "$3"
	[[ $verdict == REJECTED ]] && grep -iE 'fail|error|invalid' <<<"$r" | head -1 | sed 's/^/                  /'
}
check "$rel" "$prod_keyring" "release bundle, production keyring"               ACCEPTED
check "$dev" "$prod_keyring" "development bundle, production keyring"           REJECTED
check "$rel" "$dev_keyring"  "release bundle, development keyring"              REJECTED
check "$work/tampered-payload.raucb"   "$prod_keyring" "release bundle, 1 byte changed in the payload (signature only - see below)" ACCEPTED
check "$work/tampered-signature.raucb" "$prod_keyring" "release bundle, 1 byte changed in the signature" REJECTED

# `rauc info` accepts the bundle with a modified payload - correctly: in the
# verity format the signature covers the manifest, which carries the root
# hash of a dm-verity tree over the payload, and the payload itself is
# checked block by block by the kernel when `rauc install` mounts it. The
# same check, done here in userspace:
#   bundle = payload | hash tree (Verity Size) | CMS signature | u64 length
payload_check() {  # payload_check <bundle> <description>
	local info rh salt vs total siglen plen out
	info="$(oe-run-native rauc-native rauc info -C keyring:check-purpose=codesign \
		--keyring="$prod_keyring" "$1" 2>/dev/null)"
	rh="$(awk -F"'" '/Verity Hash/{print $2}' <<<"$info")"
	salt="$(awk -F"'" '/Verity Salt/{print $2}' <<<"$info")"
	vs="$(awk '/Verity Size/{print $3}' <<<"$info")"
	total="$(stat -c %s "$1")"
	siglen="$(tail -c 8 "$1" | od -An -t u8 --endian=big | tr -d ' ')"
	plen=$((total - 8 - siglen - vs))
	if out="$(veritysetup verify --no-superblock --format=1 --hash=sha256 \
		--data-block-size=4096 --hash-block-size=4096 --salt="$salt" \
		--hash-offset="$plen" --data-blocks=$((plen / 4096)) "$1" "$1" "$rh" 2>&1)"; then
		printf 'payload  OK       %s\n' "$2"
	else
		printf 'payload  FAILED   %s\n                  %s\n' "$2" "$(head -1 <<<"$out")"
	fi
}
echo "-- install-time payload check (dm-verity against the root hash in the signed manifest)"
payload_check "$rel" "release bundle"
payload_check "$work/tampered-payload.raucb" "release bundle, 1 byte changed in the payload"
