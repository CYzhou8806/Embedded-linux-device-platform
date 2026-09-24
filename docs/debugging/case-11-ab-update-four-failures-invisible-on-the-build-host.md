# Case 11: A/B Updates on the Raspberry Pi 5 — Four Failures That Were Invisible on the Build Host

**Platform:** Raspberry Pi 5, `device-platform-image-ab` (two boot partitions, two dm-verity root filesystems, a LUKS2 data partition, RAUC with a custom `tryboot` backend — [`yocto/meta-device-platform-verity/`](../../yocto/meta-device-platform-verity/)), Yocto Scarthgap, kernel `6.6.63-v8-16k`, bootloader firmware 2026-05-26
**Occurred:** security work, signed A/B updates ([docs/security/update-and-provisioning.md](../security/update-and-provisioning.md) §3.1, findings F6 and F11 of the [threat model](../security/threat-model.md))

Every piece of this image had been checked on the build host before the
card was written: the RAUC backend's state machine in a host test (10/10),
and the partition contents byte for byte against the verity images. The first boot still failed, and so
did two more after it. None of the four causes was a logic error in the
update mechanism; all four were about **the device's environment**:
which commands exist, what depends on what, where evidence goes, and in
what order systemd starts things.

## What was expected

```
p1 autoboot.txt   [all] boot_partition=2   [tryboot] boot_partition=3   (tryboot_a_b=1)
p2/p3 boot A/B    p5/p6 dm-verity root A/B    p7 LUKS2 data (created on first boot)
```

First boot on slot A: create the LUKS data partition, put the SSH host
key and RAUC's state on it, start device-service as user `acq`. Then an
update into B: `rauc install` writes the inactive pair, reboots with
`tryboot`, and a health check 30 s after boot runs `rauc status
mark-good` only if device-service is active and the MCU answers. The
backend then rewrites `autoboot.txt` so B becomes the default.

## Failure 1: no SSH after the first boot

The device came up (it answered ping, the port was open) but SSH reset
every connection. The admin account has no password by design
([hardening](../security/hardening.md)), there is no serial getty, and
the journal lives in RAM on a read-only root — **a bad boot left no way
in and no evidence**.

That was the first thing fixed, before the cause was known: a **boot
recorder** ([`device-platform-bootlog`](../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/device-platform-bootlog))
that two minutes after boot writes the failed units, the key services'
status and the whole journal to the small FAT partition 1, keeping the
last three boots. Pulling the card and mounting p1 on the build host
([record](../../results/security/update/first-boot-install-missing.bootlog.txt)):

```
* device-platform-data.service loaded failed failed Encrypted data partition (/data)
device-platform-data[702]: /usr/sbin/device-platform-data: line 59: install: not found
dropbear-hostkey[715]: /usr/sbin/dropbear-hostkey: line 11: install: not found
dropbear-hostkey[717]: Couldn't create new file /run/dropbear/dropbear_rsa_host_key.tmp717: No such file or directory
dropbear-hostkey[717]: Exited: Failed to generate key.
```

**`install` is not in the image.** The same image cut the package count
from 1919 to 171 by installing only the kernel modules the board uses —
and the cut also removed coreutils. BusyBox on this image has no
`install` applet. The data partition had actually been created, opened
and mounted; the script's *last* line (`install -d -m 0700 …`) failed,
the service was marked failed, and the host-key script, which used the
same command, never produced a key. dropbear accepted connections and
had nothing to answer them with.

On the build host every one of these scripts had run fine, because the
build host has coreutils. Fix: `mkdir` + `chmod`, and **every command the
on-device scripts use is now checked against the image's rootfs** before
a card is written.

## Failure 2: remote access depended on the newest component

The fix for failure 1 was one command; the design problem behind it was
bigger. The host key had been moved to `/data` so it would survive the
read-only root — which made SSH depend on the LUKS partition, the
component that is newest, most complex and most likely to break. And SSH
is how a broken `/data` gets repaired.

[`dropbear-hostkey`](../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/dropbear-hostkey)
now uses `/data` only when it is mounted and not marked degraded, and
falls back to `/run` otherwise — a host key that changes at reboot is
annoying; no SSH at all on a password-less device is a site visit.

## Failure 3: the health check never ran

The same boot log had a third problem in it, unrelated to the first two
and easy to miss among 1,200 lines:

