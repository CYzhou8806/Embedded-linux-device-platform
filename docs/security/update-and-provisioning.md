# Signed updates, keys, and provisioning

Closes findings **F6** (no update mechanism) and **F14** (no key handling)
of the [threat model](threat-model.md), and gives F12's device identity a
certificate. Status:

| Part | Status |
| --- | --- |
| Key hierarchy in a PKCS#11 token (SoftHSM2) | ✅ done |
| RAUC bundle built by BitBake, signed with a development key | ✅ done |
| Release re-signing with the HSM key, verification matrix | ✅ done — §2 |
| Provisioning station: device identity → certificate → audit log | ✅ done against a test double — §5 |
| A/B slots on the Pi 5 with the firmware's `tryboot`: install, automatic fallback, commit | ✅ on the board — §3.1 |
| Anti-rollback | ✅ on the board (`min-bundle-version`) — §4 |
| Rotating the signing key without touching the keyring; revoking the old one with a CRL | ✅ with the keys in the token (key 05 issued, release-1 revoked) — §6 |
| A board with no RTC: what certificate time checks do before NTP | ✅ measured on the host with the board's real boot-time clock — §7 |

## 1. Four keys, four jobs

All private keys are generated inside the token, `sensitive` and
`never extractable`; only public keys and certificates leave it.

| Token id | Key | Signs | Trusted by | Script |
| --- | --- | --- | --- | --- |
| 01 | RSA-2048 boot key | `boot.img`, EEPROM config, counter-signs Pi firmware | the Pi 5's OTP key hash | [`hsm-init.sh`](../../security/signing/hsm-init.sh) |
| 02 | P-256 Device CA | one certificate per device identity | the backend | [`hsm-device-ca-init.sh`](../../security/signing/hsm-device-ca-init.sh) |
| 03 | P-256 Update CA | release-signing certificates only | every device (its RAUC keyring) | [`hsm-update-keys-init.sh`](../../security/signing/hsm-update-keys-init.sh) |
| 04 | P-256 release signing | update bundles | via the Update CA | same |

Why separate keys:

- **One compromise, one consequence.** A leaked Device CA key lets someone
  mint device identities; it must not also let them ship firmware.
- **Rotation without touching the field.** Devices trust the *Update CA*,
  not the signing key. If the signing key leaks or expires, the CA issues
  `release-2`, and devices accept it without an update to their keyring.
  The boot key (01) has no such indirection — the Pi 5 pins exactly one key
  hash in OTP — which is why losing it or leaking it is so much worse.
- **The PIN never appears in argv.** `pkcs11-tool` gets it through
  `--pin env:…`, OpenSSL's engine through a `0600` config file, RAUC through
  `RAUC_PKCS11_PIN`. (The first version of these scripts passed it on the
  command line, visible to every user through `ps`; found while writing
  the Device CA script and fixed in all of them.)

## 2. Build signing vs release signing

```
 build server (BitBake)                    signing host (HSM)
 device-platform-bundle.bb                 sign-release-bundle.sh
   rootfs = device-platform-image-prod       rauc resign
   signed with the DEVELOPMENT key  ───►       verifies the development signature
   (plain files, build server only)            signs with key 04 in the token
                                               verifies against the Update CA
                                               appends to the signing audit log
```

The build never has access to a release key, and the release step only
accepts a bundle whose development signature verifies — so it signs what
the build produced, not whatever it is handed.

Bundle ([`device-platform-bundle.bb`](../../yocto/meta-device-platform/recipes-core/bundles/device-platform-bundle.bb)):
RAUC 1.15.2, **verity** format, one `rootfs` slot (the production image as
ext4, 184.5 MB, 54 MB bundle), compatible string `device-platform-rpi5`.

What a device would accept — the signature check `rauc install` starts
with, run on the host with `rauc info` and the production keyring
([`test-bundle-verification.sh`](../../security/signing/test-bundle-verification.sh),
output in [`results/security/update/`](../../results/security/update/bundle-verification.txt)):

