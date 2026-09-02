# device-service (V4)

C++ userspace service that consumes the kernel driver's `/dev/acq0` (see
`driver/custom-acq/`). Plan.md's V4: Device / RingBuffer /
AcquisitionWorker / Configuration / Logging / Metrics / ErrorRecovery /
systemd / GoogleTest, split across Phase 1 (the minimal end-to-end loop)
and Phase 2 (everything else).

## What it does

1. Loads `Config` (optional JSON file, see `config.example.json` — all
   fields have defaults, missing file or missing fields just fall back).
2. Opens `/dev/acq0`, reads `device_id`/`fw_version` from sysfs as a
   liveness check, notifies systemd it's ready (`sd_notify(READY=1)`,
   a no-op outside systemd).
3. Writes `1` to the `control` sysfs attribute to start acquisition.
4. `AcquisitionWorker` (its own thread) polls `/dev/acq0` and drains
   samples into a bounded `RingBuffer`, tracking sequence-number
   continuity via `SequenceTracker`.
5. `MetricsReporter` (its own thread) periodically logs a summary line
   (rate/samples/gaps/buffer occupancy/kfifo_overflow) and pings systemd's
   watchdog (`sd_notify(WATCHDOG=1)`) on the same heartbeat.
6. `Watchdog` (its own thread) watches for "no new sample in
   `liveness_timeout_ms`", probes the device (a real sysfs read), and
   attempts a soft recovery (`control` 0→1, which resets the MCU's own
   `seq_counter`/FIFO — see `docs/debugging/case-05-*.md`) if the probe
   still succeeds. If the probe itself fails, the SPI link is genuinely
   down; this logs an error pointing at `tools/mcu-reset.sh` rather than
   retrying blindly.
7. On `SIGINT`/`SIGTERM`: stops acquisition, drains/joins the worker,
   metrics, and watchdog threads, and prints a final shutdown report.

See `docs/learning-qa.md` Q25–Q29 for how `/dev/acq0` itself works, and
`CODE_WALKTHROUGH.zh.md` (Chinese) for a line-by-line walkthrough of this
directory's code.

## Build

```bash
bash build.sh [Debug|Release]
```

Requires g++ (C++20), cmake, and (installed via apt on the Pi):
`libspdlog-dev`, `nlohmann-json3-dev`, `libgtest-dev`, `libsystemd-dev`.

Unit tests build alongside the service (`BUILD_TESTING` CMake option,
default `ON`) and run via `ctest` from the build directory:

```bash
cd build/Debug && ctest --output-on-failure
```

## Run standalone

`/dev/acq0` and the sysfs `control` attribute are root-owned by default
(a udev rule to relax that is a Phase 3+ item, not done):

```bash
sudo ./build/Debug/device-service [path/to/config.json]
```

Ctrl+C (or `SIGTERM`) shuts it down cleanly. With no config path argument
it runs entirely on defaults (`/dev/acq0`, `/sys/bus/spi/devices/spi0.0/`,
etc. — see `include/config.hpp`).

## Run under systemd

```bash
sudo mkdir -p /opt/device-service
sudo cp build/Release/device-service /opt/device-service/
sudo cp config.example.json /opt/device-service/config.json
sudo cp systemd/device-service.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now device-service
sudo systemctl status device-service
journalctl -u device-service -f
```

The unit is `Type=notify` with `WatchdogSec=15`: if the process hangs
(stops sending `WATCHDOG=1` pings, which piggyback on `MetricsReporter`'s
own tick) rather than cleanly exiting, systemd kills and restarts it
(`Restart=on-failure`). Verified on hardware: `systemctl start`/`stop`/
`restart` all clean, `journalctl` shows the spdlog output, status goes
`active (running)` only after the startup liveness check succeeds and
`sd_notify(READY=1)` fires.

## Known limitations (see the plan file for the fuller Phase-by-phase list)

- Effective throughput is below the MCU's raw ~1kHz sample rate — the
  two-frame, echo-verified SPI protocol has real per-sample overhead
  (see Case 05). An honest, now-visible bottleneck, not something this
  phase tries to optimize; that's Plan.md V7's job.
- `Watchdog`'s recovery is a *soft* reset only (rewriting `control` over
  SPI) — it cannot recover a link that's genuinely down (SPI echo
  mismatches / no response at all). That needs a real hardware reset
  (`tools/mcu-reset.sh`, over SWD) or a manual power-cycle; this service
  deliberately does not attempt either automatically (SWD needs a
  debugger connected, and a background service auto-triggering a
  hardware reset is a bigger blast radius than this phase wants).
- No unit tests for `AcquisitionWorker`/`MetricsReporter`/`Watchdog`
  themselves (they're thin orchestration around `Device`, which talks to
  real hardware) — the testable logic inside them (`SequenceTracker`,
  `RingBuffer`, `Config`) is covered instead.
