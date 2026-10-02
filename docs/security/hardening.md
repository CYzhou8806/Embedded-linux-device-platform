# Production image hardening

Closes findings **F1, F2, F3, F11** and the first half of **F5** from the
[threat model](threat-model.md), in the image recipe
[`device-platform-image-prod`](../../yocto/meta-device-platform/recipes-core/images/device-platform-image-prod.bb).

**Status:** built, every change verified in the generated root
filesystem, and **booted and checked on the Raspberry Pi 5** (§5). Raw
output: [`results/security/hardening/first-boot-prod.txt`](../../results/security/hardening/first-boot-prod.txt).

## 1. Two images, one set of software

`device-platform-image-prod` `require`s the development image and only
changes how the device can be reached and modified. It never changes
*what runs*, so a bug found on one image reproduces on the other.

| | `device-platform-image` (development) | `device-platform-image-prod` |
| --- | --- | --- |
| root password | empty (`debug-tweaks`) | `*` — no password can match |
| SSH | dropbear `-B`: empty passwords accepted, root allowed | dropbear `-w -s -j -k`: no root, **no passwords at all**, no port forwarding |
| Who can log in | root, over SSH, serial and HDMI console | `admin`, with one SSH key, then `sudo` |
| Serial console (`ttyAMA10`) | root login prompt | getty masked (`→ /dev/null`) |
| Root filesystem | read-write | read-only (`read-only-rootfs`), `/var/lib`, `/var/log`, `/tmp` on tmpfs |
| `device-service` | root, unrestricted | root uid, 2 capabilities, systemd sandbox (§3) |

## 2. What each change is, and how it was checked

All checks are on the root filesystem tarball the build produced,
compared with the development image built from the same revision.

**F1 — no remote root, no passwords.** `debug-tweaks` is removed; it is
what sets `root::` (empty hash) in `/etc/shadow` and adds dropbear's `-B`.
A new recipe, [`device-platform-admin`](../../yocto/meta-device-platform/recipes-support/configuration/device-platform-admin_1.0.bb),
creates `admin` through `useradd.bbclass` with the password field set to
`*`, installs its `authorized_keys` (owned `1000:1000`, `0600`, in a `0700`
`.ssh`) and a `sudoers.d` entry (`0440`). Two details that matter:

- `*`, not `!`. Both mean "no password", but dropbear treats a
  `!`-prefixed hash as a *locked account* and refuses public-key logins
  too.
- The key itself comes from a file outside the repository
  (`DEVICE_PLATFORM_ADMIN_PUBKEY`), like the WiFi credentials. The recipe
  declares it with `do_install[file-checksums]`, so replacing the key
  changes the task hash — without it, sstate would silently keep
  installing the old key (the same bug existed for the WiFi file and was
  fixed alongside).

```
dev   /etc/shadow   root::            /etc/default/dropbear   DROPBEAR_EXTRA_ARGS=" -B"
prod  /etc/shadow   root:*  admin:*   /etc/default/dropbear   DROPBEAR_EXTRA_ARGS="-w -s -j -k"
```

`-j`/`-k` disable local and remote port forwarding: a stolen admin key
still can't turn the device into a tunnel into the customer's network.

**F3 — no login on the UART header.** `SERIAL_CONSOLES` is a machine
setting shared with the development image, and `systemd-serialgetty` is
a hard dependency of systemd here, so the getty can't simply be left out
of the build. The production image masks each `serial-getty@<tty>`
instance instead. (The first version of that post-process step failed:
`SERIAL_CONSOLES` is `115200;ttyAMA10`, and the unquoted `;` became a shell
command separator. The tty names are now split out in BitBake's inline
Python.)

**F5 (first half) — read-only root.** `IMAGE_FEATURES += "read-only-rootfs"`
mounts `/` read-only and moves `/var/lib`, `/var/log` and `/tmp` onto tmpfs
through `volatile-binds`. Side effect worth knowing: Yocto also drops
packages that only exist to modify a running system (`shadow`,
`base-passwd`, `update-rc.d`, `update-alternatives`), which took the
setuid binaries from 15 to 8.

