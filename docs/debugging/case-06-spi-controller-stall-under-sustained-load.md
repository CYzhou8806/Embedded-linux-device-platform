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

Since SPI is master-driven (the Pi generates the clock), a `spi_sync_transfer()` call completing should not, in principle, depend on the slave (MCU) behaving correctly — the master's own clock generation finishing is what normally signals completion. A permanent `D`-state block therefore points at the **Raspberry Pi 5's RP1 SPi controller/driver** itself failing to signal transfer completion under sustained back-to-back traffic with the protocol's tight inter-frame gaps (`INTER_FRAME_US = 500`), rather than anything in this repo's driver or firmware code. That's a plausible working theory, not a confirmed root cause — it would need `ftrace`/SPI-layer tracing on the actual RP1 driver to confirm, which is V7-scoped tooling, not V5's.

The bursty multi-second-gap behavior (Symptom 2) may or may not be the same underlying issue as the full stall (Symptom 1) — possibly a lesser version of the same thing, possibly unrelated. Not established either way yet.

## What V5 does about it (mitigation, not a fix)

Since this can't be root-caused with V5's tools/scope, the integration tests and stress script are built to **detect and report** it rather than hang forever waiting for it to resolve:

- `tests/integration/acq_device.py`'s `AcqDevice.read_sample(timeout_s=...)` uses `select()` before each read and raises `AcqStall` instead of blocking indefinitely.
- Both `tests/integration/test_acquisition.py` and `tests/hardware/stress_test.py` use an 8-second stall timeout — long enough to ride out Symptom 2's observed multi-second bursty gaps without misreporting them as a stall, short enough to still catch Symptom 1 in a bounded test run.
- Tests are wall-clock-duration-bounded (not sample-count-bounded) and check an **error-rate ceiling**, not zero gaps/zero overflow — matching what Plan.md's V5 actually asks for ("验证序列 → 校验错误率" — verify the sequence, check the error rate) rather than assuming a perfect consumer is achievable.
- `stress_test.py` writes a report even on a detected stall, so a stall during an unattended long run leaves evidence instead of just a hung process.

## Next steps (V7, not V5)

- Expose `REG_SPI_REARM_FAIL`/`REG_SPI_ERROR_COUNT` as new sysfs attributes and correlate their growth with stall/burst events.
- Trace the RP1 SPI controller driver directly (`ftrace`, `spi_sync` entry/exit) during a sustained run to see whether transfer completion itself is what's delayed.
- Try lengthening `INTER_FRAME_US` as a cheap experiment to see if the tight inter-frame gap is a contributing factor, independent of a full root-cause trace.
