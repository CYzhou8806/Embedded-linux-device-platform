# Sourced by the signing scripts. Where the (soft) HSM lives and how to
# reach it - no key material and no PIN in here.
#
# SoftHSM2 implements the same PKCS#11 interface a hardware HSM or a
# smartcard does; swapping it for a real one means changing PKCS11_MODULE
# and the token label, not the scripts.
: "${SIGNING_HOME:=$HOME/.device-platform-signing}"
: "${PKCS11_MODULE:=/usr/lib/softhsm/libsofthsm2.so}"
: "${HSM_TOKEN_LABEL:=device-platform}"
: "${HSM_KEY_LABEL:=rpi5-boot-rsa2048}"
: "${HSM_KEY_ID:=01}"
export SOFTHSM2_CONF="${SOFTHSM2_CONF:-$SIGNING_HOME/softhsm2.conf}"

# The user PIN is read from a file with 0600 permissions, never taken from
# the command line (where it would show up in ps and shell history).
: "${HSM_PIN_FILE:=$SIGNING_HOME/user.pin}"

# PINs never go into argv (world-readable through /proc/<pid>/cmdline and
# ps). pkcs11-tool gets it through its environment - `--pin env:HSM_PIN`,
# visible only to this user and root - and OpenSSL's PKCS#11 engine through
# a 0600 config file.
p11() {
	HSM_PIN="$(cat "$HSM_PIN_FILE")" pkcs11-tool --module "$PKCS11_MODULE" \
		--token-label "$HSM_TOKEN_LABEL" --login --pin env:HSM_PIN "$@"
}

openssl_p11() {
	local conf="$SIGNING_HOME/openssl-pkcs11.cnf"
	if [[ ! -f "$conf" ]]; then
		(umask 077; cat > "$conf" <<CONF
openssl_conf = openssl_init
[openssl_init]
engines = engine_section
[engine_section]
pkcs11 = pkcs11_section
[pkcs11_section]
engine_id = pkcs11
MODULE_PATH = $PKCS11_MODULE
PIN = $(cat "$HSM_PIN_FILE")
init = 0
CONF
		)
	fi
	OPENSSL_CONF="$conf" openssl "$@"
}