**F11 — sandboxed service.** See §3.

### Before and after

| | development | production |
| --- | --- | --- |
| Accounts that can log in | root (no password) | admin (key only) |
| SSH password logins | accepted, including empty | impossible |
| Login prompts on UART | 1 (root) | 0 |
| setuid binaries | 15 | 8 |
| `device-service` exposure (`systemd-analyze security`) | 9.4 UNSAFE | **1.8 OK** |
| Packages | 1918 | 1919 (−4 image-time tooling, +5 admin/sudo/sandbox) |
| of which kernel modules | 1808 | 1808 |
| Root filesystem | 121.7 MB, rw | 121.6 MB, ro |
| Unpatched CVEs, userspace | 16 | 4 (§4) |

The package count is dominated by `kernel-modules`, which installs every
module the kernel builds (1808 packages, 26 MB) because it was the quick
fix for missing WiFi drivers during bring-up. Replacing it with the
modules this board loads is the obvious next minimisation step; it wasn't
done here because it can only be verified on the board.

## 3. Sandboxing `device-service`

What the service needs: `/dev/acq0`, write access to the driver's sysfs
attributes (backpressure writes `sample_rate`), POSIX shared memory for
devbus, `sd_notify`, and — only if `config.json` asks for them —
`SCHED_FIFO` and `mlockall()`. A drop-in
([`hardening.conf`](../../yocto/meta-device-platform/recipes-apps/device-service/files/hardening.conf),
installed only by the production image) takes everything else away:

- `CapabilityBoundingSet=CAP_SYS_NICE CAP_IPC_LOCK`, `NoNewPrivileges=yes`.
  The process keeps uid 0 because `/dev/acq0` and the sysfs attributes are
  root-owned, but root without `CAP_DAC_OVERRIDE`, `CAP_SYS_MODULE`,
  `CAP_SYS_ADMIN`… can only touch files it owns.
