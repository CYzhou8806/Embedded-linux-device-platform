# devbus — zero-copy in-device middleware

Status: **v0.1** (2026-09). Tested on x86-64 and on the Raspberry Pi 5
(aarch64, the weaker memory model is where lock-free bugs show up): 15
tests, clean under ASan+UBSan and TSan, 30 consecutive passes on the Pi.
Measured on the Pi on three kernels, including a self-built
**PREEMPT_RT** kernel (see [Experiments](#experiments)). `acq-bridge`
connects it to the real acquisition driver; `device-service` itself is
not ported onto it yet.

devbus moves data between processes **on one embedded Linux device**:
acquisition, recording, statistics, algorithms, orchestration. A publisher
writes each sample once, directly into shared memory. Every subscriber
reads that same memory in place. Only 4-byte chunk indices travel through
the queues, so latency doesn't grow with payload size (measured: a 4 MiB
payload arrives as fast as a 64-byte one).

It is modeled on [Eclipse iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2)
(loan/send API, owner-driven reclaim, per-subscriber queues, no broker).
It also carries over lessons from two years on a production market-data
middleware, where the interesting failures were all about what happens
when one consumer falls behind.

## Why a device needs this, and what that implies

| Device requirement | devbus design choice |
| --- | --- |
| Big payloads (frames, sample blocks) at high rate | True zero copy: `loan()` hands out shared memory, `send()` publishes an index |
| Bounded, predictable latency | No allocation after setup, no locks on the data path, pre-faulted and optionally `mlock`ed memory, no syscall unless a subscriber is actually asleep |
| Fixed RAM budget | Every buffer sized once at service creation; the chunk budget is computed so `loan()` can't run out under correct use |
| Consumers with different needs | Overflow policy **per subscriber**: `DropOldest`, `DropNewest`, or `Block` with a timeout |
| A slow consumer must not stall the critical path | Per-subscriber queues; `Block` subscribers delivered last; blocking is time-bounded |
| Loss must be visible | Drops counted twice, independently: publisher counters in shared memory and subscriber-side sequence gaps. Tests assert they agree |
| Processes crash | Subscriber crash: its chunks are reclaimed through a pidfd. Publisher crash: the next publisher replaces the stale segment |
| Field debugging | `devbus-ls` reads every service's queue depths and drop counters live from shared memory |
| No broker to babysit | Decentralized: the publisher creates the service, subscribers attach to it by name |

## Architecture

```
publisher process          /dev/shm/devbus.<service>.data  (0640: subscribers map it read-only)       subscriber process
                     ┌───────────────────────────────────────────────────────────┐
 loan() ───────────► │ chunk pool  [hdr|payload][hdr|payload][hdr|payload] ...   │ ◄── read in place (PROT_READ)
   write in place    └───────────────────────────────────────────────────────────┘
                           /dev/shm/devbus.<service>       (0660: everyone read-write)
                     ┌───────────────────────────────────────────────────────────┐
 send(chunk) ──────► │ slot 0: data ring   (publisher → subscriber, chunk idx)   │ ──► receive()
                     │         done ring   (subscriber → publisher, chunk idx)   │ ◄── ~Sample()
 reclaim ◄────────── │ slot 1: ...                                               │
                     └───────────────────────────────────────────────────────────┘
 private: free list, per-chunk refcounts, per-subscriber outstanding counts, pidfds
```

- **Two segments per service**, laid out once from `ServiceConfig`. They hold
  only offsets and indices (never pointers), because every process maps
  them at a different address. Payloads live in their own segment, which
  only the publisher can write. Subscribers map it `PROT_READ`, so one
  compromised subscriber can't rewrite what the others are reading
  (layout version 2; threat model F13). Everything subscribers do have to
  write (ring indices, wake-up flags) is in the control segment, and the
  publisher treats all of it as untrusted.
- **Two lock-free queues per publisher→subscriber pair.** The data ring
  carries "new sample in chunk *i*". The done ring carries "I'm finished
  with chunk *i*". Each ring has a single producer.
- **Owner-driven reclaim.** Only the publisher frees chunks. Refcounts live
  in its private memory, where a misbehaving subscriber can't corrupt
  them. Indices read back from a done ring are validated, and bogus ones
  are ignored.
- **DropOldest without locks.** When a ring is full, the publisher and the
  subscriber race for the oldest entry with a CAS on the ring's read
  index. Whoever wins owns it, and there's no ABA because the index is a
  64-bit counter that never wraps.
- **Wakeups.** A subscriber spins, yields, or sleeps on a process-shared
  futex in its slot. The publisher checks a `sleeping` flag and makes the
  `FUTEX_WAKE` syscall only when it is set. The handshake is a seq_cst
  Dekker pattern, explained in `src/core.cpp`.
- **Liveness through pidfds**, not `kill(pid, 0)`. A pidfd refers to one
  specific process, so a recycled PID can't be mistaken for a live
  subscriber.

## API

```cpp
struct Frame { int64_t ts; uint16_t pixels[640 * 480]; };   // trivially copyable

auto pub = devbus::Publisher<Frame>::create("camera/frames", {.queue_capacity = 4});
if (auto loan = pub.loan()) {
	capture_into((*loan)->pixels);        // produced directly in shared memory
	pub.send(std::move(*loan));
}

devbus::Subscriber<Frame> sub("camera/frames", {.overflow = devbus::Overflow::DropOldest,
                                               .wait_mode = devbus::WaitMode::Futex});
while (sub.wait(std::chrono::milliseconds(100)))
	while (auto s = sub.receive())       // Sample<Frame>: read-only view, RAII-returned
		process(**s, s->seq(), s->publish_ns());
```

Payload types must be trivially copyable and standard-layout. A hash of
the type's name, plus its size and alignment, is checked when a subscriber
attaches, so a publisher and a subscriber built against different structs
fail loudly instead of reading garbage. That check catches a different
*name*, size or alignment — not two same-sized fields in a different
order, which is why this project's own payload lives in one shared header
(`include/devbus/acq_sample.hpp`) rather than being redeclared per
program.

### Back-pressure signal

```cpp
pub.max_queued();   // samples sitting in the deepest subscriber's queue
pub.pressure();     // the same as a fraction of queue_capacity, 0.0 .. 1.0
```

Both rise while every subscriber is still receiving everything and no
drop counter has moved — they are *leading* indicators, unlike the drop
counters, which by definition only move after data is lost. One acquire
load per active subscriber, no syscall, cheap enough to sample on every
send. `device-service` uses this to ease the MCU's rate down before the
pipeline starts dropping; see its `backpressure_max_devbus_pressure`.

## Record and replay

```bash
devbus-record acq/samples run.rec --seconds 60     # a Block subscriber writing to disk
devbus-replay run.rec --speed 1 --subscribers 1    # same type, same bytes, same timing
devbus-replay run.rec --service acq/test --speed 0 # renamed, as fast as consumers take it
```

Both are type-agnostic (`include/devbus/record.hpp`). The recorder reads
the payload type's identity (hash, size, alignment) from the live service
and stores it in the file. The replayer creates a service with exactly
that identity, so existing subscribers open a replay unchanged, and ones
built for another type are refused, as they would be for a live
publisher. The recorder uses `Block`, so a recording is complete unless
it reports gaps: if the disk falls behind, the publisher waits up to the
block timeout before dropping, and the drop shows up in the gap count.
Replay holds the recorded inter-sample timing to within tens of
microseconds (spin for the last stretch, sleep before it).

**Verified on the Pi** (2026-10-02, 6.12 hardened kernel): 5000 real
acquisition samples recorded from `acq-bridge` with 0 gaps, replayed under
a new name, and recorded again. The two recordings have **byte-identical
payloads** and the same type hash. Replay timing against the original:
median 0.1 µs, p99 6.4 µs, over 4.999 s each. Zero-copy after the F13
split, on the same board: 64 B p50 4.15–4.20 µs and 4 MiB 5.08–5.09 µs
over three runs, against 4.35 / 4.57 µs with the single-segment layout on
6.6 (another kernel, so only indicative). 4 MiB still costs what 64 B
does. Data: `results/devbus/pi5-yocto/6.12.93-hardened-layout-v2/`.

Verified on the dev host: 2001 samples recorded from the example
publisher at 1 kHz with 0 gaps, and replayed to the example subscriber
at 1000/s with 0 gaps (mean lateness 0 µs, max 20 µs). Payload bytes
come back unchanged, *including any timestamps inside them*. The example
subscriber measures latency from the payload's own timestamp, so on a
replay it reports the recording's age (~3 s in that test), not transport
latency. `Sample::publish_ns()` is the replay-time stamp to use instead.

## Build and run

On the Raspberry Pi 5 Yocto image there is no compiler; everything is
cross-built statically in a container (`experiments/rt-kernel/Containerfile`,
Ubuntu 24.04, same glibc as Scarthgap) and copied to `/tmp`.

The real data path, once the MCU is powered:

```sh
acq-bridge --prio 80 --cpu 2 &                 # /dev/acq0 -> devbus "acq/samples"
devbus-example-sub drop-oldest                 # any number of consumers
devbus-ls -w 1
```

`read()` lands each sample directly in a loaned shared-memory chunk, so the
kernel's `copy_to_user()` is the only copy between the driver and every
consumer.

On a development host:

```sh
cmake -S . -B build && cmake --build build     # BUILD_TESTING=OFF if GTest isn't installed
./build/devbus-tests
./build/devbus-example-pub 1000 &               # acq/samples at 1 kHz
./build/devbus-example-sub drop-oldest &        # fast consumer
./build/devbus-example-sub drop-newest 5000 &   # slow consumer: 5 ms per sample
./build/devbus-ls -w 1                          # watch queue depths and drop counters live
bash bench/run_all.sh ./build/devbus-bench      # all experiments -> results/devbus/<platform>/
```

Every `devbus-bench` command also takes the real-time knobs `--prio N`
(SCHED_FIFO), `--pub-cpu C` / `--sub-cpu C` (pinning), `--mlock 1` and
`--dma-latency 0` (hold `/dev/cpu_dma_latency`). `bench/run_rt_matrix.sh`
runs the whole kernel x tuning matrix on the target, and
`experiments/rt-kernel/run_on_pi.sh` boots each kernel through tryboot and
drives it.

## Platform differences

The source is identical everywhere; only how it is built and what it runs
under changes. See [`platforms/`](../../platforms/) for the full comparison.

| | Yocto Scarthgap (Pi 5) | Raspberry Pi OS (Pi 5) | Dev host |
| --- | --- | --- | --- |
| Build | cross-compiled statically in the container — the target has no compiler | the same static binaries run as-is, or build natively | native `cmake` |
| Deploy | `scp` to `/tmp`, re-copied after every reboot | ordinary filesystem | — |
| Results | `results/devbus/pi5-yocto/` | `results/devbus/pi5-raspios/` | `results/devbus/dev-host/` |

Two runtime details that are target-specific, not devbus-specific, and
that will silently skew any measurement if missed: the CPU governor lives
at `/sys/devices/system/cpu/cpufreq/policy0/` on the Pi 5 (the per-CPU
path returns `EIO`), and kernel command-line isolation flags are ignored
without the matching `CONFIG_`, so they must be read back from
`/sys/devices/system/cpu/` ([case 09](../../docs/debugging/case-09-preempt-rt-busy-spin-starves-rcu.md)).

## Experiments

See [`docs/devbus-experiments.md`](../../docs/devbus-experiments.md) for
the method, the tables, and what each result means.

## Limits of v0.1, on purpose

- One publisher per service. Many-to-one is modeled as several services.
- No history for late joiners, no request/response, no events or waitsets
  across services. iceoryx2 has all of these; add them when a consumer
  needs them.
- Same machine only. Crossing to another machine is a bridge process
  (e.g. Zenoh/MQTT), not a transport inside devbus.
- ~~`Block` waits by spinning~~ Since 2026-10-01 a `Block` publisher spins
  for 20 µs, then sleeps on a futex in the subscriber's slot, which the
  subscriber signals when it takes a sample out (only if the publisher is
  actually asleep, so the common case costs no syscall). A test holds the
  publisher back for ~200 ms behind a slow subscriber and checks it used
  under 20 % of a CPU meanwhile. A subscriber that never signals still
  only costs `block_timeout`.
- Refcounts are 16-bit, which caps a service at 65535 chunks.

## Roadmap

1. ~~Wire `device-service` to publish `acq/samples`, and feed the aggregate
   queue pressure into `BackpressureController` as the leading signal it
   lacks today~~ Done. `Publisher::pressure()` / `max_queued()` expose the
   deepest subscriber queue, `device-service`'s `devbus_service` config key
   turns publishing on, and its `BackpressureController` gained a
   `Congestion::Warning` level driven by that plus sample age. Measured
   justification: [`docs/devbus-experiments.md`](../../docs/devbus-experiments.md)'s
   `inter_frame_us` sweep, where latency climbed 11x while every drop
   counter still read zero.
2. Waitset: one thread waiting on several subscribers. Needs an
   eventfd-per-subscriber, handed over with `SCM_RIGHTS`.
3. Dynamic-size payloads (byte slices) for variable-size frames.
4. ~~Record/replay as an ordinary `Block` subscriber plus a replaying
   publisher~~ Done, see "Record and replay" above.
5. ~~Measure on the Raspberry Pi 5 under PREEMPT_RT and `SCHED_FIFO`~~ Done,
   see the experiments doc and debugging cases 08 and 09.
6. ~~`Block` waits by spinning. At `SCHED_FIFO` that ran into RT throttling
   (case 08). Wait on a futex in the subscriber slot instead.~~ Done (see
   "Limits" above). Not yet measured on the Pi under `SCHED_FIFO`.

**Fixed along the way (2026-10-01): an onboarding race.** A subscriber
claims a free slot and *then* writes its pid. A liveness check in that
window read pid 0, `process_alive(0)` said "dead", and the slot was
reclaimed under the subscriber. The subscriber then marked it `Active`
and never received a sample. With liveness checks every 1024 sends the
window was almost never hit. The replayer, which checks every
millisecond while waiting for subscribers, hit it about once in fifty
test runs. A slot that is `Claimed` with no pid is now "being
initialized" and is reclaimed only if it stays that way for a second.
`SlotBeingClaimedIsNotReapedAsDead` builds that state by hand, fails
without the fix and passes with it.
