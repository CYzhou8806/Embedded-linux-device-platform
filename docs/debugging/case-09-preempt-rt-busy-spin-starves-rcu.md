# Case 09: On PREEMPT_RT, a Busy-Spinning Subscriber Starved the RCU Thread, and `nohz_full` Had Silently Done Nothing

**Platform:** Raspberry Pi 5 (Yocto Scarthgap image), custom `rpi-6.18.y` kernels built from one source with two configs, `userspace/devbus`, `bench/run_rt_matrix.sh`
**Occurred:** V2 (`Plan.md` §12.4, M1 + M4): the PREEMPT_RT comparison, right after [case 08](case-08-busy-spin-subscriber-hit-by-rt-throttling.md)

## Symptom

Same configuration as case 08: a busy-spinning devbus subscriber at
`SCHED_FIFO` 80 on isolated core 3, with `stress-ng` load on cores 0-1.
The two 6.18 builds used the same source and the same config apart from
the preemption model. They disagreed on the worst case:

| busy-spin, isolated core, 60 000 samples | p99.9 | max |
| --- | --- | --- |
| 6.18.52, `CONFIG_PREEMPT` | 0.28 µs | 1.06 µs |
| 6.18.52, `CONFIG_PREEMPT_RT` | 0.28 µs | **130 261 µs** |
| 6.18.52, `CONFIG_PREEMPT_RT`, RT throttling off | 0.28 µs | **129 592 µs** |

It was a single ~130 ms outlier, reproducible, and unaffected by RT
throttling. So this wasn't case 08 again.

## The kernel log had the answer

```
[    0.000000] Unknown kernel command line parameters "nohz_full=2,3 rcu_nocbs=2,3", will be passed to user space.
[    0.000000] rcu:     RCU_SOFTIRQ processing moved to rcuc kthreads.
...
[  452.605598] rcu: INFO: rcu_preempt self-detected stall on CPU
[  452.611368] rcu:     3-....: (5471 ticks this GP) ... rcuc=5252 jiffies(starved)
[  512.610597] rcu: INFO: rcu_preempt self-detected stall on CPU
[  512.616366] rcu:     3-....: (1 GPs behind) ... rcuc=5253 jiffies(starved)
```

Two separate problems stack up here:

1. **`nohz_full=2,3 rcu_nocbs=2,3` were silently ignored.** Raspberry Pi's
   `bcm2712_defconfig` doesn't enable `CONFIG_NO_HZ_FULL` or
   `CONFIG_RCU_NOCB_CPU`, and neither does the stock Yocto 6.6 kernel.
   The kernel doesn't reject unknown parameters. It passes them to init
   and carries on. `isolcpus` still worked, which is why the isolated
   core *looked* correctly configured (`/sys/devices/system/cpu/isolated`
   showed `2-3`). But core 3 still took the scheduler tick and still had
   to process its own RCU callbacks.
2. **On PREEMPT_RT, RCU callback processing moves out of softirq into a
   per-CPU kthread, `rcuc/N`**, running at `SCHED_FIFO` priority 1. A
   `SCHED_FIFO` 80 task that never sleeps never lets `rcuc/3` run. After
   ~21 s (5250 jiffies at HZ=250) the RCU stall detector fires, and
   handling the stall is what the spinner sees as a ~130 ms gap. On the
   non-RT build the same callbacks run on interrupt exit, which even a
   spinning task can't block, so the problem never shows up there.

## Fix

Rebuild both 6.18 kernels (same source, both preemption models, to keep
the comparison fair) with:

```
CONFIG_NO_HZ_FULL=y          # (replaces NO_HZ_IDLE) no tick on a CPU running a single task
CONFIG_RCU_NOCB_CPU=y        # rcu_nocbs= moves callback processing to rcuo threads on housekeeping CPUs
CONFIG_CONTEXT_TRACKING_USER=y
```

Then check that the parameters actually took effect, instead of trusting
the command line: `/sys/devices/system/cpu/nohz_full` must exist and read
`2-3`, and `dmesg` must not list them as unknown.

## Result

Both rebuilt kernels booted with `/sys/devices/system/cpu/nohz_full`
reading `2-3` and no "unknown parameter" line. The whole matrix was then
rerun on each of them:

| busy-spin, isolated core, 60 000 samples | max, first build | max, rebuilt (`-iso`) |
| --- | --- | --- |
| 6.18.52 `CONFIG_PREEMPT` | 1.06 µs | 0.83 µs |
| 6.18.52 `CONFIG_PREEMPT_RT` | **130 261 µs** | **1.37 µs** |
| 6.18.52 `CONFIG_PREEMPT_RT`, throttling off | **129 592 µs** | **0.83 µs** |

The rebuilt RT kernel's `dmesg` has no RCU stall after the full run
(~20 min of load, including both spinning configurations). The
first-build results and `dmesg` are kept in
`results/devbus/pi5-yocto/first-build-without-nohz_full/`.