- `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, `DevicePolicy=closed`
  + `DeviceAllow=/dev/acq0 rw`, `PrivateNetwork` and
  `RestrictAddressFamilies=AF_UNIX` (devbus is shared memory; sd_notify and
  journald are Unix sockets), `SystemCallFilter=@system-service`, plus the
  usual `Protect*`/`Restrict*` set.
- `ProtectKernelTunables` is deliberately **not** set: it makes `/sys`
  read-only, and writing `sample_rate` there is how backpressure works.
  This is the kind of line a generic hardening checklist gets wrong.

`systemd-analyze security --offline` (systemd 255, the same major version
as the image): **9.4 UNSAFE → 1.8 OK**. Full output:
[`results/security/hardening/`](../../results/security/hardening/).

The remaining step — running as a dedicated user — needs a udev rule for
`/dev/acq0` and group ownership of the sysfs attributes, and is left for
when it can be tested against the real driver.

## 4. Vulnerability management (F2)

`INHERIT += "cve-check"` produces a report for every recipe against the
NVD database (downloaded by the build: 378,490 entries, 49 minutes
without an API key). Scarthgap also generates an SPDX 2.2 SBOM for every
image by default.

For `device-platform-image-prod`, NVD matching gives **3,869 unpatched
entries**. The number is meaningless until it is triaged, and triage
splits it into two very different problems.

### Userspace: 16 → 4, each with a reason

Every entry was checked against what the image actually contains:

| Package | CVEs | Evidence | Result |
| --- | --- | --- | --- |
| openssh | 6 | ssh client, ssh-agent, X11 forwarding, GSSAPI, sshd — the image contains only `openssh-sftp-server` (pulled in by `ssh-server-dropbear`); the SSH server is dropbear | `not-applicable-config` |
| expat | 6 | the only programs linking libexpat are `dbus-daemon` and its launch helper, parsing root-owned config on a read-only rootfs — no attacker-supplied XML | `not-applicable-config` |
| glibc | CVE-2026-5450 (9.8) | needs `scanf` with `%mc` and width > 1024; **no ELF in the image contains a `%mc` format** | `vulnerable-investigating`, fixed by the next scarthgap glibc |
| glibc | CVE-2026-5928 | `ungetwc` with an overlapping multi-byte charset; imported only by libstdc++, image uses C/UTF-8 | open, low |
| glibc | CVE-2026-6238 | deprecated `ns_printrr*`/`fp_nquery` — no binary imports them | open, low |
| glibc | CVE-2010-4756 | glob() CPU/memory exhaustion via FTP-style patterns | open, low |

The decisions are recorded as `CVE_STATUS` in
[`recipes-security/cve-status/`](../../yocto/meta-device-platform/recipes-security/cve-status/),
and the next report shows them as *Ignored* with the reason attached.
One caveat is written into each file: these are statements about **this
image** stored on a **recipe**. If someone adds the ssh client, the
openssh entries become wrong. That tension is inherent to recipe-level
CVE status and is the reason each reason string names its evidence.

### Kernel: 3,853 entries, and why the answer is "follow stable"

Version matching against NVD reports every CVE whose range covers 6.6.63,
including drivers this kernel never builds. poky ships
`scripts/contrib/improve_kernel_cve_report.py`, which uses the kernel CNA's
own records ([vulns.git](https://git.kernel.org/pub/scm/linux/security/vulns.git))
and the list of compiled files. On scarthgap with `linux-raspberrypi` it
needed three workarounds, all in
[`security/cve/kernel-cve-triage.sh`](../../security/cve/kernel-cve-triage.sh):

1. the recipe version carries an epoch (`1_6.6.63+git`), which the script
   can't parse;
2. scarthgap's cve-check JSON has no per-issue `detail` field, which the
   script dereferences;
3. **`SPDX_INCLUDE_COMPILED_SOURCES` doesn't work for this kernel**: the
   SPDX it produces lists all 33,155 `.c` files of the tree, AMD GPU
   drivers included, while the build produced 7,048 objects. The real list
   is rebuilt from the `.o` files in the build directory (6,357 `.c`/`.S`),
   counting every header as compiled to stay conservative.

Result ([`results/security/cve/kernel-triage-summary.txt`](../../results/security/cve/kernel-triage-summary.txt)):

```
NVD version match only:   Unpatched 3853
+ kernel CNA + compiled:  Unpatched 3602, Ignored 3189 (2875 code not compiled, 314 rejected)
unpatched 3602: 1102 fixed in a later 6.6.y (newest needed 6.6.157)
                 323 fixed only in newer branches (6.12 / 6.18 / 7.x)
                2177 NVD-only, no fix data from the kernel CNA
