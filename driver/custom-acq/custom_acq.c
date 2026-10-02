// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel driver for the custom STM32 acquisition peripheral
 * (v1-spi-slave-handshake firmware).
 *
 * V3 complete: probe + SPI register read/write (sysfs), a GPIO threaded
 * IRQ on DATA_READY that drains the MCU's hardware FIFO into a kernel
 * kfifo, and /dev/acq0 (misc device) exposing that kfifo to userspace via
 * open/read/poll/close (Plan.md "第四版").
 *
 * Wire protocol (matches v1-spi-slave-handshake/v1.3 firmware and
 * RaspPi/testv13.py): 5-byte frames, [cmd, data_be32]. A register read is
 * pipelined — the response to frame N is only valid in the reply to
 * frame N+1, so reading a register takes two back-to-back transfers: the
 * address frame, then a NOP frame to collect the echoed value.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/sysfs.h>
#include <linux/kernel.h>
#include <linux/version.h>
/* get_unaligned_be32() & co. moved from <asm/unaligned.h> to
 * <linux/unaligned.h> in 6.12, and the old header was removed.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#define CREATE_TRACE_POINTS
#include "custom_acq_trace.h"

#define REG_DEVICE_ID	0x00
#define REG_FW_VERSION	0x01
#define REG_CONTROL	0x03
#define REG_SAMPLE_RATE	0x04
#define REG_FIFO_LEVEL	0x05
#define REG_DATA_SEQ	0x06
#define REG_DATA_VAL	0x07
#define REG_SPI_REARM_FAIL	0x09
#define REG_SPI_ERROR_COUNT	0x0A
#define REG_STATUS	0x02
/* Device authentication, firmware v1.4+ (docs/security/device-authentication.md) */
#define REG_AUTH_NONCE0	0x10	/* ..0x13 */
#define REG_AUTH_CTRL	0x14
#define REG_AUTH_MAC0	0x15	/* ..0x18 */
#define REG_AUTH_CYCLES	0x19
#define ST_AUTH_BUSY	(1u << 4)
#define ST_AUTH_DONE	(1u << 5)
#define ST_AUTH_NOKEY	(1u << 6)
#define CMD_NOP		0x7F
#define CMD_WRITE_FLAG	0x80

/* Gap between the two frames of a pipelined register read/write, in
 * microseconds. A module parameter (not just a compile-time constant) so
 * a V7 experiment can try different values against real hardware without
 * a rebuild - see docs/debugging/case-06-*.md's "Next steps".
 *
 * Each sample costs 3 of these two-frame register reads (REG_FIFO_LEVEL +
 * REG_DATA_SEQ + REG_DATA_VAL), so this gap dominates per-sample latency.
 *
 * Was 100 (from a 2026-09-04 sweep on the Yocto card; before that 500,
 * and before that RaspPi/testv13.py's INTER_FRAME default). Changed to 50
 * on 2026-09-20 after sweeping it end to end on *both* cards, which is
 * what made the shape clear:
 *
 *   value   Yocto card                    Raspberry Pi OS card
 *   50      1000/s, 777us median          1001/s, 1220us median
 *   90      1000/s, 937us median          1070/s, 13290us median (!)
 *   100     1000/s, 977us median          686/s, kfifo overflowing
 *   150     998/s,  1226us median         (past the cliff)
 *   200     641/s,  16 hard IRQs in 18s   (past the cliff)
 *
 * Two reasons for 50 rather than 100:
 *
 * 1. The cliff is in a different place on each card - ~150-200us on the
 *    Yocto image, ~95us on Raspberry Pi OS - so 100 is comfortable on one
 *    and already past the edge on the other. Beyond the cliff the driver
 *    never catches up, the MCU's FIFO never empties, the threaded IRQ
 *    handler never returns, and every sample ends up sharing one stale
 *    irq_ts_ns (16 interrupts in 18 seconds at 200us). That is what
 *    docs/debugging/case-07 spent M1 chasing.
 * 2. Inside the clean zone this knob buys latency roughly linearly, about
 *    4us of median per 1us of gap. 50 is 200us/sample faster than 100 on
 *    the Yocto card, for free.
 *
 * 50 keeps ~3x margin to the nearer of the two cliffs, and the 2026-09-04
 * sweep found 0-50us clean as well. Still a measured data point, not a
 * hardware-verified safety margin: the earlier sweep saw occasional
 * single-digit sequence gaps in one 45s run at 100us, and nothing here
 * has been run for hours.
 */
