// SPDX-License-Identifier: GPL-2.0
/*
 * ADC1283 GPIO bit-banged IIO driver.
 *
 * This is intentionally not a generic SPI master. It implements the
 * ADC1283 32-clock read sequence directly using GPIOs.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/preempt.h>
#include <linux/regulator/consumer.h>

#define ADC1283_NUM_CHANNELS		8
#define ADC1283_NUM_CLOCKS		32
#define ADC1283_CMD_BITS		8
#define ADC1283_RESULT_MASK		0x0fff
#define ADC1283_RESOLUTION_BITS		12

/*
 * Target timing for DAPHNE:
 *
 *   SCLK target      = 800 kHz
 *   SCLK period      = 1250 ns
 *   ideal high time  = 625 ns
 *   ideal low time   = 625 ns
 *
 * These values are delay-loop values, not physical pin durations. The real
 * waveform also includes GPIO write/read overhead and residual kernel jitter.
 *
 * The defaults below intentionally start below 625 ns because GPIO operations
 * add extra time. Tune with an oscilloscope.
 */
#define ADC1283_DEFAULT_CS_SETUP_NS		100
#define ADC1283_DEFAULT_CS_HOLD_NS		100
#define ADC1283_DEFAULT_DOUT_SAMPLE_NS		150
#define ADC1283_DEFAULT_LOW_NS			350
#define ADC1283_DEFAULT_HIGH_NS			350

struct adc1283_gpio {
	struct device *dev;

	struct gpio_desc *sclk;
	struct gpio_desc *din;
	struct gpio_desc *dout;
	struct gpio_desc *cs;

	struct mutex lock;

	u32 cs_setup_delay_ns;
	u32 cs_hold_delay_ns;

	/*
	 * dout_sample_delay_ns is the sampling offset after SCLK falling edge.
	 * It is not an additional full low-phase delay.
	 */
	u32 dout_sample_delay_ns;

	/*
	 * sclk_low_delay_ns is the intended low-phase delay budget.
	 * The driver subtracts dout_sample_delay_ns and only waits the
	 * remaining low time after sampling DOUT.
	 */
	u32 sclk_low_delay_ns;

	/*
	 * sclk_high_delay_ns is the intended high-phase delay budget.
	 * DIN is updated during the high phase before the next falling edge.
	 */
	u32 sclk_high_delay_ns;

	bool disable_preempt;
	bool disable_irqs;

	struct regulator *vref;
	int vref_uv;
};

static void adc1283_delay_ns(u32 nsecs)
{
	u32 usecs;
	u32 rem_ns;

	if (!nsecs)
		return;

	usecs = nsecs / 1000;
	rem_ns = nsecs % 1000;

	if (usecs)
		udelay(usecs);

	if (rem_ns)
		ndelay(rem_ns);
}

static int adc1283_require_fast_gpio(struct device *dev,
				     struct gpio_desc *desc,
				     const char *name)
{
	if (gpiod_cansleep(desc)) {
		dev_err(dev,
			"%s GPIO can sleep; ADC1283 bit-bang timing requires fast GPIO\n",
			name);
		return -EOPNOTSUPP;
	}

	return 0;
}

static void adc1283_idle_bus(struct adc1283_gpio *adc)
{
	/*
	 * ADC1283 timing diagram uses SCLK idle high.
	 *
	 * CS uses logical GPIO semantics. With GPIO_ACTIVE_LOW in DT:
	 *
	 *   gpiod_set_value(cs, 0) -> CS inactive, physical high
	 *   gpiod_set_value(cs, 1) -> CS active,   physical low
	 */
	gpiod_set_raw_value(adc->sclk, 1);
	gpiod_set_raw_value(adc->din, 0);
	gpiod_set_value(adc->cs, 0);
}