| Bundle | Keyring | Result |
| --- | --- | --- |
| release (re-signed with the HSM key) | production (Update CA) | **accepted** |
| development | production | rejected: `unable to get local issuer certificate` |
| release | development | rejected: `unable to get local issuer certificate` |
| release, 1 byte changed in the signature | production | rejected: `Signature data is no valid CMS` |
| release, 1 byte changed in the payload | production | **accepted by `rauc info`** — see below |
| same, payload checked against the signed root hash | — | **fails**: `Verification failed at position 27168768` (the 4 KiB block that was changed) |

Two things this surfaced:

- **In the verity format, a valid signature does not mean an intact
  payload.** The signature covers the manifest; the manifest contains the
  root hash of a dm-verity tree over the payload; the payload is checked
  block by block by the kernel when `rauc install` mounts the bundle. So
  `rauc info` passing says nothing about the payload — the check happens
  at install time, and the test reproduces it in userspace with
  `veritysetup`. The advantage is that the device never has to hash (or
  even download) the whole bundle before trusting what it reads.
- **Certificate purpose is checked.** RAUC's default purpose check
  rejected the `codeSigning` certificate (`unsuitable certificate
  purpose`); the device's `system.conf` needs `[keyring]
  check-purpose=codesign`, the same setting the tests pass with `-C`.

## 3. A/B on the Raspberry Pi 5

meta-rauc-community's Raspberry Pi example uses U-Boot, and for the Pi 5
additionally meta-lts-mixins for a newer U-Boot. That conflicts with this
project in two ways: Pi 5 secure boot has the firmware verify `boot.img`
directly ([secure-boot](secure-boot.md)), and the project already relies
on the firmware's own one-shot `tryboot` to boot experimental kernels
safely. The design therefore uses the firmware's A/B support instead of
U-Boot, through RAUC's `custom` bootloader backend:

```
mmcblk0p1  autoboot.txt (FAT, tiny)   [all] boot_partition=2   [tryboot] boot_partition=3
mmcblk0p2  boot A  — boot.img + boot.sig   (kernel cmdline: root=p5, verity root hash)
mmcblk0p3  boot B  — boot.img + boot.sig   (kernel cmdline: root=p6, verity root hash)
mmcblk0p5  rootfs A (read-only, dm-verity)
mmcblk0p6  rootfs B
mmcblk0p7  data — LUKS: samples, logs, WiFi credentials, SSH host key, RAUC status
```

- **Install**: RAUC writes the inactive boot + rootfs pair, then the
  custom backend reboots with `tryboot` — the firmware boots the other
  pair exactly once.
- **Commit**: if the new system comes up healthy (device-service running,
  network reachable — the same kind of check `devbus-bootcheck.service`
  already does for kernel experiments), a service marks the slot good and
  rewrites `autoboot.txt` so it becomes the default.
- **Rollback**: if it doesn't, any reboot — watchdog, `panic=10`, power
  loss — returns to the old pair, because `tryboot` is one-shot. A power
  cut in the middle of writing only ever damages the *inactive* slot.
- Each boot partition carries its own signed `boot.img` with its own
  verity root hash, so the chain of trust covers whichever slot is booted.

Raspberry Pi's 2026-05 firmware also added A/B updates for the EEPROM
bootloader itself, which would be the next layer of the same design.

### 3.1 On the board

Image [`device-platform-image-ab`](../../yocto/meta-device-platform-verity/recipes-core/images/device-platform-image-ab.bb):
the layout above, `autoboot.txt` with `tryboot_a_b=1` (the firmware then
reads the tried partition's normal `config.txt`), one `config.txt` that
picks `cmdline-a.txt` or `cmdline-b.txt` with `[boot_partition=N]`, a RAUC
custom backend ([`rauc-tryboot-backend`](../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/rauc-tryboot-backend),
its state machine tested on the host first: [`tests/test-rauc-backend.sh`](../../yocto/meta-device-platform-verity/tests/test-rauc-backend.sh)),
and a health check that runs `rauc status mark-good` only when
device-service is active and the MCU answers. meta-rauc's
`rauc-mark-good`, which marks every booted slot good unconditionally, is
kept out of the image. Full record:
[`results/security/update/on-target-ab.txt`](../../results/security/update/on-target-ab.txt).

| Test | Result |
| --- | --- |
| Install a release-signed 0.9.0 bundle on a 1.0.0 system | refused: `Version mismatch: Expected at least '1.0.0' but bundle manifest has '0.9.0'` |
| **Failed update**: slot B receives an image in which SSH never comes up | tryboot into B, device unusable; **one power cycle → back on A**, `autoboot.txt` never changed, B left `bad` |
| **Successful update**: 1.0.2 into B | installed (≈15 s); tryboot into B; health check 30 s after boot → `marked slot(s) rootfs.1 as good` → backend commits: `[all] boot_partition=3`; a normal reboot stays on B |
| State across slots | the SSH host key on `/data` is the same on A and on B |

The failed update was not staged: the bundle had been built from an
earlier image with a real bug (below). It exercised exactly the path the
design exists for.

Four problems were found only on the board, each invisible on the build
host:

- **`install` isn't in the image.** Cutting the image to 171 packages
  removed coreutils; BusyBox here has no `install` applet. The data
  partition was created and mounted, then the script's last line failed,
  and so did the SSH host-key script — no SSH, and an administrator with no
  password can't log in any other way. Every command the scripts use is
  now checked against the image's rootfs.
- **SSH depended on the new component.** The host key now goes to `/data`
  only when it is mounted and healthy, and to `/run` otherwise: remote
  access can't depend on the part most likely to break.
- **No evidence after a bad boot.** The journal lives in RAM on a
  read-only system. A boot recorder now writes each boot's failed units
  and journal to partition 1 two minutes after boot (last three kept);
  pulling the card was enough to read why SSH had died.
- **The health check never ran.** It was `WantedBy=multi-user.target` and
  ordered after `device-service`, which is itself ordered after
  `multi-user.target`: systemd logged "Found ordering cycle … deleted to
  break ordering cycle" and dropped the job at every boot. The failure
  mode was safe — nothing ever got committed — but the gate the design
  relies on did not exist, and only one log line said so. It is started
  by a timer now.

## 4. Anti-rollback — why a signature isn't enough

Every old release is validly signed, including the ones with known
vulnerabilities. Without anti-rollback, an attacker installs last year's
release and exploits last year's bug.

- **In the updater** (verified on the board, §3.1): RAUC 1.15 has this built in — `[system]
  min-bundle-version` in `system.conf` rejects any bundle whose manifest
  version is older. Each release ships a `system.conf` that raises the
  limit, so once it runs, nothing older installs. Two caveats from RAUC's
  own documentation and API: a deliberate rollback (bad release) then has
  to ship the old content under a *new* version number; and the D-Bus
  install call accepts `ignore-version-limit`, so who may call RAUC's
  D-Bus API has to be restricted, or root can simply ask it to skip the
  check. (Before finding the option, the plan here was a `pre-install`
  handler comparing `RAUC_MF_VERSION`, which RAUC exports to handlers —
  the built-in check is the better answer.) The limit lives in the
  verified rootfs, so it holds as long as the running system is
  trustworthy, and `DEVICE_PLATFORM_RELEASE` has to be set by the release
  process and only ever increase.
- **Against someone with the SD card:** they don't need the updater; they
  write an old, validly signed image directly. That can only be stopped
  by a counter they can't reset: OTP bits (monotonic by physics — each
  release burns one more bit; limited in number, irreversible), an eMMC
  RPMB counter, or a TPM. The Pi 5 has OTP rows but no RPMB on an SD card.
- The same lesson came out of the OP-TEE work ([optee](optee.md) §3.2):
  restoring an old copy of secure storage let the TA create a second
  identity. **Any rule enforced through rewritable storage can be undone
  by rolling that storage back.**

## 5. Provisioning

[`tools/provision-device.sh`](../../tools/provision-device.sh) is the
factory station. The device makes its own identity key (the TA from
[optee](optee.md)); the station never handles a private key:

1. read the serial number; refuse if it is already in the log;
2. `devid provision` on the device → public key (refused by the TA if the
   device already has an identity — a device we didn't provision);
3. **proof of possession**: send a fresh nonce, verify the signature with
   the reported public key;
4. issue an X.509 certificate for that key, signed by the Device CA in the
   HSM (`CA:FALSE`, `digitalSignature`, `clientAuth`, subject = serial);
5. append one JSON line: time, station, operator, serial, key fingerprint,
   certificate serial and fingerprint, CA fingerprint.

Step 3 exists because of a design choice in the TA: the usual way to
prove key possession is a CSR, but the TA refuses to sign anything except
domain-separated identity challenges. Proof of possession therefore uses
the challenge path, and the certificate is built from a throwaway request
carrying only the subject, with `-force_pubkey` inserting the device's key.

Tested without a board against a clearly labelled software stand-in
(`testing/fake-devid`, key in a file) —
[`test-provisioning.sh`](../../security/optee-ta/device_identity/testing/test-provisioning.sh),
[`results/security/provisioning/`](../../results/security/provisioning/station-tests.txt):

```
PASS  new device is provisioned and certified
PASS  same serial a second time is refused (log)
PASS  device with an identity we never issued is refused
PASS  device that can't prove possession is refused
PASS  certificate carries the device's key, not the throwaway CSR key
PASS  exactly one log record
```

What a real line adds, and this doesn't: the log in a database the
station can append to but not rewrite, the SSH host key and LUKS setup
for the data partition generated on the device during the same session,
and a check that the device booted the expected signed image before it
gets an identity at all.

## 6. Rotating and revoking the release key

§1 claims that a two-level hierarchy lets the signing key be replaced
without touching devices. This section tests that claim, and the one
that usually comes with it: that a leaked key can be revoked.

[`hsm-rotate-release-key.sh`](../../security/signing/hsm-rotate-release-key.sh)
generates a new key in the token (id 05), has the Update CA (id 03) issue
`release signing 2`, records both certificates in a small `openssl ca`
database, revokes release-1 (`keyCompromise`) and issues a CRL — every
signature by the CA key in the token.
[`test-key-rotation.sh`](../../security/signing/test-key-rotation.sh)
signs the real bundle with release-2 through the normal release step
and runs the matrix below. It was measured twice: first with a throwaway
file-based CA of the same profile, then with the real keys in the token
(key 05, release-2 serial `7EA5…97D3`, release-1 `0FC4…A2E3` revoked) —
identical results
([record](../../results/security/update/key-rotation-and-revocation.txt)):

| Bundle | Device keyring | `check-crl` | Result |
| --- | --- | --- | --- |
| release-1 | CA only (as shipped) | off | accepted |
| **release-2** | **CA only (as shipped)** | off | **accepted — rotation needs nothing on the device** |
| **release-1** | CA + CRL | on | **rejected: `certificate revoked`** |
| release-2 | CA + CRL | on | accepted |
| release-1 | CA + CRL | off | **accepted** — RAUC only warns `Detected CRL but CRL checking is disabled!` |
| release-2 | CA only | on | **rejected: `unable to get certificate CRL`** |
| release-2 | CA + expired CRL | on | **rejected: `CRL has expired`** |

**And on the device**: update 1.1.1 was signed with release-2 only and
installed on the board whose keyring holds nothing but the Update CA:
`Verified inline signature by '… release signing 2'`, installed,
tryboot, committed ([record](../../results/security/update/on-target-signed-ab.txt)).

**Revocation on the device** (1.2.0 on): the keyring in the image is the
Update CA *plus* the CRL, and `system.conf` has `check-crl=true`. The
recipe refuses to build a keyring with `check-crl=true` and no CRL in it,
because that combination rejects every bundle. On the board:

```
rauc install 1.1.0 (signed with release-1):  signature verification failed: Verify error: certificate revoked
rauc install 1.2.1 / 1.2.2 (release-2):      Verified inline signature by '… release signing 2' -> installed, committed
```

The first CRL was issued with 30 days of validity; it was re-issued with
365 days before going into an image, since an expired CRL on a device
blocks every update. Renewing it (`CRL_DAYS=365 hsm-rotate-release-key.sh`)
is now part of every release, well before `nextUpdate` (2027-09-24).

What that means:

- **Rotation and revocation are not symmetric.** Rotation works from
  the signing side alone. Revocation only works on a device that holds the
  CRL *and* has `check-crl=true` — the CRL has to reach the device, which
  normally means inside an update signed by a key the device still trusts.
  A device that is offline, or never updated, still accepts the revoked
  key. For a leaked key the order is: issue release-2, ship an update
  (signed with release-2) that carries the CRL and turns checking on,
  and accept that devices which never take that update stay exposed.
- **Checking must ship together with a CRL, from the factory on.**
  `check-crl=true` with no CRL in the keyring rejects *every* bundle,
  including valid ones. Turning it on in the field without the CRL next
  to it bricks the update path.
- **A CRL expires, and an expired CRL is also a brick.** Every CRL has a
  `nextUpdate`; after it, RAUC refuses all bundles. A device that sits in
  a warehouse longer than the CRL's validity can't be updated at all. RAUC's
  documentation warns about exactly this. Choices: long-lived CRLs (a year;
  revocation then takes up to a year to be "fresh", which doesn't matter
  since devices only learn about it through updates anyway), or a CRL
  refresh as part of every release.
- **What revocation cannot do**: bundles signed with release-1 *before*
  the leak are revoked too. If anything must still install them, they
  have to be re-signed with release-2 — the release step does exactly that.

The boot key (id 01) has none of this: the Pi 5 pins one customer key
hash in OTP, with no CA above it and no CRL. A leaked boot key is trusted
by every locked device for life. That is the strongest argument for the
HSM, and against ever signing boot images on a build server.

## 7. A device without a real-time clock

The Raspberry Pi 5 has an RTC, but it keeps time across power loss only
with a battery on its `BAT` connector, and this device has no battery.
(Whether the image's cut-down module list still loads the RTC driver was
not checked.) The boot recorder's log shows what the clock says before NTP: every early message is dated **2025-06-26 08:44**.
That is systemd's `time-epoch` — poky sets it to systemd's reproducible
build timestamp (`SOURCE_DATE_EPOCH` = 1750927453) — and it is where every
boot starts, because the file that would carry the last synced time over
a reboot (`/var/lib/systemd/timesync/clock`) is on tmpfs in the
read-only image.

The certificates were issued on 2026-09-23. RAUC's certificate check
with the clock set to the board's boot-time value (host test, same RAUC
binary, `time()` replaced through `LD_PRELOAD`;
[record](../../results/security/update/clock-without-rtc.txt)):

| Clock | Option | Result |
| --- | --- | --- |
| correct | — | accepted |
| **2025-06-26 (boot, before NTP)** | — | **rejected: `certificate is not yet valid`** |
| 1970 | — | rejected: `certificate is not yet valid` |
| 2029 (release-1 expired) | — | rejected: `certificate has expired` |
| 2025-06-26 | `use-bundle-signing-time=true` | accepted |
| **2029** | `use-bundle-signing-time=true` | **accepted — an expired key still works** |
| 2025-06-26, CA + CRL | `check-crl=true` | rejected: `CRL is not yet valid` |

So on this device, an update installed before NTP has synced fails, with
an error message that points at the certificate rather than the clock.
Online, that is only a race right after boot; an offline device (a USB
stick at a customer site with no internet) can never be updated. The
choices and what each costs:

- **`use-bundle-signing-time=true`** — RAUC checks the certificates at
  the time the bundle says it was signed. Fixes the clock problem
  completely, but the signing time is written by the signer: whoever holds
  a leaked or expired key backdates the signature, and expiry means
  nothing. That leaves revocation as the only way to retire a key, and
  a CRL has its own validity window, which is checked against the same
  wrong clock unless this option is set (last row).
- **A floor for the clock** — systemd never starts earlier than the mtime
  of `/usr/lib/clock-epoch` if that is newer than its built-in epoch.
  Stamping that file with the image's build time means a device never
  believes it is earlier than the image it runs, which is always later
  than the certificates that signed its predecessors. Cheap, and it keeps
  real expiry checks. Persisting timesyncd's clock file on `/data` moves
  the floor forward with every sync.
- **An RTC battery** — the right answer for a product, costs a part.
- **Secure time** (a signed timestamp from a TSA, or roughtime) — solves
  "who says what time it is" properly; more infrastructure than this
  project needs.

The recommendation for this device is the second one, which keeps
`check-crl` and certificate expiry meaningful. The general point: **a
certificate check is a clock check**, and on embedded devices the clock
is the part nobody provisioned.