```

Two conclusions:

- **The compiled-file filter matters** — 2,875 CVEs are in code this
  kernel doesn't contain — but only with a correct file list. With the
  list scarthgap generated, it removed 90.
- **Triage is not the fix for a kernel that is 94 stable releases behind.**
  meta-raspberrypi's scarthgap branch pins 6.6.63 (December 2024); the
  current 6.6 stable is 6.6.157. Tracking the stable branch fixes 1,102
  entries at once. The answer to "how do you handle a hundred kernel CVEs"
  is a process — follow stable, rebuild, run the regression suite — and
  per-CVE analysis only for the few that stable doesn't cover.

## 5. On the board

Every item was checked on the running device, including the negative
ones — an access that is supposed to fail was attempted and seen to fail:

| Check | Result |
| --- | --- |
| `admin` with the SSH key, then `sudo` | works |
| `root` with the same key | `Permission denied (publickey)` |
| `admin` with a password | `Permission denied (publickey)` — the server doesn't offer a password method at all |
| Port forwarding through the device to the router's web UI | `channel open failed: administratively prohibited` (the same request made directly: HTTP 200) |
| Listening sockets | port 22 only (plus systemd-resolved on 127.0.0.53/54) |
| Serial getty | `masked`, `inactive` |
| `/` | `ext4 (ro,relatime)`; writing `/etc` → `Read-only file system` |
| `device-service` | active, MCU answering, 1000 samples/s, no overflow |
| its process | uid 0, `CapEff = CapBnd = 0x804000` (only `CAP_IPC_LOCK`, `CAP_SYS_NICE`), `NoNewPrivs=1`, seccomp filter on, own network namespace, sees `/` read-only and `/sys` writable |
| backpressure under the sandbox | writing `sample_rate` with the same sandbox properties: 900 written and read back, 1000 restored |

Two consequences of the read-only root filesystem, both predicted from
the build output and both **confirmed by one reboot**:

- **The SSH host key changes on every boot** (`SHA256:/kjyH7sa…` →
  `SHA256:wDpLE42O…`; the client refuses with `Host key verification
  failed`). dropbear keeps it in `/var/lib/dropbear`, which is tmpfs. A
  fingerprint that changes at every reboot trains administrators to accept
  any fingerprint — the setup a man-in-the-middle needs. Fix: a per-device
  host key generated at provisioning and kept on a persistent data
  partition ([update-and-provisioning](update-and-provisioning.md)).
  Baking one into the image would be worse — every device would share it.
- **The device's IP address changes on every boot** (`.177` → `.178`) —
  not predicted, found on the board. `/etc/machine-id` is a tmpfs
  regenerated at each boot, systemd-networkd derives its DHCP client
  identifier (a DUID) from it, and the router sees a new client. Fixed in
  `wlan0.network` with `[DHCPv4] ClientIdentifier=mac` and **verified**:
  the machine-id still changes at reboot, the address no longer does. The
  machine-id itself should, like the host key, come from provisioning.
  Fixing the IP made the host-key problem *more* visible, which is the
  point: with a stable address, every reboot now greets the administrator
  with `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! … someone could be
  eavesdropping on you right now (man-in-the-middle attack)` — a warning
  people learn to click through within a week.

Both remaining items were done in `device-platform-image-ab` (on the board,
[record](../../results/security/update/on-target-ab.txt)):

- **device-service runs as user `acq`**, not root: a udev rule gives the
  group `/dev/acq0` and write access to exactly the two sysfs attributes
  it drives (`control`, `sample_rate`); `CapEff = CapBnd = 0x804000`;
  1000 samples/s, no gaps.
- **Only the kernel modules the board uses**: the list comes from `lsmod`
  on the running system, minus Bluetooth and the camera/codec stack that
  were loaded only because the hardware exists. **1919 packages → 171,
  1808 kernel modules → 31.** The cut also removed coreutils, and with it
  `install`, which a script of this project relied on — see
  [update-and-provisioning](update-and-provisioning.md) §3.1 for how that
  surfaced.

## 6. Compile-time hardening of the binaries

What `checksec` would report, counted over every ELF file in
`device-platform-image-ab`'s root filesystem by
[`binary-hardening-report.py`](../../security/hardening/binary-hardening-report.py)
(pyelftools, reads headers only — no need to run aarch64 code;
[full list](../../results/security/hardening/binary-hardening.txt)):

| | executables (178) | shared libraries (94) |
| --- | --- | --- |
| PIE | 100 % | — |
| full RELRO (`-z relro -z now`) | 100 % | 100 % |
| NX stack | 100 % | 100 % |
| stack canary seen (`__stack_chk_fail`) | 96.6 % | 85.1 % |
| FORTIFY seen (`__*_chk`) | 88.2 % | 63.8 % |
| BTI + PAC marked | 100 % | 97.9 % |

Nothing had to be added for this: poky's distro config requires
`security_flags.inc` (`-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`,
PIE, `-z relro -z now`), and poky's generic arm64 tune (`arch-arm64.inc`) adds
`-mbranch-protection=standard` for every aarch64 machine. `device-service`, `devbus` and
`acq-bridge` get the same flags because they're built by BitBake (and by
the SDK, whose `$CC` carries them — checked with a test program).

Reading the numbers correctly:

- **The canary and FORTIFY columns are lower bounds.** A function gets a
  canary only if it has something worth protecting on its stack, and a
  program shows FORTIFY only if the compiler found a call it could check.
  The 6 executables without a visible canary are tiny programs
  (`dbus-uuidgen`, `systemd-ac-power`…), not ones built without the flag.
  glibc itself is built without `SECURITY_CFLAGS` on purpose — it is the
  code that implements these checks.
- **The tool was checked against known answers.** The same test program
  built with the SDK's flags shows every column; built with
  `-fno-stack-protector -U_FORTIFY_SOURCE -no-pie -z norelro -z lazy` it
  shows none. As a second reference, OP-TEE's Buildroot rootfs from
  [optee](optee.md): full RELRO on 57 % of its libraries, canaries on
  54 %, BTI/PAC on none — a different build system's defaults, visible in
  the same report.
- **BTI and PAC are marked but do nothing on this board.** Both are
  encoded in the hint instruction space so that the same binary runs
  everywhere; on a CPU without them the instructions are NOPs. The Pi 5's
  Cortex-A76 is Armv8.2 — pointer authentication arrived in Armv8.3 and
  BTI in Armv8.5. The binaries are ready for newer silicon (Cortex-A78AE,
  A720…); on this one, return-address protection comes from the canary
  alone. Two libraries (`libffi`, `libzstd`) aren't marked at all: they
  contain hand-written assembly without the BTI landing pads, and the
  linker drops the property for the whole file if one object lacks it.

## 7. The kernel: configuration and runtime settings

### Configuration

[`kconfig-hardening-check.py`](../../security/hardening/kconfig-hardening-check.py)
checks 44 options — a hand-picked arm64 subset of the Kernel Self
Protection Project's recommendations, each with its reason in the script
(the full [kernel-hardening-checker](https://github.com/a13xp0p0v/kernel-hardening-checker)
has several hundred and is the tool to run in CI;
`pip install git+https://github.com/a13xp0p0v/kernel-hardening-checker`).
Before: the 6.6.63 kernel the A/B image runs. After: 6.12.93 with the
fragment [`hardening.cfg`](../../yocto/meta-device-platform-verity/recipes-kernel/linux/files/hardening.cfg)
([record](../../results/security/hardening/kernel-config-hardening.txt)):