```
[5.712133] systemd[1]: multi-user.target: Found ordering cycle on device-platform-healthcheck.service/start
[5.722025] systemd[1]: multi-user.target: Found dependency on device-service.service/start
[5.730428] systemd[1]: multi-user.target: Found dependency on multi-user.target/start
[5.738387] systemd[1]: multi-user.target: Job device-platform-healthcheck.service/start deleted to break ordering cycle starting with multi-user.target/start
```

The health check was `WantedBy=multi-user.target` and
`After=device-service.service`, and device-service is itself ordered
`After=multi-user.target`. multi-user waits for the health check, which
waits for device-service, which waits for multi-user. systemd resolves a
cycle by **deleting a job** and carrying on; the boot succeeds, nothing
is marked failed, `systemctl --failed` is empty.

The failure mode happened to be safe — without the health check nothing
is ever committed, so every update would have been rolled back at the
next reboot — but the gate the A/B design depends on **did not exist**,
and one log line per boot was the only sign. The cycle only exists
once device-service's own ordering (another package) is installed next
to the health check — on the finished image, not in either recipe. The health check is now started by a
timer, 30 s after boot, and `journalctl -b | grep "ordering cycle"` is
part of every on-board check.

## Failure 4: the update that was broken for real

With the first three fixed and slot A healthy, the plan was to test a
successful update to B first and stage a broken one afterwards. The
first bundle installed into B turned out to be the broken one, without
staging: **the bundle had been built from the earlier image** — the one
with `install` missing — because the bundle task hadn't re-run to pick
up the rebuilt image. The install itself succeeded (the bundle was
validly signed and had a higher version):

```
install into B succeeded; autoboot.txt: [all] boot_partition=2, [tryboot] boot_partition=3
reboot "0 tryboot" -> slot B boots, SSH: Connection reset by peer (the broken image)
power cycle -> booted: rauc.slot=A, bootpart 2; autoboot.txt still [all]=2
rauc status: rootfs.1 (B) boot status bad, rootfs.0 (A) good - never committed
```

That is exactly the scenario A/B exists for — a validly signed release
that doesn't work on the device — and it was handled the way the design
says: `tryboot` is one-shot, so the next power cycle, by anyone, with no
network, returned the device to A. Nothing on A was touched.

The successful update came afterwards, with the bundle rebuilt from the
current image and its contents checked first
([record](../../results/security/update/on-target-ab.txt)):

```
Installing `/data/update-ab-1.0.2.raucb` succeeded
reboot "0 tryboot" -> booted: rauc.slot=B, bootpart 3
healthcheck (timer, 30 s after boot): device-service active, MCU answering - marking slot good
autoboot.txt: [all] boot_partition=3, [tryboot] boot_partition=2
normal reboot -> booted: rauc.slot=B, bootpart 3, tryboot flag 0
SSH host key unchanged (same as on slot A)
```

## What this shows

- **The host is not the device.** Three of the four failures were about
  the image's environment (a missing applet, a startup ordering that
  only exists with all packages installed, a stale artifact), and the
  build host had all of them right. The only check that would have caught
  failure 1 before the board was running the scripts inside the image's
  rootfs.
- **Recovery paths must not depend on what they recover.** SSH relied on
  `/data`, so the component most likely to fail took the repair path down
  with it. The same rule applies to the A/B design itself: rollback works
  because it needs nothing from the new slot — just a power cycle.
- **Observability comes before diagnosis.** Nothing was learned about
  failure 1 until the boot recorder existed; after that, one pulled card
  showed failures 1, 2 *and* 3. (The recorder writes logs to an
  unencrypted partition, which is a development aid and not something a
  production image should keep.)
- **Silent is not the same as fine.** systemd deleting a job is logged
  at the same level as routine start-up messages. A safety gate that isn't
  running looks exactly like a safety gate that has nothing to do.

## Takeaways

- Check every command an on-device script uses against the image's rootfs
  (BusyBox here: no `install`, `od` without `-A`/`-t`, `dd` without `conv=`,
  `head -n N` only).
- Before trusting an update bundle, check what's inside it, not just that
  the build succeeded and the signature verifies — the signature said
  "this came from our build", which was true.
- After every boot of a new image: `systemctl --failed` *and*
  `journalctl -b | grep "ordering cycle"`.
- Four card writes, three power cycles, two pulled cards: the cost of
  learning this on a device with a working rollback. Without `tryboot`, failure 4 would have
  needed the card re-flashed.