static unsigned int inter_frame_us = 50;
module_param(inter_frame_us, uint, 0644);
MODULE_PARM_DESC(inter_frame_us,
		  "Gap (us) between the address and NOP frames of a register op");

/* Plan.md V2/M0: three ways to behave once the in-kernel kfifo can't keep
 * up with the MCU's production rate (docs/performance.md's "M0: Overload
 * Behavior" section has the real numbers this compares against):
 *
 * - "newest" (default, unchanged behavior): reject the incoming sample
 *   when the kfifo is full - kfifo_put() fails, priv->kfifo_overflow
 *   counts it. Keeps everything already queued; the newest data is what
 *   gets dropped. This is plain kfifo semantics, nothing added.
 * - "oldest": pop and discard the queue's oldest entry to make room,
 *   then always succeed the put. Keeps the most recent data at the cost
 *   of silently rewriting history a consumer may not have read yet.
 * - "downsample": deterministically keep 1 in every downsample_n drained
 *   samples and discard the rest *before* ever touching the kfifo,
 *   regardless of whether it's actually full. Unlike the other two
 *   (reactive, only kick in once the buffer is already under pressure),
 *   this proactively reduces the offered rate - trades guaranteed,
 *   evenly-spaced gaps for (hopefully) avoiding the reactive policies'
 *   bursty loss entirely.
 *
 * "newest"/"oldest" drops are counted in priv->kfifo_overflow (same
 * counter, same sysfs attribute, so anything already reading it for the
 * "newest" default keeps working); "downsample" drops go to the separate
 * priv->policy_dropped/policy_dropped sysfs attribute since they're a
 * deliberate design choice, not congestion.
 */
static char *drop_policy = "newest";
module_param(drop_policy, charp, 0644);
MODULE_PARM_DESC(drop_policy,
		  "kfifo-full policy: newest (reject new, default) | oldest (evict old) | downsample (keep 1 in downsample_n)");

static unsigned int downsample_n = 2;
module_param(downsample_n, uint, 0644);
MODULE_PARM_DESC(downsample_n,
		  "drop_policy=downsample only: keep 1 sample out of every N drained");

/* Plan.md V2/M8: fault injection. Each knob reproduces, on demand, one
 * failure this project has met or designed against, so the diagnostics
 * (the custom_acq tracepoints, device-service's supervisor and its fault
 * evidence) can be shown to locate it in the right layer. All 0 = off,
 * which is the default and the only sane production value. Root-only
 * (0644 on a root-owned sysfs file); every injection is itself traced
 * (custom_acq_fault) so a trace never shows an effect without its cause.
 *
 *   fault_drain_delay_us   extra delay per drained sample: a slow SPI
 *                          drain, i.e. inter_frame_us past the cliff
 *                          (case-07) without touching inter_frame_us
 *   fault_drop_every       drop every Nth drained sample in the driver:
 *                          lost frames, seen by userspace as seq gaps
 *   fault_spi_error_every  fail every Nth register read with -EIO: a
 *                          noisy or half-dead link
 *   fault_stall_ms         one shot: the next drain pass sleeps this long
 *                          before starting, then the knob resets to 0 - a
 *                          stuck drain, which is what the watchdog and the
 *                          supervisor's recovery exist for
 */
static unsigned int fault_drain_delay_us;
module_param(fault_drain_delay_us, uint, 0644);
MODULE_PARM_DESC(fault_drain_delay_us, "M8 fault injection: extra delay (us) per drained sample");

static unsigned int fault_drop_every;
module_param(fault_drop_every, uint, 0644);
MODULE_PARM_DESC(fault_drop_every, "M8 fault injection: drop every Nth drained sample (0 = off)");

static unsigned int fault_spi_error_every;
module_param(fault_spi_error_every, uint, 0644);
MODULE_PARM_DESC(fault_spi_error_every, "M8 fault injection: fail every Nth register read with -EIO (0 = off)");

static unsigned int fault_stall_ms;
module_param(fault_stall_ms, uint, 0644);
MODULE_PARM_DESC(fault_stall_ms, "M8 fault injection: one-shot stall (ms) of the next drain pass");

static atomic_t fault_reg_reads = ATOMIC_INIT(0);

enum custom_acq_drop_policy {
	DROP_POLICY_NEWEST = 0,
	DROP_POLICY_OLDEST,
	DROP_POLICY_DOWNSAMPLE,
};

