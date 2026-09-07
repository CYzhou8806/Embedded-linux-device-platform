# Case 06: SPI Controller Stalls Under Sustained Load (Open — Root Cause Unresolved)

**Platform:** STM32F103VET6 (SPI slave) + Raspberry Pi 5 (RP1 SPI controller), `driver/custom-acq/custom_acq.c`
**Occurred:** V5, while writing the Python integration tests (`tests/integration/`) against sustained multi-second acquisition runs

Unlike the other cases in this directory, this one does **not** end with a fix — it ends with a documented, worked-around limitation and an open question for V7 (Plan.md's end-to-end performance analysis stage, which has the right tools — `ftrace`, a logic analyzer — for what's needed next). Recording it now because V5's whole point is to make instability like this visible and repeatable instead of something that only shows up once, gets shrugged off, and comes back later.

## Symptom 1: a genuine kernel-level stall

Reading continuously for more than a couple hundred samples occasionally wedges the Pi's SPI path entirely:

```
$ ps -eo pid,stat,comm | grep irq/185
   7250 D    irq/185-custom-acq
```

`D` = uninterruptible sleep. `sudo rmmod custom_acq` also hangs (it's waiting on the same in-flight `spi_sync_transfer()` inside the IRQ thread). `kfifo_level` sticks at 128 (full) and `kfifo_overflow` climbs continuously — the IRQ thread is stuck mid-transfer, not looping. Observed recovery behavior was inconsistent: sometimes it cleared on its own within seconds, other times it stayed wedged for 2+ minutes and only cleared after a **physical MCU reset** (not `tools/mcu-reset.sh`'s SWD reset — that was tried and did not help; only power-cycling/pressing the physical reset button worked).

## Symptom 2: throughput far below Case 05's measurement, and bursty rather than steady

Case 05 measured ~435 samples/sec with the C++ device service, sustained and even (2176 samples in ~5s, zero overflow). Re-measuring today with a plain Python reader (`tests/integration/acq_device.py`, no C++ involved) found something very different: long multi-second gaps with no new data, followed by a burst, rather than a steady stream:

```
backlog drained, level= 0
steady-state per-sample times (ms): [3003.04, 0.01, 0.0, 0.0, 0.0, 0.0, ...]
```

That is: once the buffered backlog was drained, the *next* sample took ~3 seconds to show up, then a whole burst arrived essentially instantly. This isn't a Python-speed artifact — a direct microbenchmark of the `select()` + `read()` pair against an already-non-empty kfifo showed sub-millisecond latency per call, every time. The bottleneck is upstream of the userspace read path entirely: either the MCU's own production, or the IRQ thread's drain loop, is pausing for seconds at a time before resuming. A short stress-test run (`tests/hardware/stress_test.py`, 20s) measured **11.4 samples/sec** average with `kfifo_overflow` rising by ~10,400 against only 275 samples delivered — an overflow-to-delivered ratio that doesn't square with "Python is just slow."

## What's been ruled out

