# Backpressure acting on the leading signal, 2026-09-20

Hardware verification of `Congestion::Warning` (see
`userspace/device-service/include/backpressure_controller.hpp`): the
controller changing the MCU's rate *before* anything is lost, driven by
sample age rather than by `kfifo_overflow`.

Yocto Scarthgap card, stock 6.6.63 kernel, `inter_frame_us=100`, MCU
started at 1250 Hz — 10 Hz below the cliff at 1260 Hz.

| file | what |
| --- | --- |
| `w3.log` | the run this section is about: threshold 1300 us on the EWMA |
| `w2.log` | the earlier run with the window-peak statistic, which ratcheted 1250 -> 550 Hz for no reason |
| `obs.log` | last of the observation runs used to pick the threshold |

## Calibration first

Backpressure off, sweeping the MCU rate, reading the age EWMA:

| MCU rate | age EWMA | `kfifo_overflow` |
| --- | --- | --- |
| 1000 Hz | 973 us | 0 |
| 1100 Hz | 1 208 us | 0 |
| 1200 Hz | 1 449 us | 0 |
| 1250 Hz | 1 480 us | 0 |
| 1260 Hz | 8 777 300 us (collapsed) | +6 698 |

973 us at 1000 Hz is the same number `docs/performance.md` records for
this link, measured independently there - which is the check that this
statistic means what it is supposed to mean.

## The run

Threshold 1300 us, target 1250 Hz, step 100 Hz, window 500 ms, 25 s:

```
easing MCU 1250 -> 1150 Hz      (strain detected before any loss)
easing MCU 1150 -> 1050 Hz
ramping MCU back up 1050 -> 1150 Hz
easing MCU 1150 -> 1050 Hz
... alternating 1050 <-> 1150 for the rest of the run
```

Sustained 1 180 samples/s, age EWMA flat at ~1 200 us, and over the whole
run:

```
kfifo_overflow delta = 0
policy_dropped delta = 0
```

**The lagging signal never moved.** The controller found and held a
working point about 6% below the cliff without losing a sample. The
1050/1150 alternation is the designed settling behaviour - back-off and
ramp are the same size, so a link held at the edge oscillates between two
adjacent steps instead of sawtoothing across the range (pinned by
`WarningAndCleanWindowsSettleInsteadOfOscillating` in
`tests/test_backpressure_controller.cpp`).