static int adc1283_read_channel(struct adc1283_gpio *adc,
				unsigned int channel,
				int *value)
{
	unsigned long flags = 0;
	u8 cmd;
	u32 raw = 0;
	int i;

	if (channel >= ADC1283_NUM_CHANNELS)
		return -EINVAL;

	/*
	 * ADC1283 control register:
	 *
	 * bit 7: don't care
	 * bit 6: don't care
	 * bit 5: ADD2
	 * bit 4: ADD1
	 * bit 3: ADD0
	 * bit 2: don't care
	 * bit 1: don't care
	 * bit 0: don't care
	 *
	 * Therefore the channel command byte is channel << 3.
	 */
	cmd = (channel & 0x7) << 3;

	mutex_lock(&adc->lock);

	/*
	 * Enter the transaction from the required idle state:
	 * SCLK high, DIN low, CS inactive.
	 */
	gpiod_set_raw_value(adc->sclk, 1);
	gpiod_set_raw_value(adc->din, 0);

	if (adc->disable_preempt)
		preempt_disable();

	if (adc->disable_irqs)
		local_irq_save(flags);

	/*
	 * CS falling edge starts the conversion.
	 */
	gpiod_set_value(adc->cs, 1);
	adc1283_delay_ns(adc->cs_setup_delay_ns);

	for (i = 0; i < ADC1283_NUM_CLOCKS; i++) {
		bool din_bit;
		u32 low_tail_ns;
		int dout_bit;

		/*
		 * At this point SCLK is high.
		 *
		 * First keep SCLK high for the configured high delay. This also
		 * guarantees DIN hold time after the previous rising edge before
		 * DIN is changed for the next bit.
		 */
		adc1283_delay_ns(adc->sclk_high_delay_ns);

		/*
		 * Update DIN while SCLK is still high.
		 *
		 * This prevents the DIN GPIO write from stretching the SCLK low
		 * phase. DIN will remain stable throughout the upcoming low
		 * phase and will be latched by the next SCLK rising edge.
		 */
		if (i < ADC1283_CMD_BITS)
			din_bit = !!(cmd & BIT(7 - i));
		else
			din_bit = false;

		gpiod_set_raw_value(adc->din, din_bit);

		/*
		 * Falling edge.
		 *
		 * ADC1283 DOUT becomes valid after the falling edge. The first
		 * edge after CS assertion is therefore falling, because SCLK
		 * idles high.
		 */
		gpiod_set_raw_value(adc->sclk, 0);

		/*
		 * Wait until the configured sample point inside the low phase.
		 *
		 * If the sample point is configured beyond the low delay budget,
		 * clamp it to the low delay. This avoids unsigned underflow in
		 * the low-tail calculation.
		 */
		if (adc->dout_sample_delay_ns >= adc->sclk_low_delay_ns) {
			adc1283_delay_ns(adc->sclk_low_delay_ns);
			low_tail_ns = 0;
		} else {
			adc1283_delay_ns(adc->dout_sample_delay_ns);
			low_tail_ns = adc->sclk_low_delay_ns -
				      adc->dout_sample_delay_ns;
		}

		/*
		 * Sample DOUT during SCLK low.
		 *
		 * Reading every cycle keeps the operation count constant across
		 * all 32 cycles, even though only the second 16-bit word is used.
		 */
		dout_bit = gpiod_get_raw_value(adc->dout);
		raw = (raw << 1) | !!dout_bit;

		/*
		 * Complete the remaining low phase. This is only the remainder
		 * after the DOUT sample point, not a second full low delay.
		 */
		adc1283_delay_ns(low_tail_ns);

		/*
		 * Rising edge.
		 *
		 * ADC1283 latches DIN here.
		 */
		gpiod_set_raw_value(adc->sclk, 1);
	}

	/*
	 * Hold CS after the final SCLK rising edge, then stop conversion.
	 */
	adc1283_delay_ns(adc->cs_hold_delay_ns);
	gpiod_set_value(adc->cs, 0);

	gpiod_set_raw_value(adc->din, 0);
	gpiod_set_raw_value(adc->sclk, 1);

	if (adc->disable_irqs)
		local_irq_restore(flags);

	if (adc->disable_preempt)
		preempt_enable();

	mutex_unlock(&adc->lock);

	/*
	 * Captured 32 bits:
	 *
	 * raw[31:16] = first word, normally discarded
	 * raw[15:0]  = selected-channel word = 0000 DB11..DB0
	 */
	*value = raw & ADC1283_RESULT_MASK;

	return 0;
}

