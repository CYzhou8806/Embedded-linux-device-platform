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
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/wait.h>

#define REG_DEVICE_ID	0x00
#define REG_FW_VERSION	0x01
#define REG_CONTROL	0x03
#define REG_FIFO_LEVEL	0x05
#define REG_DATA_SEQ	0x06
#define REG_DATA_VAL	0x07
#define CMD_NOP		0x7F
#define CMD_WRITE_FLAG	0x80

/* Must be a power of 2 (kfifo requirement). One IRQ can drain many MCU
 * FIFO entries at once (see custom_acq_irq_thread), so this needs enough
 * headroom that a burst doesn't overflow before /dev/acq0 exists to drain
 * it from userspace.
 */
#define SAMPLE_KFIFO_SIZE	128

struct custom_acq_sample {
	u32 seq;
	u32 value;
};

/* Gap between the two frames of a pipelined register read. Matches
 * RaspPi/testv13.py's INTER_FRAME; empirically the margin the firmware
 * needs to finish handling one frame's SPI ISR before the next one lands.
 */
#define INTER_FRAME_US	500

struct custom_acq {
	struct spi_device *spi;
	struct gpio_desc *data_ready;

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
	wait_queue_head_t data_wq;	/* woken whenever the IRQ thread adds samples */
	struct miscdevice miscdev;	/* registers /dev/acq0 */

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

	ret = custom_acq_xfer(spi, addr, 0, rx);
	if (ret)
		goto out;

	usleep_range(INTER_FRAME_US, INTER_FRAME_US + 100);

	ret = custom_acq_xfer(spi, CMD_NOP, 0, rx);
	if (ret)
		goto out;

	if (rx[0] != addr) {
		dev_err(&spi->dev, "echo mismatch reading reg 0x%02x: got 0x%02x\n",
			addr, rx[0]);
		ret = -EIO;
		goto out;
	}

	*val = ((u32)rx[1] << 24) | ((u32)rx[2] << 16) | ((u32)rx[3] << 8) | rx[4];
out:
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

	usleep_range(INTER_FRAME_US, INTER_FRAME_US + 100);

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

static struct attribute *custom_acq_attrs[] = {
	&dev_attr_device_id.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_control.attr,
	&dev_attr_fifo_level.attr,
	&dev_attr_data_val.attr,
	&dev_attr_kfifo_level.attr,
	&dev_attr_kfifo_overflow.attr,
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

/* Threaded handler only (no hard-IRQ handler) because acknowledging
 * DATA_READY means talking to the MCU over SPI, which can sleep — not
 * allowed in hard-IRQ context.
 *
 * DATA_READY is level-driven (high while the MCU's FIFO is non-empty,
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
	int ret;
	unsigned int drained = 0;

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
	for (;;) {
		ret = custom_acq_reg_read(priv->spi, REG_FIFO_LEVEL, &level);
		if (ret) {
			dev_err(&priv->spi->dev, "IRQ: failed to read FIFO level: %d\n", ret);
			break;
		}
		if (level == 0)
			break;

		ret = custom_acq_read_sample(priv->spi, &s);
		if (ret) {
			dev_err(&priv->spi->dev, "IRQ: failed to read sample: %d\n", ret);
			break;
		}

		mutex_lock(&priv->fifo_lock);
		if (!kfifo_put(&priv->samples, s))
			priv->kfifo_overflow++;
		mutex_unlock(&priv->fifo_lock);

		drained++;
	}

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

	ret = gpiod_to_irq(priv->data_ready);
	if (ret < 0)
		return dev_err_probe(&spi->dev, ret,
				      "failed to map data-ready gpio to irq\n");

	ret = devm_request_threaded_irq(&spi->dev, ret, NULL, custom_acq_irq_thread,
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