static enum custom_acq_drop_policy custom_acq_get_drop_policy(void)
{
	/* sysfs_streq(), not strcmp(): a sysfs store (e.g. `echo oldest >
	 * .../drop_policy`) hands param_set_charp() the raw write buffer
	 * including its trailing '\n', which kstrdup() then preserves
	 * verbatim in drop_policy - a plain strcmp() against "oldest"
	 * (no newline) would never match. sysfs_streq() is the kernel's
	 * standard helper for exactly this: equal ignoring one trailing
	 * newline on either side.
	 */
	if (sysfs_streq(drop_policy, "oldest"))
		return DROP_POLICY_OLDEST;
	if (sysfs_streq(drop_policy, "downsample"))
		return DROP_POLICY_DOWNSAMPLE;
	return DROP_POLICY_NEWEST;
}

/* Must be a power of 2 (kfifo requirement). One IRQ can drain many MCU
 * FIFO entries at once (see custom_acq_irq_thread), so this needs enough
 * headroom that a burst doesn't overflow before /dev/acq0 exists to drain
 * it from userspace.
 */
#define SAMPLE_KFIFO_SIZE	128

/* Consecutive failed register reads a drain pass absorbs before it gives
 * up (custom_acq_irq_thread). One transient error must not end the pass:
 * see the comment there.
 */
#define DRAIN_MAX_ERRORS	3

/* irq_ts_ns: CLOCK_MONOTONIC-equivalent (ktime_get_ns()) timestamp of the
 * hard-IRQ that triggered this sample's drain (Plan.md V7 latency work).
 * Placed last so seq/value keep their existing 8-byte layout for anything
 * still assuming that; adds 8 bytes, still naturally aligned (no padding).
 * One hard-IRQ firing can drain many MCU FIFO entries in one thread pass
 * (see custom_acq_irq_thread) - all samples from the same drain share the
 * same irq_ts_ns, so this is "when we started reacting to this batch",
 * not a true per-sample production time. Good enough to bound
 * IRQ-to-userspace latency; does not cover MCU-sample-produced-to-GPIO-edge,
 * which needs a logic analyzer on a dedicated MCU pin (not wired up yet).
 */
struct custom_acq_sample {
	u32 seq;
	u32 value;
	s64 irq_ts_ns;
};

struct custom_acq {
	struct spi_device *spi;
	struct gpio_desc *data_ready;

	/* V7: optional spare Pi GPIO toggled in custom_acq_irq_hard(), for a
	 * logic analyzer to capture alongside the MCU's own sample-produced
	 * marker pin (v1-spi-slave-handshake/v1.3's PA9) - two edges on the
	 * analyzer's own single clock, no cross-clock-domain correlation
	 * needed to compute the MCU-to-hard-IRQ latency. NULL if the
	 * "irq-marker-gpios" DT property isn't present - existing overlays
	 * without it keep working unchanged.
	 */
	struct gpio_desc *irq_marker;
	bool irq_marker_state;

	/* Filled by custom_acq_irq_thread() (producer), drained by
	 * custom_acq_read() (consumer). A mutex, not a spinlock: the reader
	 * side uses kfifo_to_user(), which does copy_to_user() and can take
	 * a page fault (i.e. can sleep) — not legal while holding a
	 * spinlock. Both producer and consumer only ever run in process
	 * context (the IRQ thread is threaded, never hard-IRQ), so a mutex
	 * is safe here on both sides.
	 */
	DECLARE_KFIFO(samples, struct custom_acq_sample, SAMPLE_KFIFO_SIZE);
	struct mutex fifo_lock;
	u32 kfifo_overflow;

	/* drop_policy=downsample only: samples deliberately discarded before
	 * ever reaching the kfifo (not a kfifo_overflow - see drop_policy's
	 * comment above), and the running counter used to decide which 1-in-N
	 * sample to keep. Both only ever touched from custom_acq_irq_thread(),
	 * which never runs concurrently with itself for one priv, so no lock
	 * needed.
	 */
	u32 policy_dropped;
	u32 downsample_counter;
	wait_queue_head_t data_wq;	/* woken whenever the IRQ thread adds samples */
	struct miscdevice miscdev;	/* registers /dev/acq0 */

	/* Set by custom_acq_irq_hard() (hard-IRQ context, so this has to be a
	 * plain scalar, not something that needs locking) and read back by
	 * custom_acq_irq_thread() right after IRQ_WAKE_THREAD hands off - no
	 * lock needed since the two only ever run for the same IRQ activation,
	 * never concurrently for the same priv.
	 */
	s64 irq_ts_ns;