#define ADC1283_CHAN(_idx)						\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.channel = (_idx),					\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	}

static const struct iio_chan_spec adc1283_channels[] = {
	ADC1283_CHAN(0),
	ADC1283_CHAN(1),
	ADC1283_CHAN(2),
	ADC1283_CHAN(3),
	ADC1283_CHAN(4),
	ADC1283_CHAN(5),
	ADC1283_CHAN(6),
	ADC1283_CHAN(7),
};

static int adc1283_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			    int *val,
			    int *val2,
			    long mask)
{
	struct adc1283_gpio *adc = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = adc1283_read_channel(adc, chan->channel, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		if (adc->vref_uv <= 0)
			return -EINVAL;

		/*
		 * ADC1283 uses AVCC as reference.
		 * Scale = Vref / 2^12.
		 * IIO voltage scale is normally reported in millivolts.
		 */
		*val = adc->vref_uv / 1000;
		*val2 = ADC1283_RESOLUTION_BITS;

		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static const struct iio_info adc1283_iio_info = {
	.read_raw = adc1283_read_raw,
};

static void adc1283_regulator_disable(void *data)
{
	struct regulator *reg = data;

	regulator_disable(reg);
}

static void adc1283_read_timing_properties(struct adc1283_gpio *adc)
{
	struct device *dev = adc->dev;

	adc->cs_setup_delay_ns = ADC1283_DEFAULT_CS_SETUP_NS;
	adc->cs_hold_delay_ns = ADC1283_DEFAULT_CS_HOLD_NS;
	adc->dout_sample_delay_ns = ADC1283_DEFAULT_DOUT_SAMPLE_NS;
	adc->sclk_low_delay_ns = ADC1283_DEFAULT_LOW_NS;
	adc->sclk_high_delay_ns = ADC1283_DEFAULT_HIGH_NS;

	device_property_read_u32(dev, "daphne,cs-setup-delay-ns",
				 &adc->cs_setup_delay_ns);
	device_property_read_u32(dev, "daphne,cs-hold-delay-ns",
				 &adc->cs_hold_delay_ns);
	device_property_read_u32(dev, "daphne,dout-sample-delay-ns",
				 &adc->dout_sample_delay_ns);

	/*
	 * New property name. This is the total low-phase delay budget.
	 */
	device_property_read_u32(dev, "daphne,sclk-low-delay-ns",
				 &adc->sclk_low_delay_ns);

	/*
	 * Backward compatibility with the previous overlay property.
	 *
	 * Note: old "low-tail" semantics were different. If this property is
	 * still used, treat it as the new total low delay to avoid double
	 * counting sample delay + tail delay.
	 */
	device_property_read_u32(dev, "daphne,sclk-low-tail-delay-ns",
				 &adc->sclk_low_delay_ns);

	device_property_read_u32(dev, "daphne,sclk-high-delay-ns",
				 &adc->sclk_high_delay_ns);

	adc->disable_preempt =
		device_property_read_bool(dev, "daphne,disable-preempt");
	adc->disable_irqs =
		device_property_read_bool(dev, "daphne,disable-irqs");

	if (adc->dout_sample_delay_ns > adc->sclk_low_delay_ns)
		dev_warn(dev,
			 "dout_sample_delay_ns=%u exceeds sclk_low_delay_ns=%u; sample point will be clamped\n",
			 adc->dout_sample_delay_ns,
			 adc->sclk_low_delay_ns);
}

static int adc1283_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct adc1283_gpio *adc;
	u32 vref_uv;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->dev = dev;
	mutex_init(&adc->lock);

	adc1283_read_timing_properties(adc);

	adc->sclk = devm_gpiod_get(dev, "sclk", GPIOD_OUT_HIGH);
	if (IS_ERR(adc->sclk))
		return dev_err_probe(dev, PTR_ERR(adc->sclk),
				     "failed to get SCLK GPIO\n");

	adc->din = devm_gpiod_get(dev, "din", GPIOD_OUT_LOW);
	if (IS_ERR(adc->din))
		return dev_err_probe(dev, PTR_ERR(adc->din),
				     "failed to get DIN GPIO\n");

	adc->dout = devm_gpiod_get(dev, "dout", GPIOD_IN);
	if (IS_ERR(adc->dout))
		return dev_err_probe(dev, PTR_ERR(adc->dout),
				     "failed to get DOUT GPIO\n");

	/*
	 * Use GPIO_ACTIVE_LOW in DT for ADC1283 CS.
	 * Logical 1 = active, logical 0 = inactive.
	 */
	adc->cs = devm_gpiod_get(dev, "cs", GPIOD_OUT_LOW);
	if (IS_ERR(adc->cs))
		return dev_err_probe(dev, PTR_ERR(adc->cs),
				     "failed to get CS GPIO\n");

	ret = adc1283_require_fast_gpio(dev, adc->sclk, "SCLK");
	if (ret)
		return ret;

	ret = adc1283_require_fast_gpio(dev, adc->din, "DIN");
	if (ret)
		return ret;

	ret = adc1283_require_fast_gpio(dev, adc->dout, "DOUT");
	if (ret)
		return ret;

	ret = adc1283_require_fast_gpio(dev, adc->cs, "CS");
	if (ret)
		return ret;

	/*
	 * Optional reference regulator. If absent, allow a fixed DT property.
	 */
	adc->vref = devm_regulator_get_optional(dev, "vref");
	if (!IS_ERR(adc->vref)) {
		ret = regulator_enable(adc->vref);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to enable vref regulator\n");

		ret = devm_add_action_or_reset(dev,
						adc1283_regulator_disable,
						adc->vref);
		if (ret)
			return ret;

		adc->vref_uv = regulator_get_voltage(adc->vref);
		if (adc->vref_uv <= 0)
			dev_warn(dev,
				 "vref regulator voltage unavailable; scale disabled\n");
	} else {
		ret = PTR_ERR(adc->vref);
		adc->vref = NULL;

		if (ret != -ENODEV)
			return dev_err_probe(dev, ret,
					     "failed to get vref regulator\n");

		if (!device_property_read_u32(dev, "daphne,vref-microvolt",
					      &vref_uv))
			adc->vref_uv = vref_uv;
	}

	adc1283_idle_bus(adc);

	indio_dev->name = "adc1283-gpio";
	indio_dev->info = &adc1283_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adc1283_channels;
	indio_dev->num_channels = ARRAY_SIZE(adc1283_channels);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IIO device\n");

	dev_info(dev,
		 "ADC1283 GPIO driver active: cs_setup=%u ns cs_hold=%u ns dout_sample=%u ns low=%u ns high=%u ns preempt=%u irqs=%u\n",
		 adc->cs_setup_delay_ns,
		 adc->cs_hold_delay_ns,
		 adc->dout_sample_delay_ns,
		 adc->sclk_low_delay_ns,
		 adc->sclk_high_delay_ns,
		 adc->disable_preempt,
		 adc->disable_irqs);

	return 0;
}

static const struct of_device_id adc1283_of_match[] = {
	{ .compatible = "daphne,adc1283-gpio" },
	{ .compatible = "st,adc1283-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, adc1283_of_match);

static struct platform_driver adc1283_driver = {
	.probe = adc1283_probe,
	.driver = {
		.name = "adc1283-gpio",
		.of_match_table = adc1283_of_match,
	},
};
module_platform_driver(adc1283_driver);

MODULE_DESCRIPTION("ADC1283 GPIO bit-banged IIO ADC driver");
MODULE_AUTHOR("DAPHNE collaboration");
MODULE_LICENSE("GPL");