| | 6.6.63 (flashed) | 6.12.93 + `hardening.cfg` (1.1.0) | 1.2.x |
| --- | --- | --- | --- |
| self-protection (31 checks) | 12 | 27 | 28 |
| attack surface (13 checks) | 3 | 6 | 8 |
| **total** | **15 / 44** | **33 / 44** | **36 / 44** |

1.2.x adds what the board had to show first: `MODULE_SIG_FORCE` (after
seeing every module, `custom_acq` included, signed and accepted), and
`COMPAT` and `IO_URING` off (no 32-bit ELF and no io_uring user in the
image; only the `__arm64_sys_io_uring_*` entry names remain, which return
`ENOSYS`). The remaining eight are the deliberate ones below.

What changed, and what each buys:

- **Heap and copy hardening** — `HARDENED_USERCOPY`, `FORTIFY_SOURCE`,
  `SLAB_FREELIST_HARDENED`/`_RANDOM`, `SHUFFLE_PAGE_ALLOCATOR`,
  `INIT_ON_ALLOC_DEFAULT_ON`, `LIST_HARDENED`, `BUG_ON_DATA_CORRUPTION`:
  the options that turn a kernel memory-corruption bug from "exploitable"
  into "crash". Raspberry Pi's defconfig optimises for compatibility and
  speed and leaves all of them off.
- **`STRICT_DEVMEM` + `IO_STRICT_DEVMEM`** — on the stock Pi kernel root
  could read and write *all* physical memory through `/dev/mem`, which
  makes every other kernel protection optional for an attacker with root.