	/* Serializes each full reg_read/reg_write "logical operation" (its
	 * two SPI frames, address + NOP, back to back). Individual
	 * spi_sync_transfer() calls are already atomic at the controller
	 * level, but nothing stops the IRQ thread's SPI traffic from
	 * interleaving *between* our two frames without this — see
	 * docs/debugging/case-04-* for what that looked like.
	 */
	struct mutex spi_lock;
};

static int custom_acq_xfer(struct spi_device *spi, u8 cmd, u32 data, u8 *rx)
{
	u8 tx[5];
	struct spi_transfer t = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = 5,
	};

	tx[0] = cmd;
	tx[1] = (data >> 24) & 0xFF;
	tx[2] = (data >> 16) & 0xFF;
	tx[3] = (data >> 8) & 0xFF;
	tx[4] = data & 0xFF;

	return spi_sync_transfer(spi, &t, 1);
}

static int custom_acq_reg_read(struct spi_device *spi, u8 addr, u32 *val)
{
	struct custom_acq *priv = spi_get_drvdata(spi);
	u8 rx[5];
	int ret;

	mutex_lock(&priv->spi_lock);

	if (fault_spi_error_every &&
	    atomic_inc_return(&fault_reg_reads) % fault_spi_error_every == 0) {
		trace_custom_acq_fault("spi_error", addr);
		ret = -EIO;
		rx[0] = 0;
		goto out;
	}

	ret = custom_acq_xfer(spi, addr, 0, rx);
	if (ret)
		goto out;

	usleep_range(inter_frame_us, inter_frame_us + 100);

	ret = custom_acq_xfer(spi, CMD_NOP, 0, rx);
	if (ret)
		goto out;

	/* The echo check can't tell a dead bus from REG_DEVICE_ID: an MCU
	 * that isn't driving MISO reads as all zeros, and the echo of
	 * address 0x00 is 0x00. So device_id "succeeds" with 0x00000000 on a
	 * dead link while every other register fails here with -EIO - exactly
	 * the pair seen on the board on 2026-10-01. Userspace has to treat
	 * device_id == 0 as "no device" (device-service's startup probe and
	 * supervisor do); fixing it here would take a protocol change, e.g.
	 * echoing ~addr.
	 */
	if (rx[0] != addr) {
		dev_err(&spi->dev, "echo mismatch reading reg 0x%02x: got 0x%02x\n",
			addr, rx[0]);
		ret = -EIO;
		goto out;
	}

	*val = ((u32)rx[1] << 24) | ((u32)rx[2] << 16) | ((u32)rx[3] << 8) | rx[4];
out:
	if (ret)
		trace_custom_acq_spi_error(addr, rx[0], ret);
	mutex_unlock(&priv->spi_lock);
	return ret;
}

static int custom_acq_reg_write(struct spi_device *spi, u8 addr, u32 val)
{
	struct custom_acq *priv = spi_get_drvdata(spi);
	u8 cmd = addr | CMD_WRITE_FLAG;
	u8 rx[5];
	int ret;

	mutex_lock(&priv->spi_lock);

	ret = custom_acq_xfer(spi, cmd, val, rx);
	if (ret)
		goto out;

	usleep_range(inter_frame_us, inter_frame_us + 100);

	ret = custom_acq_xfer(spi, CMD_NOP, 0, rx);
	if (ret)
		goto out;

	if (rx[0] != cmd) {
		dev_err(&spi->dev, "echo mismatch writing reg 0x%02x: got 0x%02x\n",
			cmd, rx[0]);
		ret = -EIO;
		goto out;
	}

	ret = 0;
out:
	mutex_unlock(&priv->spi_lock);
	return ret;
}

/* Write 1/0 to start or stop acquisition (REG_CONTROL bit 0). Debug-only
 * knob for exercising DATA_READY end to end; the real control path is
 * /dev/acq0 once that lands.
 */
static ssize_t control_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;

	ret = custom_acq_reg_write(spi, REG_CONTROL, val & 0x01u);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(control);

/* REG_SAMPLE_RATE (Hz), MCU-firmware-validated range 1-10000
 * (v1-spi-slave-handshake/v1.3's main.c - out-of-range values are
 * rejected on the MCU side via ST_RANGE_ERR, not applied - checked here
 * too so a bad write fails loudly instead of silently no-op'ing).
 * Takes effect immediately if acquisition is already running (MCU
 * re-programs its sample timer live), matching Plan.md V2/M0's
 * backpressure design: userspace writes this down when it can't keep up,
 * no stop/restart required.
 */
