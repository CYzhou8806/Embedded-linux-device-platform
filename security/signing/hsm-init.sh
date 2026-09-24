#!/usr/bin/env bash
# One-time: create a SoftHSM2 token and generate the Raspberry Pi 5 secure-boot
# signing key *inside* it. The private key is created with CKA_SENSITIVE and
# without CKA_EXTRACTABLE, so no PKCS#11 call can return it - signing is the
# only thing it can be used for. Only the public key comes out, as PEM.
#
# BCM2712 secure boot only supports RSA-2048 + SHA-256 (PKCS#1 v1.5).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"

install -d -m 700 "$SIGNING_HOME" "$SIGNING_HOME/tokens"
if [[ ! -f "$SOFTHSM2_CONF" ]]; then
	cat > "$SOFTHSM2_CONF" <<CONF
directories.tokendir = $SIGNING_HOME/tokens
objectstore.backend = file
log.level = ERROR
CONF
fi

if pkcs11-tool --module "$PKCS11_MODULE" --list-token-slots 2>/dev/null | grep -q "token label *: *$HSM_TOKEN_LABEL\$"; then
	echo "token '$HSM_TOKEN_LABEL' already exists - refusing to re-create it" >&2
	exit 1
fi

umask 077
# Random PINs, written once to files only this user can read. A real HSM
# would use smartcards / quorum here instead of files.
[[ -f "$HSM_PIN_FILE" ]] || openssl rand -hex 16 > "$HSM_PIN_FILE"
so_pin_file="$SIGNING_HOME/so.pin"
[[ -f "$so_pin_file" ]] || openssl rand -hex 16 > "$so_pin_file"

# softhsm2-util has no way to take the PINs other than argv or a prompt;
# this runs once, on the signing host, before any other user could be
# watching for it. Everything after this uses p11() (hsm-env.sh).
softhsm2-util --init-token --free --label "$HSM_TOKEN_LABEL" \
	--so-pin "$(cat "$so_pin_file")" --pin "$(cat "$HSM_PIN_FILE")"

p11 --keypairgen --key-type rsa:2048 --id "$HSM_KEY_ID" --label "$HSM_KEY_LABEL" \
	--usage-sign --sensitive --private

# --private above also makes the public-key object login-only.
p11 --read-object --type pubkey --id "$HSM_KEY_ID" -o "$SIGNING_HOME/public.der"
openssl rsa -pubin -inform DER -in "$SIGNING_HOME/public.der" -outform PEM \
	-out "$SIGNING_HOME/public.pem" 2>/dev/null
echo "public key: $SIGNING_HOME/public.pem"
