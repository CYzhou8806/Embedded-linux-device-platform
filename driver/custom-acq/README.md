# custom-acq: SPI acquisition driver

Out-of-tree Linux driver for the STM32F103 acquisition peripheral. Binds
through the Device Tree overlay in [`device-tree/`](../../device-tree/),
exposes `/dev/acq0` (misc device, `read()` + `poll()`) and a set of sysfs
diagnostics.

Design and a line-by-line explanation (Chinese):
[`docs/notes/driver-code-walkthrough.zh.md`](../../docs/notes/driver-code-walkthrough.zh.md).

## Shape of it

- **Hard IRQ** on `DATA_READY` does one thing: timestamp with
  `ktime_get_ns()`. That timestamp is what every latency number in
  [`docs/performance.md`](../../docs/performance.md) is measured from.
- **Threaded IRQ** drains the MCU's hardware FIFO over SPI until the MCU
  reports it empty, pushing samples into a `kfifo`.
- **Overflow policy** is selectable (`drop_policy=newest|oldest|downsample`),
  with `policy_dropped` counting what it discarded.

## Module parameters

| Parameter | Meaning |
| --- | --- |
| `inter_frame_us` | Gap between SPI frames. The dominant term in per-sample cost, and therefore in the throughput ceiling. |
| `drop_policy` | `newest` (kfifo refuses) / `oldest` (overwrite) / `downsample`. Compared in [`docs/performance.md`](../../docs/performance.md). |
| `downsample_n` | Keep 1 of every N, for `drop_policy=downsample`. |

## sysfs

Under `/sys/class/misc/acq0/device/`: `device_id`, `fw_version`,
`control`, `sample_rate` (RW — the knob the backpressure controller
drives), `fifo_level`, `data_val`, `kfifo_level`, `kfifo_overflow`,
`policy_dropped`, `spi_rearm_fail`, `spi_error_count`.

`kfifo_overflow` and `spi_error_count` exist to be **cross-checked
against userspace-side numbers**: more than one wrong conclusion in this
project was caught by a counter disagreeing with a computed rate rather
than by the number looking implausible.

Device authentication (firmware v1.4+, root only,
[docs/security/device-authentication.md](../../docs/security/device-authentication.md)):
`auth_challenge` (write a 16-byte nonce as 32 hex digits), `auth_response`
(the MCU's truncated HMAC as hex; `ENOKEY` if the MCU has no key,
`ETIMEDOUT` if it doesn't answer), `auth_cycles` (Cortex-M3 cycles the
last MAC took). The driver only moves bytes; verifying the answer is the
caller's job.

Writing sysfs attributes on the BusyBox images: use
`sudo sh -c "printf VALUE > ATTR"`. `echo VALUE | sudo tee ATTR` was seen
to deliver a truncated value to the store function.

## Build

```bash
make                      # against /lib/modules/$(uname -r)/build
make KDIR=/path/to/kernel # against a specific tree (cross-build)
```

## Platform differences

Same source on both targets — see [`platforms/`](../../platforms/).

- **Yocto Scarthgap (the main line):** built by
  `yocto/meta-device-platform/recipes-kernel/custom-acq-driver` as part of
  the image. For iteration, the `.ko` can be `scp`'d and
  `rmmod`/`insmod`'d in place.
- **Raspberry Pi OS (the comparison card):** `make` natively against
  `raspberrypi-kernel-headers`. Verified to build cleanly against both
  the stock and the official `PREEMPT_RT` kernel there
  ([case 07](../../docs/debugging/case-07-preempt-rt-comparison-exposes-a-different-bottleneck.md)).

The driver behaves identically on both, but its *measured* behaviour does
not: on the Raspberry Pi OS card throughput tops out around 640-655
samples/s against ~1000/s on Yocto, and because the threaded IRQ drains
until the MCU FIFO is empty, a card that never catches up fires almost no
hard interrupts — which invalidates the hard-IRQ timestamp as a latency
reference there. Anyone re-running latency measurements should check
`/proc/interrupts` first.
