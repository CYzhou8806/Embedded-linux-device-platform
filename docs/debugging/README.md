# Debugging case studies

Fifteen case studies from building this platform (fourteen root-cause investigations and one verification), written up
as *how the cause was found*, not as a list of fixes. Each one follows
the same shape: what was observed, what the first hypothesis was, what
ruled it out, what the actual mechanism turned out to be, and what was
left unexplained.

Two of them were later corrected by re-running the same experiment on a
second machine. The corrections are appended to the originals rather
than replacing them, because how a wrong conclusion survived is part of
what the case is worth.

| # | Layer | The finding, in one line |
| --- | --- | --- |
| [01](case-01-nvic-priority-spi-collapse.md) | MCU firmware | SPI collapsed once a timer ran alongside it — an NVIC **priority group** setting, so the sample timer could preempt the SPI interrupt mid-frame. |
| [02](case-02-disable-irq-spi-overrun.md) | MCU firmware | Disabling interrupts to protect a FIFO caused SPI overrun and frame desync. The protection was more expensive than the race it prevented. |
| [03](case-03-data-ready-gpio-verification.md) | Wiring | How to verify a new GPIO signal path when **neither end** can be trusted yet — instrument first, then believe. |
| [04](case-04-spi-transaction-race-two-frame-protocol.md) | Kernel driver | Two callers interleaved *between* the two frames of one register operation. `spi_sync_transfer()` is atomic per transfer, which is not the same as atomic per protocol exchange. |
| [05](case-05-irq-thread-stale-fifo-level-snapshot.md) | Kernel driver | The IRQ drain loop exited after one sample because it re-used a `FIFO_LEVEL` value read before the drain began. |
| [06](case-06-spi-controller-stall-under-sustained-load.md) | SoC / driver | An intermittent controller stall under sustained load, ending in D-state. Diagnosed and instrumented; **not fully root-caused** — kept as an open case rather than quietly dropped. |
| [07](case-07-preempt-rt-comparison-exposes-a-different-bottleneck.md) | Kernel / scheduling | Set out to measure PREEMPT_RT, found the bottleneck was intrinsic SPI pacing instead, and the planned metric was invalid on that card. **Follow-up:** the ~650 samples/s that made it invalid was one module parameter one step past a cliff, not the card. |
| [08](case-08-busy-spin-subscriber-hit-by-rt-throttling.md) | Real-time Linux | The fastest configuration had the worst tail: **RT throttling** takes the CPU from all RT tasks for 50 ms of every second, which an isolated core does not need protecting from. |
| [09](case-09-preempt-rt-busy-spin-starves-rcu.md) | Real-time Linux | A busy-spinning FIFO task starved `rcuc/N` on PREEMPT_RT, and the `nohz_full` that should have prevented it had been silently ignored because the kernel was built without it. **Follow-up:** the starvation is real, but its 130 ms cost was `printk` to a 115200-baud console — a clean latency histogram does not mean a clean kernel. |
| [10](case-10-dm-verity-one-byte-on-the-card.md) | Security / Yocto | One byte changed on the card: the kernel rejected exactly the data block computed on the build host (75372). Before that, a partition label written by `wic` into the verified filesystem would have made the image fail its first boot — caught only by a byte comparison. |
| [11](case-11-ab-update-four-failures-invisible-on-the-build-host.md) | Security / systemd | Signed A/B updates failed three times on the board for reasons the build host couldn't show: a BusyBox applet missing after the package cut (SSH died with it), SSH depending on the new data partition, and an ordering cycle that made systemd silently delete the health check. Then a stale bundle became a real bad update — and the one-shot `tryboot` rolled it back. |
| [12](case-12-pseudo-inode-mismatch-kbuild-clean-in-workdir.md) | Yocto / Kbuild | A recurring pseudo "inode mismatch" in `do_package`, worked around for a week with `-c clean`: with `S = "${WORKDIR}"`, Kbuild's recursive `make clean` deleted the previous build's packaged `.ko.xz` files outside pseudo. |
| [13](case-13-dead-mcu-reads-device-id-zero.md) | Protocol / userspace | A dead MCU reads as `DEVICE_ID = 0x00000000` without an error, because the echo of a floating bus (0x00) matches register 0's address. The startup probe and the old watchdog trusted "the read succeeded"; the health check, comparing against the expected ID, didn't. |
| [14](case-14-update-rejected-by-its-own-health-check.md) | systemd / release | Update 1.3.0 booted but device-service never started: an ordering cycle made systemd delete its start job, and a drop-in used a BusyBox applet the image lacks — case 11's two failure modes again. The health check refused the slot; a release gate (`tools/check-rootfs-units.py`) now catches both offline before anything is signed. |
| [15](case-15-one-spi-error-stops-acquisition.md) | Kernel driver | M8's first board run: one injected SPI error every ~70 ms stopped acquisition for 3.9 s, because a drain pass that gave up left DATA_READY high and an edge-triggered IRQ never fired again. Retrying transient errors fixed it: 0 gaps, 7/7 scenarios located in the right layer. |

## Recurring lessons

The same few mistakes produced most of these, which is the more useful
takeaway than any individual fix:

- **Cross-check a number against an independent signal.** Cases 07 and
  09, and three separate errors during M0/M1, all looked plausible until
  a counter, a compiler or a second measurement disagreed.
- **Configuration is a request, not a confirmation.** Kernel command
  lines, module parameters and sysfs writes have all silently failed
  here. Read the effect back, not the setting.
- **A metric can be invalid rather than merely surprising.** Case 07's
  latency figures were not noisy, they were meaningless — the
  timestamps behind them had stopped advancing.
- **Measure the same thing twice, on two machines if possible.** Both
  follow-ups above exist only because a second platform was available.