- **`fifo_lock`/`spi_lock` mutex logic** — re-read `custom_acq_reg_read()`/`custom_acq_reg_write()`/`custom_acq_irq_thread()`/`custom_acq_read()` line by line. Every lock/unlock pair is correctly nested; there's no self-deadlock or double-acquire in this driver.
- **MCU firmware giving up under errors** — `HAL_SPI_ErrorCallback()` already increments `spi_error_count`, calls `spi_resync()`, and re-arms `HAL_SPI_TransmitReceive_IT()`; `HAL_SPI_TxRxCpltCallback()` re-arms after every completed frame. The firmware author already anticipated SPI-slave errors and built in recovery — there are even two diagnostic registers for this (`REG_SPI_REARM_FAIL` `0x09`, `REG_SPI_ERROR_COUNT` `0x0A`) that the driver has never read or exposed via sysfs. Reading those during a stall (not done yet — see Next steps) would confirm or rule out MCU-side rearm failure as a contributor.
- **Echo-mismatch errors causing the hang** — these are already handled gracefully (`custom_acq_reg_read()`/`write()` return `-EIO`, and `custom_acq_irq_thread()`'s loop breaks cleanly on that error). They show up in `dmesg` around the same time as stalls but don't by themselves explain a `D`-state block — an `-EIO` return doesn't sleep.

## What's not yet explained

Since SPI is master-driven (the Pi generates the clock), a `spi_sync_transfer()` call completing should not, in principle, depend on the slave (MCU) behaving correctly — the master's own clock generation finishing is what normally signals completion. A permanent `D`-state block therefore points at the **Raspberry Pi 5's RP1 SPI controller/driver** itself failing to signal transfer completion under sustained back-to-back traffic with the protocol's tight inter-frame gaps (originally `INTER_FRAME_US = 500`, now the `inter_frame_us` module param, default 100 - see below), rather than anything in this repo's driver or firmware code. **Update 2026-09-07**: confirmed via `ftrace` which driver this actually is - `spi_dw` (DesignWare SPI), not the `bcm2835_spi` this section originally speculated. Worth knowing if the next diagnostic step is reading that driver's source. That's a plausible working theory, not a confirmed root cause — it would need `ftrace`/SPI-layer tracing on the actual RP1 driver to confirm, which is V7-scoped tooling, not V5's.

The bursty multi-second-gap behavior (Symptom 2) may or may not be the same underlying issue as the full stall (Symptom 1) — possibly a lesser version of the same thing, possibly unrelated. Not established either way yet.

## What V5 does about it (mitigation, not a fix)

Since this can't be root-caused with V5's tools/scope, the integration tests and stress script are built to **detect and report** it rather than hang forever waiting for it to resolve:

- `tests/integration/acq_device.py`'s `AcqDevice.read_sample(timeout_s=...)` uses `select()` before each read and raises `AcqStall` instead of blocking indefinitely.
- Both `tests/integration/test_acquisition.py` and `tests/hardware/stress_test.py` use an 8-second stall timeout — long enough to ride out Symptom 2's observed multi-second bursty gaps without misreporting them as a stall, short enough to still catch Symptom 1 in a bounded test run.
- Tests are wall-clock-duration-bounded (not sample-count-bounded) and check an **error-rate ceiling**, not zero gaps/zero overflow — matching what Plan.md's V5 actually asks for ("验证序列 → 校验错误率" — verify the sequence, check the error rate) rather than assuming a perfect consumer is achievable.
- `stress_test.py` writes a report even on a detected stall, so a stall during an unattended long run leaves evidence instead of just a hung process.

## Update, 2026-09-07 (V7): 30-minute real-hardware run, no hard stall, but a related transient-error pattern found

Ran a 30-minute continuous acquisition against the real MCU (`inter_frame_us=100`, the current default), monitored over SSH every 60s for `D`-state processes and sysfs counter movement, with `ftrace` (`function_graph`, filtered) running the whole time. Full log: `results/ftrace/20260907-122231.txt` / `.monitor.log`.

**Prerequisite fix before this run was even meaningful**: the driver `.ko` actually loaded on the Pi turned out to be stale — missing `inter_frame_us` and the `spi_rearm_fail`/`spi_error_count` sysfs attributes entirely (`srcversion` didn't match the VM's build). The board had rebooted at some point since the 2026-09-04 work and picked the image-baked module from `/lib/modules/.../updates/` rather than the ad-hoc `insmod`'d copy from that session, which doesn't survive a reboot. Redeployed properly this time (rebuilt via full `bitbake custom-acq-driver`, replaced the `.xz` in `/lib/modules/.../updates/`, `depmod -a`, hot-swapped via `rmmod`/`insmod`) so it persists across future reboots too. Worth remembering: **"was it actually deployed" needs re-checking after any board reboot/power-cycle**, not just after a code change.

**Result — Symptom 1 (hard `D`-state stall): not reproduced.** Zero `D`-state processes across all 30 polls, `device-service` stayed `active` the entire run. This is a real data point, not just "didn't get around to it" — 30 minutes of continuous 1000/s real MCU traffic under the new `inter_frame_us=100` default did not trigger it. Doesn't rule it out (V5's own observations describe it as inconsistent, sometimes taking much longer to appear), but it's the longest clean run recorded against this driver version so far.

