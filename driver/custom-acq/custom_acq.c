// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel driver for the custom STM32 acquisition peripheral
 * (v1-spi-slave-handshake firmware).
 *
 * V3 first version: probe/remove + SPI register read, DEVICE_ID and
 * FW_VERSION exposed via sysfs. No FIFO/IRQ handling yet (see
 * Plan.md V3 milestone — GPIO threaded IRQ + kfifo + /dev/acq0 come later,
 * once DATA_READY is wired to a Pi GPIO).
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

#define REG_DEVICE_ID	0x00
#define REG_FW_VERSION	0x01
#define REG_CONTROL	0x03
#define REG_FIFO_LEVEL	0x05
#define REG_DATA_VAL	0x07
#define CMD_NOP		0x7F
#define CMD_WRITE_FLAG	0x80

/* Gap between the two frames of a pipelined register read. Matches
 * RaspPi/testv13.py's INTER_FRAME; empirically the margin the firmware
 * needs to finish handling one frame's SPI ISR before the next one lands.
 */
#define INTER_FRAME_US	500

struct custom_acq {
	struct spi_device *spi;
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
	u8 rx[5];
	int ret;

	ret = custom_acq_xfer(spi, addr, 0, rx);
	if (ret)
		return ret;

	usleep_range(INTER_FRAME_US, INTER_FRAME_US + 100);

	ret = custom_acq_xfer(spi, CMD_NOP, 0, rx);
	if (ret)
		return ret;

	if (rx[0] != addr) {
		dev_err(&spi->dev, "echo mismatch reading reg 0x%02x: got 0x%02x\n",
			addr, rx[0]);
		return -EIO;
	}

	*val = ((u32)rx[1] << 24) | ((u32)rx[2] << 16) | ((u32)rx[3] << 8) | rx[4];
	return 0;
}

static int custom_acq_reg_write(struct spi_device *spi, u8 addr, u32 val)
{
	u8 cmd = addr | CMD_WRITE_FLAG;
	u8 rx[5];
	int ret;

	ret = custom_acq_xfer(spi, cmd, val, rx);
	if (ret)
		return ret;

	usleep_range(INTER_FRAME_US, INTER_FRAME_US + 100);

	ret = custom_acq_xfer(spi, CMD_NOP, 0, rx);
	if (ret)
		return ret;

	if (rx[0] != cmd) {
		dev_err(&spi->dev, "echo mismatch writing reg 0x%02x: got 0x%02x\n",
			cmd, rx[0]);
		return -EIO;
	}

	return 0;
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
	NULL,
};
ATTRIBUTE_GROUPS(custom_acq);

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

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "spi_setup failed\n");

	ret = custom_acq_reg_read(spi, REG_DEVICE_ID, &device_id);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "failed to read DEVICE_ID\n");

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