## Takeaways

- **A kernel command line is a request, not a confirmation.** Unknown
  parameters are logged once at boot and then ignored. Every isolation
  knob needs a readback: `/sys/devices/system/cpu/{isolated,nohz_full}`
  and `dmesg | grep -i unknown`.
- **PREEMPT_RT turns some kernel work into ordinary schedulable threads.**
  That's why it bounds latency better, and it's also why a
  high-priority task can starve the kernel's own housekeeping. On RT,
  "pin a spinning FIFO task to an isolated core" is only safe once that
  core has been fully offloaded (`nohz_full`, `rcu_nocbs`, IRQ affinity).
- Cases 08 and 09 together: the same user-space configuration (spin,
  FIFO 80, isolated core) behaved **three different ways** on three
  kernels: 50 ms throttling stalls on 6.6, clean on 6.18 non-RT, and
  130 ms RCU stalls on 6.18 RT until the kernel config was fixed. A latency result is a property of the
  whole stack, not of the application. (The follow-up below revises what
  the 130 ms measured: the starvation was real, but the milliseconds were
  the console printing the stall report, not the stall.)

---

## Follow-up, 2026-09-20: the 130 ms was the console, not the stall

Rerunning this scenario on the project's second card — Raspberry Pi OS
with Raspberry Pi's **own** `PREEMPT_RT` kernel
(`6.18.39+rpt-rpi-v8-rt`, installed from `apt`) — split this case's
conclusion in two. One half held up exactly. The other was wrong.

That kernel is built without `CONFIG_NO_HZ_FULL` and
`CONFIG_RCU_NOCB_CPU`, exactly like the first build above, so
`nohz_full=2,3 rcu_nocbs=2,3` are ignored there too (same boot message,
same missing sysfs file). `rcuc/3` runs at `SCHED_FIFO` 1, and `dmesg`
confirms `RCU_SOFTIRQ processing moved to rcuc kthreads`. Every
precondition this case identified was present.

A 180-second busy-spin run at `SCHED_FIFO` 80 on isolated core 3, with
RT throttling disabled — the harshest row of the table above:

```
latency,devbus-loan,64,spin,1000,180000,p50=0.26,p99=0.85,p99.9=1.41,max=3.85 µs
```

```
[ 1046.755170] rcu: INFO: rcu_preempt self-detected stall on CPU
[ 1046.755185] rcu:  3-....: (5252 ticks this GP) ... rcuc=5294 jiffies(starved)
[ 1109.774056] rcu: INFO: rcu_preempt self-detected stall on CPU
[ 1172.792008] rcu: INFO: rcu_preempt self-detected stall on CPU
```

**The starvation reproduced three times. The latency did not move at
all** — 3.85 µs worst case over 180 s, against 130 261 µs here.

### Where the 130 ms actually went

The two stall reports contain the same thing (the same register dump of
the spinning `devbus-bench` thread). What differs is how long the kernel
took to *print* it:

| | first line → last line | per line |
| --- | --- | --- |
| Yocto card, this case | 452.605598 → 452.729254 = **123.7 ms** | **6.51 ms** |
| Pi OS card, follow-up | 1046.755170 → 1046.755215 = **0.045 ms** | **0.003 ms** |

A 20-line report at 6.51 ms per line is a serial console: 80 characters
× 10 bits ÷ 115200 baud = **6.94 ms per line**, and `printk` to the UART
blocks the CPU it runs on. 123.7 ms of printing, on the very core the
measurement was pinned to, against a 130.3 ms observed outlier — the
stall report *is* the outlier, with the rest of the gap being the one
further line and the sampling boundary.

So the causal chain in this case was one link too long:

- **Correct:** a busy-spinning `SCHED_FIFO` task starves `rcuc/N` on
  PREEMPT_RT when the core has not been offloaded, and the RCU stall
  detector fires. `rcu_nocbs` (with the `CONFIG_` actually enabled) is
  the right fix, and the readback discipline stands.
- **Wrong:** that the starvation is what cost 130 ms. It cost nothing
  measurable. The 130 ms was `printk` to a 115200-baud console, which
  would have cost the same had anything else printed 20 lines.

### What this changes

- **A slow console is a latency source in its own right.** Any kernel
  message printed to a serial console — a stall report, a WARN, a driver
  probe error — stops that CPU for milliseconds per line. On a latency
  target, that is worth knowing before it appears in a percentile and
  gets attributed to whatever the application was doing.
- **A clean latency histogram does not mean a clean kernel.** On the Pi
  OS card the RCU machinery was starving for three minutes and the
  application-level metric showed nothing. `dmesg` has to be read as
  part of the measurement, not only when a number looks wrong.
- The original comparison remains valid, because both 6.18 kernels there
  were built from one source and ran on the same card with the same
  console: whatever the printing cost, the non-RT build never paid it,
  because it never starved RCU in the first place.
