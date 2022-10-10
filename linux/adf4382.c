// SPDX-License-Identifier: GPL-2.0-only
/*
 * adf4382 driver
 *
 * Copyright 2022 Analog Devices Inc.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <linux/iio/iio.h>
#include <linux/regmap.h>

enum {
	ADF4382_FREQ,
};

struct adf4382_state {
	struct spi_device	*spi;
	struct regmap		*regmap;
	struct clk		*clkin;
	/* Protect against concurrent accesses to the device and data content */
	struct mutex		lock;
	struct notifier_block	nb;
};

static const struct regmap_config adf4382_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.read_flag_mask = BIT(7),
	.max_register = 0x54,
};

static int adf4382_reg_access(struct iio_dev *indio_dev,
			      unsigned int reg,
			      unsigned int write_val,
			      unsigned int *read_val)
{
	struct adf4382_state *st = iio_priv(indio_dev);

	if (read_val)
		return regmap_read(st->regmap, reg, read_val);

	return regmap_write(st->regmap, reg, write_val);
}

static const struct iio_info adf4382_info = {
	.debugfs_reg_access = &adf4382_reg_access,
};

int adf4382_set_freq(struct adf4382_state *st, u64 freq)
{
	return 0;
}

int adf4382_get_freq(struct adf4382_state *st, u64 *freq)
{
	return 0;
}

static ssize_t adf4382_write(struct iio_dev *indio_dev, uintptr_t private,
			     const struct iio_chan_spec *chan, const char *buf,
			     size_t len)
{
	struct adf4382_state *st = iio_priv(indio_dev);
	unsigned long long freq;
	int ret;

	mutex_lock(&st->lock);
	switch ((u32)private) {
	case ADF4382_FREQ:
		ret = kstrtoull(buf, 10, &freq);
		if (ret)
			break;

		ret = adf4382_set_freq(st, freq);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&st->lock);

	return ret ? ret : len;
}

static ssize_t adf4382_read(struct iio_dev *indio_dev, uintptr_t private,
			    const struct iio_chan_spec *chan, char *buf)
{
	struct adf4382_state *st = iio_priv(indio_dev);
	u64 val = 0;
	int ret;

	switch ((u32)private) {
	case ADF4382_FREQ:
		ret = adf4382_get_freq(st, &val);
		break;
	default:
		ret = -EINVAL;
		val = 0;
		break;
	}

	return ret ?: sysfs_emit(buf, "%llu\n", val);
}

#define _ADF4382_EXT_INFO(_name, _shared, _ident) { \
		.name = _name, \
		.read = adf4382_read, \
		.write = adf4382_write, \
		.private = _ident, \
		.shared = _shared, \
	}

static const struct iio_chan_spec_ext_info adf4382_ext_info[] = {
	/*
	 * Usually we use IIO_CHAN_INFO_FREQUENCY, but there are
	 * values > 2^32 in order to support the entire frequency range
	 * in Hz.
	 */
	_ADF4382_EXT_INFO("frequency", IIO_SHARED_BY_ALL, ADF4382_FREQ),
	{ },
};

static const struct iio_chan_spec adf4382_channels[] = {
	{
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = 0,
		.ext_info = adf4382_ext_info,
	},
};

static int adf4382_init(struct adf4382_state *st)
{
	return 0;
}

static int adf4382_freq_change(struct notifier_block *nb, unsigned long action, void *data)
{
	struct adf4382_state *st = container_of(nb, struct adf4382_state, nb);
	int ret;

	if (action == POST_RATE_CHANGE) {
		mutex_lock(&st->lock);
		ret = notifier_from_errno(adf4382_init(st));
		mutex_unlock(&st->lock);
		return ret;
	}

	return NOTIFY_OK;
}

static void adf4382_clk_disable(void *data)
{
	clk_disable_unprepare(data);
}

static void adf4382_clk_notifier_unreg(void *data)
{
	struct adf4382_state *st = data;

	clk_notifier_unregister(st->clkin, &st->nb);
}

static int adf4382_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct regmap *regmap;
	struct adf4382_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	regmap = devm_regmap_init_spi(spi, &adf4382_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	st = iio_priv(indio_dev);

	indio_dev->info = &adf4382_info;
	indio_dev->name = "adf4382";
	indio_dev->channels = adf4382_channels;
	indio_dev->num_channels = ARRAY_SIZE(adf4382_channels);

	st->regmap = regmap;
	st->spi = spi;

	mutex_init(&st->lock);

	ret = clk_prepare_enable(st->clkin);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&spi->dev, adf4382_clk_disable, st->clkin);
	if (ret)
		return ret;

	st->nb.notifier_call = adf4382_freq_change;
	ret = clk_notifier_register(st->clkin, &st->nb);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&spi->dev, adf4382_clk_notifier_unreg, st);
	if (ret)
		return ret;

	ret = adf4382_init(st);
	if (ret) {
		dev_err(&spi->dev, "adf4382 init failed\n");
		return ret;
	}

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct spi_device_id adf4382_id[] = {
	{ "adf4382", 0 },
	{},
};
MODULE_DEVICE_TABLE(spi, adf4382_id);

static const struct of_device_id adf4382_of_match[] = {
	{ .compatible = "adi,adf4382" },
	{},
};
MODULE_DEVICE_TABLE(of, adf4382_of_match);

static struct spi_driver adf4382_driver = {
	.driver = {
		.name = "adf4382",
		.of_match_table = adf4382_of_match,
	},
	.probe = adf4382_probe,
	.id_table = adf4382_id,
};
module_spi_driver(adf4382_driver);

MODULE_AUTHOR("Antoniu Miclaus <antoniu.miclaus@analog.com>");
MODULE_DESCRIPTION("Analog Devices ADF4382");
MODULE_LICENSE("GPL v2");