static ssize_t sample_rate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = custom_acq_reg_read(spi, REG_SAMPLE_RATE, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t sample_rate_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;

	if (val < 1 || val > 10000)
		return -EINVAL;

	ret = custom_acq_reg_write(spi, REG_SAMPLE_RATE, val);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(sample_rate);

static ssize_t fifo_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = custom_acq_reg_read(spi, REG_FIFO_LEVEL, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(fifo_level);

/* Reading this pops one item off the MCU's FIFO (real REG_DATA_VAL
 * semantics) and re-evaluates DATA_READY on the MCU side as a side effect
 * — not idempotent, that's inherent to the hardware register, not a driver
 * bug.
 */
static ssize_t data_val_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = custom_acq_reg_read(spi, REG_DATA_VAL, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%08x\n", val);
}
static DEVICE_ATTR_RO(data_val);

static ssize_t kfifo_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct custom_acq *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", kfifo_len(&priv->samples));
}
static DEVICE_ATTR_RO(kfifo_level);

static ssize_t kfifo_overflow_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct custom_acq *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", priv->kfifo_overflow);
}
static DEVICE_ATTR_RO(kfifo_overflow);

/* Counts drops from drop_policy=oldest (evicted to make room) and
 * drop_policy=downsample (deliberately skipped) - see that module
 * param's comment. Stays 0 under the default "newest" policy, which
 * keeps counting its drops in kfifo_overflow instead, unchanged from
 * before this existed.
 */
static ssize_t policy_dropped_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct custom_acq *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", priv->policy_dropped);
}
static DEVICE_ATTR_RO(policy_dropped);

static ssize_t device_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = custom_acq_reg_read(spi, REG_DEVICE_ID, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%08x\n", val);
}
static DEVICE_ATTR_RO(device_id);

static ssize_t fw_version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = custom_acq_reg_read(spi, REG_FW_VERSION, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%08x\n", val);
}
static DEVICE_ATTR_RO(fw_version);

/* MCU-side diagnostic counters (Plan.md V7 / case-06): spi_rearm_fail
 * increments when the firmware's HAL_SPI_ErrorCallback()/TxRxCpltCallback()
 * fails to re-arm HAL_SPI_TransmitReceive_IT() for the next frame;
 * spi_error_count increments on every SPI-slave error the firmware sees.
 * Existed as MCU registers since early firmware versions but were never
 * read from this driver before - added to correlate their growth against
 * the RP1 SPI controller stall (docs/debugging/case-06-*.md's "Next
 * steps"), not because the driver itself does anything with these values.
 */
static ssize_t spi_rearm_fail_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = custom_acq_reg_read(spi, REG_SPI_REARM_FAIL, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(spi_rearm_fail);

static ssize_t spi_error_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = custom_acq_reg_read(spi, REG_SPI_ERROR_COUNT, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(spi_error_count);

/* Device authentication: userspace writes a 16-byte nonce as 32 hex digits
 * to auth_challenge, then reads the MCU's truncated HMAC from auth_response.
 * The driver only moves bytes; the key and the check live elsewhere (the MCU,
 * and whoever verifies the answer). Root only - a challenge and its response
 * are two separate sysfs operations, so concurrent callers would mix them up.
 */
static ssize_t auth_challenge_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	u8 nonce[16];
	int ret, i;

	if (count < 32 || hex2bin(nonce, buf, sizeof(nonce)))
		return -EINVAL;

	for (i = 0; i < 4; i++) {
		ret = custom_acq_reg_write(spi, REG_AUTH_NONCE0 + i, get_unaligned_be32(&nonce[4 * i]));
		if (ret)
			return ret;
	}
	ret = custom_acq_reg_write(spi, REG_AUTH_CTRL, 1);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(auth_challenge);

static ssize_t auth_response_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u8 mac[16];
	u32 status, word;
	int ret, i, tries;

	/* The MCU computes in its main loop; poll for up to ~100 ms. */
	for (tries = 0; tries < 100; tries++) {
		ret = custom_acq_reg_read(spi, REG_STATUS, &status);
		if (ret)
			return ret;
		if (status & ST_AUTH_DONE)
			break;
		usleep_range(1000, 1500);
	}
	if (!(status & ST_AUTH_DONE))
		return -ETIMEDOUT;
	if (status & ST_AUTH_NOKEY)
		return -ENOKEY;

	for (i = 0; i < 4; i++) {
		ret = custom_acq_reg_read(spi, REG_AUTH_MAC0 + i, &word);
		if (ret)
			return ret;
		put_unaligned_be32(word, &mac[4 * i]);
	}
	return sysfs_emit(buf, "%16phN\n", mac);
}
static DEVICE_ATTR_RO(auth_response);

static ssize_t auth_cycles_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	u32 val;
	int ret;

	ret = custom_acq_reg_read(spi, REG_AUTH_CYCLES, &val);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", val);
}
static DEVICE_ATTR_RO(auth_cycles);

