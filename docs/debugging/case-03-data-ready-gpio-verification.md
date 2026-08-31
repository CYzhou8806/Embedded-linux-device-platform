# Case 03: Verifying a New GPIO Signal Path Without Trusting Either End

**Platform:** STM32F103VET6 (SPI slave, DATA_READY on `PA8`) + Raspberry Pi 5 (`GPIO17`, physical pin 11)
**Occurred:** V3, wiring up DATA_READY ahead of GPIO threaded-IRQ work in `driver/custom-acq/custom_acq.c`

## Symptom

A new wire (MCU `PA8` → Pi `GPIO17`) had just been added — never tested before. The goal was simply "confirm the wire works" before writing interrupt-handling driver code around it. The first attempt at doing so produced nothing: watching the pin with `gpiomon` while triggering SPI reads from the Pi showed zero edge events, even though the reads themselves succeeded normally.

## Expected

A leftover debug line in the firmware's `handle_frame()` (`HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_8)`, added during an earlier debugging session, never removed) toggles `PA8` on every SPI frame. Since every register read is two frames (address + NOP), each `cat device_id` should have produced two edges. It produced none.

## The core problem: three independent variables, one ambiguous result

A "no signal" result here could mean any of three unrelated things, and the naive test couldn't distinguish them:

1. **The wire itself is broken** (bad crimp, wrong pin, wrong header row).
2. **The firmware isn't doing what the source implies** — the debug toggle line exists in the repo, but nothing confirms the currently-flashed `.elf` was actually built from that exact source.
3. **The test methodology is wrong** — e.g. a backgrounded `gpiomon` process silently dying before the trigger commands ran (which, separately, is exactly what happened in one attempt: a `nohup`'d process launched over SSH and disowned was killed anyway when the SSH session tore down, because `systemd-logind`'s `KillUserProcesses` reaps a user's processes when their login session ends — `nohup` alone doesn't survive that).

Debugging any one of these while the other two are still unknowns just produces more ambiguous results. The fix was to eliminate variables one at a time instead of guessing.

## Investigation

**Attempt 1 — indirect, via firmware debug code.** Watched `GPIO17` with `gpiomon` while issuing `cat device_id` a few times. Zero edges. Inconclusive per above — could not tell whether the wire, the firmware, or the test process was at fault.

**Attempt 2 — direct, bypass firmware entirely.** To remove the firmware as a variable, the plan was to physically inject a known voltage: detach the wire from `PA8`, touch the loose end to the MCU board's `3V3` pin by hand, and watch the Pi side for a level change. This is the right idea (decouple hardware continuity from software correctness) but the wrong execution — the header pins were too densely packed for a reliable finger touch, and while manipulating the board the network dropped and the power LED changed color, an unplanned side effect. `vcgencmd get_throttled` afterward returned `0x0` (no undervoltage or thermal event ever recorded), confirming no hardware damage, but the method itself was too failure-prone to repeat.

**Attempt 3 — direct, but low-risk.** Same idea (bypass firmware, inject a known voltage) with a safer mechanism: instead of a finger against a dense header, use a second jumper wire to bridge the loose end directly to the *Raspberry Pi's own* `3V3` rail (physical pin 1 or 17) — a spot with generous pin spacing, and one that removes the MCU board from the test path entirely. This is a strictly better version of Attempt 2's idea: same goal (test the wire + Pi GPIO input in isolation), lower physical risk, no ambiguity about which pin was touched.

**Attempt 4 — the one that actually shipped.** Rather than keep chasing a hand-verification method, the firmware was made to do real work instead of being bypassed: three new sysfs attributes (`control`, `fifo_level`, `data_val`) were added to the kernel driver so the Pi could genuinely start acquisition and drain the MCU's FIFO over SPI — the actual production code path, not the leftover debug toggle. This served two purposes at once: it exercised the wire under real conditions, and it was work V3 needed anyway (`/dev/acq0` will need the same register-write plumbing later).

Reflashing firmware for later steps required the SWD debugger, which failed to attach over USB/IP (`Device not found`). Checking `usbipd list` on the Windows host showed the debugger's USB `BUSID` had silently changed (`2-7` → `2-8`) without a reboot — apparently just from being replugged at some point. This is an environment quirk, not a code bug, and is now documented in `tools/debug-connect.sh`'s error message and `tools/README.md` so it doesn't need re-diagnosing.

## Root Cause

Two, once the real acquisition path was exercised instead of the debug toggle:

1. **The leftover debug toggle in `handle_frame()` was in fact still compiled into the running firmware**, and was the *only* thing driving `PA8` — it fired on every SPI frame regardless of FIFO state, which is not the semantics an interrupt-driven consumer needs (edge count would reflect "how many times we talked to the MCU," not "whether there's new data").
2. **The real logic was silently disabled**: `update_data_ready_gpio()` — which sets the pin based on whether the FIFO is empty — was commented out at the one call site that mattered, inside `fifo_push()`'s timer-ISR callback (`main.c`, `HAL_TIM_PeriodElapsedCallback`). So even without problem 1, the pin would never go high the instant data arrived — only when a register read happened to re-evaluate it afterward.

Neither of these was a wiring problem. Attempt 1's "zero edges" result was real (the debug toggle line, when it did run, produced edges too fast/paired to observe reliably in that particular test window — moot once it was removed), and Attempt 4 proved the physical path had been fine the entire time.

## Fix

`main.c`: removed the leftover `HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_8);` debug line from `handle_frame()`, and un-commented `update_data_ready_gpio();` in the TIM2 period-elapsed callback so the pin is re-evaluated the instant a sample is pushed, not just on start/stop/read.

## Verification

With the fix flashed (`build-flash.sh` → OpenOCD/SWD program+verify), a short test — start acquisition, briefly wait, stop, drain the FIFO completely — produced exactly one rising edge (when the first sample landed) and exactly one falling edge (when the FIFO was fully drained), watched live with `gpiomon`. This is the correct level-based semantics a threaded IRQ handler needs: the physical wire, the Pi-side GPIO read path, and the firmware's DATA_READY logic are now all confirmed working end to end, clearing the way for the actual interrupt-driven driver code (V3's second sub-milestone).
