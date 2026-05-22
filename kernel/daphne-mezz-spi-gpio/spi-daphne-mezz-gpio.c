// SPDX-License-Identifier: GPL-2.0
/*
 * DAPHNE mezzanine calibrated GPIO SPI master.
 *
 * This driver is intentionally narrow: it targets the DAPHNE PS MIO
 * mezzanine SPI-style pins and starts with SPI mode 0, 8-bit words.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/preempt.h>
#include <linux/spi/spi.h>

#define DAPHNE_DEFAULT_HIGH_DELAY_NS	250
#define DAPHNE_DEFAULT_LOW_DELAY_NS	250
#define DAPHNE_DEFAULT_SETUP_DELAY_NS	0
#define DAPHNE_DEFAULT_CRITICAL_BYTES	4

struct daphne_mezz_spi {
	struct device *dev;
	struct spi_controller *ctlr;
	struct gpio_desc *sck;
	struct gpio_desc *mosi;
	struct gpio_desc *miso;
	struct gpio_desc *cs;
	u32 sclk_high_delay_ns;
	u32 sclk_low_delay_ns;
	u32 mosi_setup_delay_ns;
	u32 critical_max_bytes;
	bool disable_preempt;
	bool disable_irqs;
	bool cs_active_high;
};

static void daphne_delay_ns(u32 nsecs)
{
	if (!nsecs)
		return;

	if (nsecs >= 1000)
		udelay(DIV_ROUND_UP(nsecs, 1000));
	else
		ndelay(nsecs);
}

static void daphne_set_cs(struct daphne_mezz_spi *d, bool active)
{
	int value;

	if (d->cs_active_high)
		value = active ? 1 : 0;
	else
		value = active ? 0 : 1;

	gpiod_set_raw_value(d->cs, value);
}

static void daphne_idle_lines(struct daphne_mezz_spi *d)
{
	gpiod_set_raw_value(d->sck, 0);
	if (d->mosi)
		gpiod_set_raw_value(d->mosi, 0);
	daphne_set_cs(d, false);
}

static int daphne_transfer_one(struct spi_controller *ctlr,
			       struct spi_device *spi,
			       struct spi_transfer *xfer)
{
	struct daphne_mezz_spi *d = spi_controller_get_devdata(ctlr);
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	bool critical = xfer->len <= d->critical_max_bytes;
	unsigned long irq_flags = 0;
	unsigned int byte_index;

	if ((spi->mode & (SPI_CPOL | SPI_CPHA)) != SPI_MODE_0)
		return -EOPNOTSUPP;

	if (xfer->bits_per_word && xfer->bits_per_word != 8)
		return -EOPNOTSUPP;

	if (critical && d->disable_preempt)
		preempt_disable();
	if (critical && d->disable_irqs)
		local_irq_save(irq_flags);

	daphne_set_cs(d, true);

	for (byte_index = 0; byte_index < xfer->len; byte_index++) {
		u8 tx_byte = tx ? tx[byte_index] : 0x00;
		u8 rx_byte = 0;
		int bit;

		for (bit = 7; bit >= 0; bit--) {
			if (d->mosi)
				gpiod_set_raw_value(d->mosi, !!(tx_byte & BIT(bit)));

			daphne_delay_ns(d->mosi_setup_delay_ns);
			daphne_delay_ns(d->sclk_low_delay_ns);

			gpiod_set_raw_value(d->sck, 1);
			daphne_delay_ns(d->sclk_high_delay_ns);

			if (d->miso && gpiod_get_raw_value(d->miso))
				rx_byte |= BIT(bit);

			gpiod_set_raw_value(d->sck, 0);
		}

		if (rx)
			rx[byte_index] = rx_byte;
	}

	daphne_set_cs(d, false);
	daphne_idle_lines(d);

	if (critical && d->disable_irqs)
		local_irq_restore(irq_flags);
	if (critical && d->disable_preempt)
		preempt_enable();

	return 0;
}

static int daphne_require_fast_gpio(struct device *dev, struct gpio_desc *desc,
				    const char *name)
{
	if (!desc)
		return 0;

	if (gpiod_cansleep(desc)) {
		dev_err(dev, "%s GPIO can sleep; low-jitter mode requires fast GPIO\n",
			name);
		return -EOPNOTSUPP;
	}

	return 0;
}

static int daphne_mezz_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct daphne_mezz_spi *d;
	u32 value;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*d));
	if (!ctlr)
		return -ENOMEM;

	d = spi_controller_get_devdata(ctlr);
	d->dev = dev;
	d->ctlr = ctlr;

	d->sclk_high_delay_ns = DAPHNE_DEFAULT_HIGH_DELAY_NS;
	d->sclk_low_delay_ns = DAPHNE_DEFAULT_LOW_DELAY_NS;
	d->mosi_setup_delay_ns = DAPHNE_DEFAULT_SETUP_DELAY_NS;
	d->critical_max_bytes = DAPHNE_DEFAULT_CRITICAL_BYTES;
	d->disable_preempt = of_property_read_bool(dev->of_node,
						   "daphne,disable-preempt");
	d->disable_irqs = of_property_read_bool(dev->of_node,
						"daphne,disable-irqs");
	d->cs_active_high = of_property_read_bool(dev->of_node,
						  "daphne,cs-active-high");

	if (!of_property_read_u32(dev->of_node, "daphne,sclk-high-delay-ns", &value))
		d->sclk_high_delay_ns = value;
	if (!of_property_read_u32(dev->of_node, "daphne,sclk-low-delay-ns", &value))
		d->sclk_low_delay_ns = value;
	if (!of_property_read_u32(dev->of_node, "daphne,mosi-setup-delay-ns", &value))
		d->mosi_setup_delay_ns = value;
	if (!of_property_read_u32(dev->of_node, "daphne,critical-max-bytes", &value))
		d->critical_max_bytes = value;

	d->sck = devm_gpiod_get(dev, "sck", GPIOD_ASIS);
	if (IS_ERR(d->sck))
		return dev_err_probe(dev, PTR_ERR(d->sck), "failed to get SCK GPIO\n");

	d->mosi = devm_gpiod_get_optional(dev, "mosi", GPIOD_ASIS);
	if (IS_ERR(d->mosi))
		return dev_err_probe(dev, PTR_ERR(d->mosi), "failed to get MOSI GPIO\n");

	d->miso = devm_gpiod_get_optional(dev, "miso", GPIOD_ASIS);
	if (IS_ERR(d->miso))
		return dev_err_probe(dev, PTR_ERR(d->miso), "failed to get MISO GPIO\n");

	d->cs = devm_gpiod_get(dev, "cs", GPIOD_ASIS);
	if (IS_ERR(d->cs))
		return dev_err_probe(dev, PTR_ERR(d->cs), "failed to get CS GPIO\n");

	ret = daphne_require_fast_gpio(dev, d->sck, "SCK");
	if (ret)
		return ret;
	ret = daphne_require_fast_gpio(dev, d->mosi, "MOSI");
	if (ret)
		return ret;
	ret = daphne_require_fast_gpio(dev, d->miso, "MISO");
	if (ret)
		return ret;
	ret = daphne_require_fast_gpio(dev, d->cs, "CS");
	if (ret)
		return ret;

	ret = gpiod_direction_output_raw(d->sck, 0);
	if (ret)
		return dev_err_probe(dev, ret, "failed to drive SCK\n");
	if (d->mosi) {
		ret = gpiod_direction_output_raw(d->mosi, 0);
		if (ret)
			return dev_err_probe(dev, ret, "failed to drive MOSI\n");
	}
	if (d->miso) {
		ret = gpiod_direction_input(d->miso);
		if (ret)
			return dev_err_probe(dev, ret, "failed to set MISO input\n");
	}
	ret = gpiod_direction_output_raw(d->cs, d->cs_active_high ? 0 : 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to drive CS\n");

	daphne_idle_lines(d);

	ctlr->dev.of_node = dev->of_node;
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = 0;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->transfer_one = daphne_transfer_one;

	platform_set_drvdata(pdev, ctlr);

	ret = devm_spi_register_controller(dev, ctlr);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register SPI controller\n");

	dev_info(dev,
		 "DAPHNE mezz SPI GPIO driver active: high_delay=%u ns low_delay=%u ns setup=%u ns critical_max=%u preempt=%u irqs=%u\n",
		 d->sclk_high_delay_ns, d->sclk_low_delay_ns,
		 d->mosi_setup_delay_ns, d->critical_max_bytes,
		 d->disable_preempt, d->disable_irqs);

	return 0;
}

static const struct of_device_id daphne_mezz_spi_of_match[] = {
	{ .compatible = "daphne,mezz-spi-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, daphne_mezz_spi_of_match);

static struct platform_driver daphne_mezz_spi_driver = {
	.probe = daphne_mezz_spi_probe,
	.driver = {
		.name = "daphne-mezz-spi-gpio",
		.of_match_table = daphne_mezz_spi_of_match,
	},
};
module_platform_driver(daphne_mezz_spi_driver);

MODULE_DESCRIPTION("DAPHNE calibrated mezzanine GPIO SPI master");
MODULE_AUTHOR("DAPHNE collaboration");
MODULE_LICENSE("GPL");