static struct attribute *custom_acq_attrs[] = {
	&dev_attr_auth_challenge.attr,
	&dev_attr_auth_response.attr,
	&dev_attr_auth_cycles.attr,
	&dev_attr_device_id.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_control.attr,
	&dev_attr_sample_rate.attr,
	&dev_attr_fifo_level.attr,
	&dev_attr_data_val.attr,
	&dev_attr_kfifo_level.attr,
	&dev_attr_kfifo_overflow.attr,
	&dev_attr_policy_dropped.attr,
	&dev_attr_spi_rearm_fail.attr,
	&dev_attr_spi_error_count.attr,
	NULL,
};
ATTRIBUTE_GROUPS(custom_acq);

/* One item off the MCU's hardware FIFO: REG_DATA_SEQ peeks the head
 * item's sequence number (without removing it), REG_DATA_VAL then pops
 * it and returns the value — matches the firmware's reg_read() handling
 * of these two addresses (item_peeked latch) and RaspPi/testv13.py.
 */
static int custom_acq_read_sample(struct spi_device *spi, struct custom_acq_sample *s)
{
	int ret;

	ret = custom_acq_reg_read(spi, REG_DATA_SEQ, &s->seq);
	if (ret)
		return ret;

	return custom_acq_reg_read(spi, REG_DATA_VAL, &s->value);
}

/* Hard-IRQ top half: only job is to stamp "when did we first react to
 * this edge" as early as possible, before anything that can sleep (the
 * SPI drain below) has a chance to add scheduling jitter to the number.
 * Everything else - acknowledging DATA_READY means talking to the MCU
 * over SPI, which can sleep - stays in the threaded handler.
 */
static irqreturn_t custom_acq_irq_hard(int irq, void *data)
{
	struct custom_acq *priv = data;

	priv->irq_ts_ns = ktime_get_ns();
	trace_custom_acq_irq(priv->irq_ts_ns);

	if (priv->irq_marker) {
		priv->irq_marker_state = !priv->irq_marker_state;
		gpiod_set_value(priv->irq_marker, priv->irq_marker_state);
	}

	return IRQ_WAKE_THREAD;
}

/* DATA_READY is level-driven (high while the MCU's FIFO is non-empty,
 * see docs/debugging/case-03-data-ready-gpio-verification.md), and we
 * only get the rising edge once when it goes from empty to non-empty —
 * so one IRQ can mean "many samples arrived", not just one. Drain the
 * MCU's FIFO down to empty here rather than reading a single sample per
 * interrupt.
 */
