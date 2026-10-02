# Case 14: Update 1.3.0 Rejected by Its Own Health Check, and the Release Gate It Led To

**Platform:** Raspberry Pi 5, `device-platform-image-ab`, RAUC with the firmware's tryboot A/B ([update-and-provisioning](../security/update-and-provisioning.md) §3), kernel 6.12.93 with the second hardening fragment
**Occurred:** 2026-10-02, the first on-board update carrying M5 (device-service's supervisor) and M7's `device-monitor`

This is the same class of failure as [case 11](case-11-ab-update-four-failures-invisible-on-the-build-host.md),
reached again from a clean build. The difference is that this time the
A/B design caught it on the first boot, and the follow-up made both
failure modes checkable before anything is signed.

## What happened

Update 1.3.0 installed into slot B and booted through tryboot. The new
kernel came up (lockdown `integrity`, LSMs `lockdown,yama,landlock`), and
SSH worked. **device-service was not running and had never been
started.** It was `inactive (dead)`, there were no journal entries for
it, and nothing appeared in `systemctl --failed`.

The health check requires device-service to report `"state":"Running"`
over its control socket. It found no socket and left the slot
uncommitted. That is the A/B design doing its job. Nothing on the board
had to be undone: a normal reboot returned to slot A.

## Two independent causes

**1. An ordering cycle; systemd deleted device-service's start job.**

```
device-monitor.service: Found ordering cycle on device-service.service/start
device-monitor.service: Found dependency on multi-user.target/start
device-monitor.service: Job device-service.service/start deleted to break ordering cycle
```

device-service is `After=multi-user.target` (a choice made in V4: the SPI
driver has to be bound first, and there is no systemd edge for that).
`device-monitor` was `WantedBy=multi-user.target` *and*
`After=device-service.service`. That closes a loop:
device-service → multi-user → device-monitor → device-service. systemd
resolves a cycle by dropping one job, logs it once, and carries on. The
dropped job happened to be the one the whole device exists for.

`device-monitor` also had `Wants=network-online.target`. It was not
part of the cycle, but it pulled in `systemd-networkd-wait-online`,
which held the boot for two minutes waiting on an unplugged `eth0`. And
the monitor wasn't even provisioned, so it was skipped by its
`ConditionPathExists`. A skipped unit's `Wants=` and `After=` still
shape the boot.

**2. A command the image doesn't have.** With the cycle broken by
starting it by hand, device-service failed in its `ExecStartPre`:

```
/bin/sh: install: not found        (status 127)
```

The new drop-in that creates `/data/device-service` used `install -d`.
The production image's cut-down BusyBox has no `install` applet. Case 11
lost SSH the same way, to a different missing applet.

## Fixes

- `device-monitor`: `After=device-platform-data.service` only. It
  connects to the control socket per request and answers 503 until the
  socket exists, so it needs no ordering against device-service, and a
  listener on `0.0.0.0` needs no online network.
- The drop-in uses `mkdir -p` / `chown` / `chmod`.

## The release gate

Both failures are visible **offline**, in the rootfs bitbake produces:

- `systemd-analyze verify --root=ROOTFS multi-user.target <every enabled
  unit>` builds the boot transaction and prints the cycle;
- every `Exec*=` line of every unit and drop-in can be resolved against
  the rootfs, including the first word of each command inside
  `sh -c '…'`.

[`tools/check-rootfs-units.py`](../../tools/check-rootfs-units.py) does
both. `make-signed-ab-release.sh` now runs it as step 0 and signs nothing
if it fails. Run against the 1.3.0 rootfs, it reports exactly the two
real problems (3 cycle lines, `install` missing) and no false positives,
after it was taught three systemd rules: a `-` prefix means a missing
binary is allowed, a `Condition*` on the same path makes the unit skip
itself, and `/lib` is `/usr/lib`. On 1.3.1 it reported 0 and 0. 1.3.1 then
booted with device-service `Running` from `Init → Ready → Running` and
was committed by the health check about 25 s after the kernel started.

## A third trap, found while releasing the fix

`sign-release-bundle.sh` still defaulted to the **release-1** key, which
the key rotation revoked (update-and-provisioning §6). Every device with
the CRL would have refused a bundle signed with the default. 1.3.0 was
signed correctly only because the key was overridden by hand. The default
is release-2 now.

## What generalises

- **systemd failures are silent by design.** A deleted job and a skipped
  unit are not failures in `systemctl --failed`. The only reliable
  signal was the end-to-end one, the health check asking the service
  whether it actually runs. That is why the health check asks for the
  machine state, not for `is-active`.
- **If a class of bug reaches the board twice, check for it before
  release.** Case 11 was written up and fixed, but nothing stopped the
  next instance. The gate does.