**But a related, lesser symptom did show up**: `kfifo_overflow` jumped twice during the run — 0→294 around the 2nd poll (~60-120s in), then 294→706 around the 17th poll (~17 minutes in) — then stayed flat for the rest of the run. `dmesg` shows these line up with clusters of the already-known `-EIO` echo-mismatch errors ("echo mismatch reading reg 0x05/0x06", "IRQ: failed to read sample/FIFO level"), same error class already noted in "What's been ruled out" above as *not* itself capable of causing a `D`-state block (an `-EIO` return doesn't sleep). `device-service`'s own `gap_count` reached 13 by the end — small, recoverable, but real data loss, and it happened in bursts rather than a steady trickle. This looks like Symptom 2's bursty-gap behavior, still present at the new `inter_frame_us` default, just far less severe than the pre-fix 2026-09-04 measurements (11.4 samples/sec / ~10,400 overflow in 20s). Not Symptom 1, but a reminder that Symptom 2 isn't fully gone either — just much rarer and smaller per occurrence.

**Methodology problem found, worth fixing before relying on ftrace evidence again**: `tests/hardware/case06_ftrace_spi.sh`'s filter (`grep -i spi` over `available_filter_functions`) is a substring match, and `spin_lock`/`spin_unlock`/`raw_spin_*` all contain the literal substring `"spi"` (`spin` starts with `spi`). The filter therefore also enabled tracing on every spinlock acquire/release kernel-wide — extremely high frequency — which very likely wrapped the `function_graph` ring buffer many times over before the actual error bursts (at ~60-120s and ~17min into a 30-minute run) ever happened. The saved trace file's content is dominated by generic `_raw_spin_lock`/`_raw_spin_unlock` noise and does **not** contain evidence from the actual error-burst moments — this run's `ftrace` capture didn't tell us anything new about root cause. Next attempt should anchor the filter more precisely (e.g. `grep -E '^spi_|_spi_sync|spi_transfer|bcm2835|rp1'`, word-boundary not substring) and/or only arm tracing reactively (e.g. triggered by a `D`-state detection) instead of for the whole run, so the buffer isn't wasted on unrelated noise for 29+ minutes before anything interesting happens.

## Update, 2026-09-07 (V7, continued): ftrace filter fixed and re-verified, but a second 30-min run caught neither the stall nor another overflow burst

Re-ran the same 30-minute monitor immediately after fixing the `grep -i spi` substring bug above, this time with the corrected word-anchored filter (verified on-target first: 263 matched functions, zero of them containing `spin`, includes `spi_sync`/`__spi_pump_transfer_message`/the controller driver's own functions — confirms the filter itself is now sound). Full log: `results/ftrace/20260907-125957.txt` / `.monitor.log`.

**Result**: another fully clean 30 minutes — no `D`-state, `device-service` stayed active, and this time **`kfifo_overflow` didn't move at all** (stayed at 706, the value carried over from the previous run's two bursts). So the echo-mismatch/overflow bursts found in the first run are confirmed genuinely intermittent, not a "happens once every ~15 minutes" pattern — two data points isn't enough to say more than that.

**But the fix is now confirmed working, which is real progress on its own**: the trace this time actually shows meaningful `spi_sync()` call graphs instead of spinlock noise, e.g.:

```
spi_sync() {
  __spi_sync() {
    __spi_validate() { ... }
    __spi_pump_transfer_message() {
      spi_transfer_one_message() {
        spi_set_cs() { dw_spi_set_cs [spi_dw](); ... }
        ...
        spi_finalize_current_message() { ... }
      }
    }
  }
}   <- total ~80us, consistently, across the whole capture
```

Two things learned from this even without catching an anomaly:
- **The real controller driver in use is `spi_dw` (DesignWare SPI), not `bcm2835_spi`** — worth correcting in "What's not yet explained" above, which only speculated "RP1's SPI controller" without naming the actual driver. Pi 5's RP1 I/O chip apparently uses a DesignWare SPI IP block rather than the classic BCM2835-style controller earlier Pis used.
- **Normal-path `spi_sync()` cost is a stable ~75-85us** at `inter_frame_us=100`, no outliers observed in this window — a clean baseline to compare a future captured stall/burst against.

**Net state of Case 06 after both 2026-09-07 runs**: two independent 30-minute real-hardware windows, zero hard stalls, one shows a transient overflow-burst pattern (unexplained, MCU-side ruled out), the other completely clean. The diagnostic tooling (redeployed driver, working sysfs counters, correctly-filtered ftrace) is now in good enough shape that the next stall or burst — whenever it happens — should actually be catchable with useful evidence, which wasn't true before today. Whether to keep running longer/repeated windows to chase the intermittent overflow bursts, or move on to other V7 items and treat Case 06 as "instrumented and waiting," is a judgment call, not something today's two runs settle on their own.

## Next steps (V7, not V5)

- ~~Expose `REG_SPI_REARM_FAIL`/`REG_SPI_ERROR_COUNT` as new sysfs attributes~~ — done (`spi_rearm_fail`/`spi_error_count`, `driver/custom-acq/custom_acq.c`). Correlated against a real burst event on 2026-09-07 (see Update above): both stayed at 0 across two `kfifo_overflow` bursts, so the MCU-side rearm-failure/error-count path is **not** what's causing the echo-mismatch bursts — narrows it back toward the Pi-side SPI controller/timing theory below. Still not correlated against an actual Symptom-1 `D`-state stall, since none has recurred yet.
- Trace the RP1 SPI controller driver directly (`ftrace`, `spi_sync` entry/exit) during a sustained run to see whether transfer completion itself is what's delayed. `tests/hardware/case06_ftrace_spi.sh` was written for this but assumes on-target `python3`/`bash`/`timeout`, none of which exist on this minimal Yocto image — actually run on 2026-09-07 as a VM-driven SSH-polling equivalent instead (see Update above). That run's filter had a substring bug (`grep -i spi` also matched `spin_lock`) that wasted the whole 30-minute capture on spinlock noise — **still need a real, correctly-filtered ftrace capture correlated with an actual error burst**, this hasn't been obtained yet.
- ~~Try lengthening `INTER_FRAME_US` as a cheap experiment~~ — done, with a real finding, though not the one this bullet expected. Swept `inter_frame_us` (now a runtime module parameter) across 500/300/250/200/150/100/50/20/10/0us on real hardware, 2026-09-04 (see `docs/session-log.md`'s fourth round that day for the full table). Relationship is *not* monotonic: 200-300us is a worse "valley" (more kfifo overflow/sequence gaps) than either 500us or anything at/below ~100us. Below ~100us throughput jumps from Case 05's ~520 samples/sec ceiling to ~1000/sec (matching the MCU's apparent production rate) with `kfifo_overflow`/`gap_count` mostly at 0. **Driver's compile-time default changed from 500 to 100us on the strength of this data.** This directly explains a large chunk of Case 05's throughput gap (per-sample cost was 3 register reads × the frame gap, not 1) - it does not, on its own, explain or reproduce Symptom 1's hard `D`-state stall, which still hasn't recurred under the new default. Worth revisiting whether a shorter gap changes Symptom 1's odds, but that needs an actual reproduction of the stall to test against, which still hasn't happened.