- **`MODULE_SIG` + `MODULE_SIG_ALL`**, not yet `MODULE_SIG_FORCE`: every
  in-tree module is signed at build time with a key the kernel build
  generates and discards. Before enforcing, the board has to show that the
  out-of-tree `custom-acq` gets signed too — otherwise enforcement would
  remove the product's own driver. (During the B6 work a test driver was
  `insmod`ed from `/tmp`; enforcement is exactly what makes that
  impossible.)
- **Yama**, `SECURITY_DMESG_RESTRICT`, and three pieces of attack surface
  off (`HIBERNATION`, `LEGACY_TIOCSTI`, `LDISC_AUTOLOAD`).

Left as they are, deliberately (the "why not" is in the fragment):
`INIT_ON_FREE_DEFAULT_ON` (measurable cost on every free), the lockdown
LSM, `KPROBES` and `FTRACE` (this project debugs with ftrace — cases 04
and 09 were solved with it; a production kernel would drop them and keep a
separate debug build), `COMPAT` and `IO_URING` (not yet shown to be unused).
`ARM64_BTI_KERNEL`/pointer authentication need CPU features the A76
doesn't have (§6).

### Configuration, step 2: the full checker (2026-10-01)

The hand-picked 44 above were the start. The full
[kernel-hardening-checker](https://github.com/a13xp0p0v/kernel-hardening-checker)
(0.6.17.1) checks 282 kconfig and cmdline items. Over the 1.2.x kernel it
reported **175 OK / 107 FAIL**. A second fragment,
[`hardening-kspp.cfg`](../../yocto/meta-device-platform-verity/recipes-kernel/linux/files/hardening-kspp.cfg),
takes it to **230 OK / 52 FAIL**: KSPP's kconfig items go from 46/91 to
71/91, grsecurity's from 36/53 to 48/53. Records:
[before](../../results/security/hardening/kernel-hardening-checker-6.12-before.txt),
[after](../../results/security/hardening/kernel-hardening-checker-6.12-after.txt).
**Booted on the board** through the A/B path (images 1.3.1/1.3.2,
2026-10-02): `/sys/kernel/security/lockdown` reads `[integrity]`, the LSMs
are `lockdown,capability,yama,landlock`, there are no failed units, and
M8's `custom_acq` tracepoints work under lockdown. The full M8 matrix ran
on this kernel. The cost question (`INIT_ON_FREE_DEFAULT_ON` is a memset
per free, plus the debug checks) was measured the only way that isolates
the kernel: the same card, slot A on the old kernel and slot B on the new
one, 3 × 10 000 raw samples each. The fraction of samples drained two to
a pass was A 15.0 / 22.9 / 28.1 % and B 27.6 / 27.9 / 27.4 %
([data and script](../../results/kernel-hardening-ab/)). B sits steadily
at A's upper end. That suggests a small cost, but A's spread covers B, so
it is **not established**. Sequence gaps were 0 in all six runs.

The decision that shaped the rest is **lockdown in integrity mode, not
confidentiality**. Both stop root from modifying the running kernel
(unsigned modules, `/dev/mem`, kexec, debugfs). Confidentiality also
closes tracefs, perf and kprobes, which would switch off M8's field
diagnostics (the `custom_acq` tracepoints that place a fault in its layer).
Tracing was cut down to what M8 uses instead. Static tracepoints and
`trace_marker` stay; the function tracer, kprobes, the stack tracer and
debugfs are gone. Those are the parts that can patch or probe arbitrary
kernel code.

As in step 1, every line of the fragment was checked against the
resulting `.config`. That caught five that had not taken effect:
`KPROBE_EVENTS` vanishes with `KPROBES`; `HARDENED_USERCOPY_DEFAULT_ON`
doesn't exist in 6.12; `LSM_MMAP_MIN_ADDR` is SELinux-only;
`ARM64_BTI_KERNEL` is `depends on !CC_IS_GCC` (GCC bug 106671); and
`IP_SCTP` stayed `=m` because the DLM selects it, and two cluster
filesystems select the DLM. Those three are off now, and the final check
reads 47 of 47 applied.

The 52 that remain, and why. (An earlier version of this table also
listed `FAIL_FUTEX` as "missed". It isn't a failure: the checker reports
`OK: is not found`. A `grep FAIL` over the report had matched the option's
*name*.)

| group | items | why they stay |
| --- | --- | --- |
| **Not possible here** | `CFI_CLANG`, `CFI_PERMISSIVE`, `SHADOW_CALL_STACK` (Clang only; this is a GCC build); `ARM64_BTI_KERNEL` (GCC); `KASAN_HW_TAGS` (MTE, Armv8.5), `ARM64_GCS` (Armv9.4); `ARM64_SW_TTBR0_PAN` (the A76 has hardware PAN, so the software emulation would add nothing); three `ARM_SMMU*` (no Arm SMMU on the BCM2712); `EFI_DISABLE_PCI_DMA`, `RESET_ATTACK_MITIGATION`, `efi=` (no EFI); `SCHED_CORE`, `nosmt` (the A76 has no SMT); `HARDENED_USERCOPY_DEFAULT_ON`, `LSM_MMAP_MIN_ADDR`, `LSM=*selinux*` (not in 6.12, or SELinux-only, and there is no SELinux policy); `hardened_usercopy=1` on the cmdline (already compiled in as the default) | — |
| **Kept, something needs them** | `MODULES`, `nomodule`, `TRIM_UNUSED_KSYMS` | the out-of-tree `custom_acq` driver and the WiFi stack are modules (all signed, `MODULE_SIG_FORCE`) |
| | `FTRACE`, `GENERIC_TRACER`, `KALLSYMS`, `LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY`, `lockdown=confidentiality` | M8's tracepoints (above) |
| | `BPF_SYSCALL` | systemd enforces `DevicePolicy=` (device-service's `/dev/acq0` allow-list) with a cgroup BPF program |
| | six `CRYPTO_USER_API*` | `libcryptsetup` checks ciphers through AF_ALG (its "Required kernel crypto interface not available" path), and `/data` is LUKS2 |
| | `STAGING` | the Pi's VideoCore drivers live in staging |
| | `FB`, `VT` | the HDMI console was how the secure-boot tryboot failure was diagnosed (§ secure-boot); a recovery channel worth keeping |
| | `CHECKPOINT_RESTORE`, `KCMP`, `RSEQ`, `PROC_PAGE_MONITOR` | used by systemd or glibc, or not yet examined; the cost of a wrong guess here is a broken boot |
| **Deferred to a board soak** | `UBSAN_BOUNDS`/`_LOCAL_BOUNDS`/`_TRAP`/`_SANITIZE_ALL`, `PAGE_TABLE_CHECK`(`_ENFORCED`), `RANDSTRUCT_FULL`, `STATIC_USERMODEHELPER`, `WERROR` | UBSAN trap and page-table checks *panic* on bugs in vendor drivers, which is the point but needs a soak test first; randstruct ties every module to the build's seed; a static usermode helper breaks `request_module`; `-Werror` on a vendor tree is a build-policy decision |
| | `page_alloc.shuffle`, `hash_pointers` (cmdline) | the cmdline is part of what tryboot switches; change it together with a boot test |

### Runtime settings

[`90-device-platform-hardening.conf`](../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/90-device-platform-hardening.conf)
(sysctl.d, applied by systemd at boot): `kptr_restrict=2`,
`dmesg_restrict=1`, unprivileged BPF off and JIT hardening, `kexec` and
SysRq off, no setuid core dumps, no line-discipline autoload, Yama
`ptrace_scope=1`, no unprivileged userfaultfd, and the network settings of
an end device (no redirects, no source routing, reverse-path filter).
Keys a kernel doesn't have are prefixed with `-`, so a missing option is
skipped instead of failing the unit.

### Kernel 6.12: the same triage after an upgrade

The conclusion above was "follow stable". meta-raspberrypi's scarthgap
branch also carries `linux-raspberrypi_6.12.bb` (6.12.93, the newer
long-term branch Raspberry Pi OS itself moved to); `device-platform-image-ab`
now builds it (`PREFERRED_VERSION_linux-raspberrypi = "6.12%"` in the
verity layer). Same NVD database and vulns.git checkout, same script
([6.6 record](../../results/security/cve/kernel-triage-summary.txt),
[6.12 record](../../results/security/cve/kernel-triage-summary-6.12.txt)):

| | 6.6.63 | 6.12.93 |
| --- | --- | --- |
| NVD version match only | 3,853 unpatched | 782 unpatched |
| after kernel CNA data + compiled-file filter | **3,602** | **1,789** |
| of those: fixed in a later release of the same branch | 1,102 (up to 6.6.157) | 1,093 (up to 6.12.111) |
| fixed only in newer branches | 323 | 243 |
| NVD-only, no fix data from the CNA | 2,177 | 453 |

- **Half the triaged total, from one line of configuration.** Most of the
  drop is the "NVD-only" column: old CVEs that NVD's version ranges
  attach to 6.6 and that the CNA never assigned a fix for.
- **For 6.12 the CNA data *adds* entries** (782 → 1,789). NVD enrichment
  of recent kernel CVEs lags behind; version ranges are missing, so a
  version match doesn't find them. The kernel CNA's records do. A report
  built from NVD alone would have looked better than 6.6 by a factor of
  five — and been wrong. The number to compare is the triaged one.
- **The same process gap remains:** 6.12.93 is 18 releases behind
  6.12.111, and 1,093 fixes are waiting in stable. An upgrade resets the
  counter; only following the stable branch keeps it down.
- The upgrade cost for this project was two lines: the driver's
  `<asm/unaligned.h>` moved to `<linux/unaligned.h>` in 6.12, and the RP1
  south-bridge driver became built-in, so its module package disappeared
  from the image's module list (the image build failed on it, not the
  device).

### On the board

Read back on the running 6.12 image ([record](../../results/security/update/on-target-signed-ab.txt)),
because configuration is a request, not a confirmation:

- **Every sysctl set as written**, except three keys that don't exist —
  and each absence meant something different:
  - `kexec_load_disabled`, `unprivileged_userfaultfd`: the features are
    compiled out (`KEXEC=n`, `USERFAULTFD=n`), which is better than a
    switch.
  - `kernel.yama.ptrace_scope`: Yama **was** compiled in, but
    `/sys/kernel/security/lsm` said only `capability`. Raspberry Pi's
    defconfig has `CONFIG_LSM=""`, and an empty list initialises no LSM
    at all — AppArmor, also built in, was just as inactive. Fixed with
    `CONFIG_LSM="yama"`, shipped as update 1.1.1 through A/B:
    `capability,yama`, `ptrace_scope = 1`. *Built in is not enabled.*
- **All modules signed, and accepted**: `custom_acq` taints only `O`
  (out-of-tree), not `E` (unsigned) — its signature from the build's key
  was accepted — so `MODULE_SIG_FORCE` went on in 1.2.0, and the driver
  still loads (taint `O` only). A module built without the kernel's
  build-time key can't be loaded any more, by anyone, `insmod` from
  `/tmp` included.
- **`FORTIFY_SOURCE` found something on the first boot**:
  `memcpy: detected field-spanning write (size 27) of single field
  "eventmask_msg->mask"` in the WiFi driver (`brcmfmac/fweh.c:466`). The
  struct declares `u8 mask[1]` — the old idiom for a variable-length
  tail — and the buffer was allocated with the full length, so no memory
  was overwritten; upstream fixes this pattern by converting such fields
  to flexible arrays. The check warns and lets it through (the `W` taint).
  It is exactly what the option is for: finding the copies whose size the
  compiler can't prove, in code nobody here wrote.
- `DEBUG_WX`: `Checked W+X mappings: passed, no W+X pages found`.
