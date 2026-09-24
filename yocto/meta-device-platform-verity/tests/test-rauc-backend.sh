#!/bin/sh
# Host-side state-machine test for recipes-support/device-platform-ab/files/
# rauc-tryboot-backend: plays the firmware (which partition booted) and RAUC
# (which commands it calls) through a successful update, a failed update and
# a slot marked bad, and checks autoboot.txt after each step.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
B="$here/../recipes-support/device-platform-ab/files/rauc-tryboot-backend"
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
export AUTOBOOT_FILE="$t/autoboot.txt" PARTITION_FILE="$t/partition" STATE_DIR="$t/state"
boot() { printf "\000\000\000\00$1" > "$PARTITION_FILE"; }          # firmware booted partition $1
ab() { awk '/^\[/{s=$0;next} /^boot_partition=/{sub(/boot_partition=/,"");printf "%s=%s ", s, $0}' "$AUTOBOOT_FILE"; }
fail=0
check() { got=$(ab); if [ "$got" = "$1" ]; then echo "ok    $2  ($got)"; else echo "FAIL  $2: expected '$1', got '$got'"; fail=1; fi; }

printf '[all]\ntryboot_a_b=1\nboot_partition=2\n[tryboot]\nboot_partition=3\n' > "$AUTOBOOT_FILE"
boot 2
[ "$(sh "$B" get-current)" = A ] && [ "$(sh "$B" get-primary)" = A ] && echo "ok    running A, primary A"
check "[all]=2 [tryboot]=3 " "initial"

# 1. successful update to B
sh "$B" set-primary B
check "[all]=2 [tryboot]=3 " "install to B: only [tryboot] points at B, A stays committed"
boot 3                                   # reboot "0 tryboot" -> firmware boots partition 3 once
sh "$B" set-state B good                 # health check passed -> rauc status mark-good
check "[all]=3 [tryboot]=2 " "B healthy -> committed"

# 2. failed update to A: the new slot never becomes healthy
sh "$B" set-primary A
check "[all]=3 [tryboot]=2 " "install to A"
boot 2                                   # tryboot into A ... it hangs; power cycle
boot 3                                   # one-shot: next boot is the committed slot B again
sh "$B" set-state B good                 # B's health check runs as usual
check "[all]=3 [tryboot]=2 " "A never healthy -> B still committed"

# 3. a slot marked bad is reported bad and not committed by a stray good on the other
sh "$B" set-state A bad
[ "$(sh "$B" get-state A)" = bad ] && echo "ok    A marked bad" || { echo "FAIL  get-state A"; fail=1; }
[ "$(sh "$B" get-state B)" = good ] && echo "ok    B good" || { echo "FAIL  get-state B"; fail=1; }

# 4. mark-good on a slot that is not running must not commit it
sh "$B" set-state A good
check "[all]=3 [tryboot]=2 " "good for a slot that isn't running changes nothing"

# 5. bad input
sh "$B" set-primary C 2>/dev/null && { echo "FAIL  accepted slot C"; fail=1; } || echo "ok    slot C rejected"
exit $fail