static irqreturn_t custom_acq_irq_thread(int irq, void *data)
{
	struct custom_acq *priv = data;
	struct custom_acq_sample s;
	u32 level;
	int ret = 0;
	unsigned int drained = 0;
	unsigned int stall_ms;
	unsigned int errors = 0;
	const s64 pass_start = ktime_get_ns();
	s64 pass_end;

	stall_ms = xchg(&fault_stall_ms, 0);
	if (stall_ms) {
		trace_custom_acq_fault("stall", stall_ms);
		msleep(stall_ms);
	}

	/* Re-read REG_FIFO_LEVEL from the MCU on every iteration rather than
	 * snapshotting it once before the loop. DATA_READY is level-driven
	 * but the GPIO IRQ is edge-triggered (IRQF_TRIGGER_RISING) - we only
	 * get one rising edge for the whole time the MCU's FIFO stays
	 * non-empty. If the MCU keeps producing samples faster than we can
	 * drain them (each drained sample costs ~2 two-frame SPI ops here,
	 * ~2-3ms), a stale one-time level snapshot means we stop after that
	 * many samples while DATA_READY is still HIGH - no second edge ever
	 * arrives to re-trigger us, so the MCU's hardware FIFO fills and
	 * silently overflows for the rest of the run. Re-checking the real
	 * level keeps this thread draining for as long as data keeps
	 * arriving, exiting only once the MCU actually reports empty.
	 */
	/* A failed register read used to end the pass. That is fatal here,
	 * not cosmetic: the IRQ is edge-triggered and DATA_READY stays high
	 * while the MCU's FIFO is non-empty, so a pass that leaves data behind
	 * never gets another edge and acquisition stops until a watchdog
	 * soft-resets the MCU. Found by M8's fault injection on the board
	 * (2026-10-02): one injected -EIO in every 200 register reads stopped
	 * the pipeline for 3.9 s each time. A transient error is now retried;
	 * only DRAIN_MAX_ERRORS in a row give up, which leaves a really dead
	 * link to the watchdog and the supervisor's recovery, as before.
	 */
	for (;;) {
		ret = custom_acq_reg_read(priv->spi, REG_FIFO_LEVEL, &level);
		if (ret) {
			if (++errors < DRAIN_MAX_ERRORS)
				continue;
			dev_err(&priv->spi->dev, "IRQ: failed to read FIFO level: %d\n", ret);
			break;
		}
		if (level == 0)
			break;

		ret = custom_acq_read_sample(priv->spi, &s);
		if (ret) {
			if (++errors < DRAIN_MAX_ERRORS)
				continue;
			dev_err(&priv->spi->dev, "IRQ: failed to read sample: %d\n", ret);
			break;
		}
		errors = 0;
		s.irq_ts_ns = priv->irq_ts_ns;

		if (fault_drain_delay_us) {
			trace_custom_acq_fault("drain_delay", fault_drain_delay_us);
			usleep_range(fault_drain_delay_us, fault_drain_delay_us + 10);
		}
		if (fault_drop_every && (s.seq % fault_drop_every) == 0) {
			trace_custom_acq_fault("drop", s.seq);
			trace_custom_acq_sample(s.seq, ktime_get_ns() - s.irq_ts_ns,
						kfifo_len(&priv->samples), "injected_drop");
			drained++;
			continue;
		}

		if (custom_acq_get_drop_policy() == DROP_POLICY_DOWNSAMPLE) {
			priv->downsample_counter++;
			if (priv->downsample_counter % downsample_n != 0) {
				priv->policy_dropped++;
				trace_custom_acq_sample(s.seq, ktime_get_ns() - s.irq_ts_ns,
							kfifo_len(&priv->samples), "downsampled");
				drained++;
				continue;
			}
		}

		mutex_lock(&priv->fifo_lock);
		if (custom_acq_get_drop_policy() == DROP_POLICY_OLDEST && kfifo_is_full(&priv->samples)) {
			struct custom_acq_sample discard;

			kfifo_get(&priv->samples, &discard);
			priv->policy_dropped++;
			trace_custom_acq_sample(discard.seq, ktime_get_ns() - discard.irq_ts_ns,
						kfifo_len(&priv->samples), "evicted");
		}
		if (!kfifo_put(&priv->samples, s)) {
			priv->kfifo_overflow++;
			trace_custom_acq_sample(s.seq, ktime_get_ns() - s.irq_ts_ns,
						kfifo_len(&priv->samples), "overflow");
		} else {
			trace_custom_acq_sample(s.seq, ktime_get_ns() - s.irq_ts_ns,
						kfifo_len(&priv->samples), "queued");
		}
		mutex_unlock(&priv->fifo_lock);

		drained++;
	}

	pass_end = ktime_get_ns();
	trace_custom_acq_drain(drained, pass_end - pass_start, pass_end - priv->irq_ts_ns, ret);

	if (drained)
		wake_up_interruptible(&priv->data_wq);

	dev_dbg(&priv->spi->dev, "IRQ: DATA_READY fired, drained %u sample(s)\n", drained);
	return IRQ_HANDLED;
}

/* file->private_data is set to the struct miscdevice* before open() runs
 * (misc_open() in the misc-device core does this); swap it for our own
 * priv struct so every other file_operations callback can just read
 * file->private_data directly, same as the sysfs callbacks use dev.
 */
static int custom_acq_open(struct inode *inode, struct file *file)
{
	struct miscdevice *mdev = file->private_data;
	struct custom_acq *priv = container_of(mdev, struct custom_acq, miscdev);

	file->private_data = priv;
	return 0;
}

static int custom_acq_release(struct inode *inode, struct file *file)
{
	return 0;
}

