# device-service (V4 Phase 1)

Minimal end-to-end C++ userspace service that consumes the kernel driver's
`/dev/acq0` (see `driver/custom-acq/`). This is Plan.md's V4 Phase 1: prove
the whole pipeline works, deferring Configuration/Logging/Metrics/
ErrorRecovery/systemd/unit tests to Phase 2.

## What it does

1. Opens `/dev/acq0`, reads `device_id`/`fw_version` from sysfs as a
   liveness check.
2. Writes `1` to the `control` sysfs attribute to start acquisition.
3. Runs a dedicated `AcquisitionWorker` thread that polls `/dev/acq0` and
   drains samples into a bounded `RingBuffer`, tracking sequence-number
   continuity (a gap means something was lost between the MCU and here).
4. The main thread pops from the buffer and prints samples.
5. On `SIGINT`/`SIGTERM`: stops acquisition, drains/joins the worker
   thread, and prints a shutdown report (`samples_read`, `gap_count`,
   `kfifo_overflow`).

See `docs/learning-qa.md` Q25–Q29 for how `/dev/acq0` itself works
(misc device, `file_operations`, wait queues), and
`docs/debugging/case-05-irq-thread-stale-fifo-level-snapshot.md` for a
driver-side bug this service's testing turned up and fixed.

## Build

```bash
bash build.sh [Debug|Release]
```

Requires g++ (C++20) and cmake; no extra dependencies for Phase 1 (Phase 2
will add spdlog/nlohmann-json/GoogleTest/libsystemd-dev, none installed on
the Pi yet as of this writing).

## Run

`/dev/acq0` and the sysfs `control` attribute are root-owned by default, so
this currently needs `sudo` (a udev rule to relax that is a Phase 2 item,
not done yet):

```bash
sudo ./build/Debug/device-service
```

Ctrl+C (or `SIGTERM`) shuts it down cleanly.

## Known limitations (Phase 1, by design — see the plan file / Plan.md V4)

- Sysfs/device paths are hardcoded (`/dev/acq0`,
  `/sys/bus/spi/devices/spi0.0/`) — Phase 2 makes this configurable.
- No reconnect/error-recovery logic: `/dev/acq0` itself gives userspace no
  explicit "MCU disconnected" signal (confirmed by reading the driver), so
  a real recovery strategy needs a liveness-timeout design, not something
  to improvise in Phase 1.
- Effective throughput is below the MCU's raw ~1kHz sample rate — the
  two-frame, echo-verified SPI protocol has real per-sample overhead. This
  is an honest, now-visible bottleneck (see Case 05), not something Phase
  1 tries to optimize; that's Plan.md V7's job.