/* Copies whole samples only — count is rounded down to a multiple of
 * sizeof(struct custom_acq_sample), matching kfifo's "record" is really
 * just a fixed-size element here, not the length-prefixed kfifo_rec
 * variant.
 */
static ssize_t custom_acq_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct custom_acq *priv = file->private_data;
	unsigned int copied;
	int ret;

	count -= count % sizeof(struct custom_acq_sample);
	if (count == 0)
		return -EINVAL;

	if (kfifo_is_empty(&priv->samples)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(priv->data_wq, !kfifo_is_empty(&priv->samples));
		if (ret)
			return ret;
	}

	mutex_lock(&priv->fifo_lock);
	ret = kfifo_to_user(&priv->samples, buf, count, &copied);
	trace_custom_acq_read(copied / sizeof(struct custom_acq_sample), kfifo_len(&priv->samples));
	mutex_unlock(&priv->fifo_lock);
	if (ret)
		return ret;

	return copied;
}

static __poll_t custom_acq_poll(struct file *file, poll_table *wait)
{
	struct custom_acq *priv = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &priv->data_wq, wait);
	if (!kfifo_is_empty(&priv->samples))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static const struct file_operations custom_acq_fops = {
	.owner = THIS_MODULE,
	.open = custom_acq_open,
	.release = custom_acq_release,
	.read = custom_acq_read,
	.poll = custom_acq_poll,
};

/* There is no devm_misc_register() in this kernel; wire misc_deregister()
 * up as a devres cleanup action ourselves so /dev/acq0 goes away
 * automatically on probe failure/unbind, same as the other devm_* resources
 * in probe().
 */
static void custom_acq_misc_deregister(void *data)
{
	misc_deregister(data);
}

static int custom_acq_probe(struct spi_device *spi)
{
	struct custom_acq *priv;
	u32 device_id;
	int ret;

	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
	spi_set_drvdata(spi, priv);
	INIT_KFIFO(priv->samples);
	mutex_init(&priv->fifo_lock);
	mutex_init(&priv->spi_lock);
	init_waitqueue_head(&priv->data_wq);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "spi_setup failed\n");

	ret = custom_acq_reg_read(spi, REG_DEVICE_ID, &device_id);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "failed to read DEVICE_ID\n");

	priv->data_ready = devm_gpiod_get(&spi->dev, "data-ready", GPIOD_IN);
	if (IS_ERR(priv->data_ready))
		return dev_err_probe(&spi->dev, PTR_ERR(priv->data_ready),
				      "failed to get data-ready gpio\n");

	priv->irq_marker = devm_gpiod_get_optional(&spi->dev, "irq-marker", GPIOD_OUT_LOW);
	if (IS_ERR(priv->irq_marker))
		return dev_err_probe(&spi->dev, PTR_ERR(priv->irq_marker),
				      "failed to get irq-marker gpio\n");

	ret = gpiod_to_irq(priv->data_ready);
	if (ret < 0)
		return dev_err_probe(&spi->dev, ret,
				      "failed to map data-ready gpio to irq\n");

	ret = devm_request_threaded_irq(&spi->dev, ret, custom_acq_irq_hard,
					 custom_acq_irq_thread,
					 IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					 "custom-acq", priv);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "failed to request IRQ\n");

	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.name = "acq0";
	priv->miscdev.fops = &custom_acq_fops;
	ret = misc_register(&priv->miscdev);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "failed to register /dev/acq0\n");

	ret = devm_add_action_or_reset(&spi->dev, custom_acq_misc_deregister, &priv->miscdev);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "failed to register /dev/acq0 cleanup\n");

	dev_info(&spi->dev, "custom-acq bound, DEVICE_ID=0x%08x\n", device_id);
	return 0;
}

static const struct of_device_id custom_acq_of_match[] = {
	{ .compatible = "edp,custom-acq" },
	{}
};
MODULE_DEVICE_TABLE(of, custom_acq_of_match);

static const struct spi_device_id custom_acq_spi_id[] = {
	{ "custom-acq", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, custom_acq_spi_id);

static struct spi_driver custom_acq_driver = {
	.driver = {
		.name = "custom-acq",
		.of_match_table = custom_acq_of_match,
		.dev_groups = custom_acq_groups,
	},
	.probe = custom_acq_probe,
	.id_table = custom_acq_spi_id,
};
module_spi_driver(custom_acq_driver);

MODULE_AUTHOR("Chengyi Zhou");
MODULE_DESCRIPTION("Driver for the custom STM32 SPI acquisition peripheral");
MODULE_LICENSE("GPL");
