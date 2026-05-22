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
#include <linux/interrupt.h>
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
 * Current scope-validated starting point:
 *
 *   cs_setup_delay_ns     = 20 ns
 *   cs_hold_delay_ns      = 100 ns
 *   dout_sample_delay_ns  = 100 ns
 *   sclk_low_delay_ns     = 100 ns
 *   sclk_high_delay_ns    = 360 ns
 *
 * The measured MEZ0 waveform was approximately:
 *
 *   SCLK high = 685 ns
 *   SCLK low  = 564 ns
 *   period    = 1249 ns
 *   frequency = 800.6 kHz
 */
#define ADC1283_DEFAULT_CS_SETUP_NS		20
#define ADC1283_DEFAULT_CS_HOLD_NS		100
#define ADC1283_DEFAULT_DOUT_SAMPLE_NS		100
#define ADC1283_DEFAULT_LOW_NS			100
#define ADC1283_DEFAULT_HIGH_NS			360

/*
 * Dummy SCLK cycles generated while CS is inactive.
 *
 * This makes SCLK already active before CS falls and also primes the GPIO path,
 * avoiding the previously observed first-cycle anomaly.
 */
#define ADC1283_DEFAULT_PRECLOCK_CYCLES		2

/*
 * DIN hold time after SCLK rising edge. This is an offset inside the high
 * phase, not an extra full high-phase delay.
 */
#define ADC1283_DEFAULT_DIN_HOLD_NS		20

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
	 * DOUT sample offset after SCLK falling edge.
	 * This is inside the low phase.
	 */
	u32 dout_sample_delay_ns;

	/*
	 * Total programmed low-phase delay budget.
	 * The driver waits to the DOUT sample point, reads DOUT, then waits
	 * the remaining low-phase delay.
	 */
	u32 sclk_low_delay_ns;

	/*
	 * Total programmed high-phase delay budget.
	 * The driver waits a DIN-hold offset after SCLK rising, changes DIN
	 * for the next bit, then waits the remaining high-phase delay.
	 */
	u32 sclk_high_delay_ns;

	u32 preclock_cycles;
	u32 din_hold_delay_ns;

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

static void adc1283_delay_clamped(u32 nsecs, u32 max_nsecs,
				  u32 *remaining_nsecs)
{
	if (nsecs >= max_nsecs) {
		adc1283_delay_ns(max_nsecs);
		*remaining_nsecs = 0;
	} else {
		adc1283_delay_ns(nsecs);
		*remaining_nsecs = max_nsecs - nsecs;
	}
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
	gpiod_set_value(adc->cs, 0);
	gpiod_set_raw_value(adc->sclk, 1);
	gpiod_set_raw_value(adc->din, 0);
}

static bool adc1283_din_bit(u8 cmd, int bit)
{
	if (bit < ADC1283_CMD_BITS)
		return !!(cmd & BIT(7 - bit));

	return false;
}

static void adc1283_preclock_gpio_path(struct adc1283_gpio *adc)
{
	u32 low_tail_ns;
	unsigned int n;
	int dummy;

	/*
	 * Preclock with CS inactive.
	 *
	 * This produces real SCLK activity before CS falls. The ADC1283 ignores
	 * these cycles because CS is inactive, but the GPIO set/get paths and
	 * SCLK timing are already warmed up before the real frame starts.
	 */
	gpiod_set_value(adc->cs, 0);
	gpiod_set_raw_value(adc->din, 0);
	gpiod_set_raw_value(adc->sclk, 1);

	for (n = 0; n < adc->preclock_cycles; n++) {
		/*
		 * Keep the same high/low structure as the real bit loop.
		 */
		adc1283_delay_ns(adc->sclk_high_delay_ns);

		gpiod_set_raw_value(adc->sclk, 0);

		adc1283_delay_clamped(adc->dout_sample_delay_ns,
				      adc->sclk_low_delay_ns,
				      &low_tail_ns);

		dummy = gpiod_get_raw_value(adc->dout);
		(void)dummy;

		adc1283_delay_ns(low_tail_ns);

		gpiod_set_raw_value(adc->sclk, 1);
	}

	/*
	 * Leave the bus in the required idle state for the real frame:
	 * SCLK high, DIN low, CS inactive.
	 */
	gpiod_set_raw_value(adc->sclk, 1);
	gpiod_set_raw_value(adc->din, 0);
}

static int adc1283_read_channel(struct adc1283_gpio *adc,
				unsigned int channel,
				int *value)
{
	unsigned long flags = 0;
	u8 cmd;
	u32 raw = 0;
	u16 word;
	u16 prefix;
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
	 * Enter from idle:
	 *   SCLK high
	 *   DIN low
	 *   CS inactive
	 */
	gpiod_set_value(adc->cs, 0);
	gpiod_set_raw_value(adc->sclk, 1);
	gpiod_set_raw_value(adc->din, 0);

	if (adc->disable_preempt)
		preempt_disable();

	if (adc->disable_irqs)
		local_irq_save(flags);

	/*
	 * Run dummy SCLK cycles while CS is inactive.
	 *
	 * This solves two problems:
	 *   1. It primes the GPIO path before the real frame.
	 *   2. It makes SCLK already active before CS goes low.
	 */
	adc1283_preclock_gpio_path(adc);

	/*
	 * Prepare DIN bit 0 while CS is still inactive and SCLK is high.
	 * This gives maximum setup margin for the first real rising edge.
	 */
	gpiod_set_raw_value(adc->din, adc1283_din_bit(cmd, 0));

	/*
	 * Complete a normal SCLK high phase before asserting CS. This makes
	 * the first real falling edge occur shortly after CS falling instead
	 * of waiting a full high-delay after CS is already active.
	 */
	adc1283_delay_ns(adc->sclk_high_delay_ns);

	/*
	 * CS falling edge starts the ADC1283 frame. SCLK is high here.
	 *
	 * After this short delay, SCLK falls immediately. The following low
	 * phase gives much more than the ADC1283 CS setup requirement before
	 * the next SCLK rising edge.
	 */
	gpiod_set_value(adc->cs, 1);
	adc1283_delay_ns(adc->cs_setup_delay_ns);

	for (i = 0; i < ADC1283_NUM_CLOCKS; i++) {
		u32 low_tail_ns;
		u32 high_tail_ns;
		int dout_bit;

		/*
		 * Falling edge.
		 *
		 * For i == 0, this occurs shortly after CS assertion because
		 * the SCLK high phase was already completed while CS was still
		 * inactive.
		 */
		gpiod_set_raw_value(adc->sclk, 0);

		/*
		 * Sample DOUT at a programmable offset inside the low phase.
		 */
		adc1283_delay_clamped(adc->dout_sample_delay_ns,
				      adc->sclk_low_delay_ns,
				      &low_tail_ns);

		dout_bit = gpiod_get_raw_value(adc->dout);
		raw = (raw << 1) | !!dout_bit;

		adc1283_delay_ns(low_tail_ns);

		/*
		 * Rising edge. ADC1283 latches DIN here.
		 */
		gpiod_set_raw_value(adc->sclk, 1);

		/*
		 * Do not prepare another bit after the final rising edge.
		 * Hold CS active for cs_hold_delay_ns, then deassert.
		 */
		if (i == ADC1283_NUM_CLOCKS - 1)
			break;

		/*
		 * Hold DIN after the SCLK rising edge, then change DIN for the
		 * next bit. The hold delay is an offset inside the SCLK high
		 * phase, not an extra full delay.
		 */
		adc1283_delay_clamped(adc->din_hold_delay_ns,
				      adc->sclk_high_delay_ns,
				      &high_tail_ns);

		gpiod_set_raw_value(adc->din, adc1283_din_bit(cmd, i + 1));

		/*
		 * Complete the remaining SCLK high phase before the next
		 * falling edge.
		 */
		adc1283_delay_ns(high_tail_ns);
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
	 *
	 * Validate the upper nibble of the selected 16-bit word. For a valid
	 * ADC1283 frame, bits [15:12] must be zero.
	 */
	word = raw & 0xffff;
	prefix = word >> 12;

	if (prefix != 0) {
		dev_warn_ratelimited(adc->dev,
				     "invalid ADC1283 frame: raw=0x%08x word=0x%04x prefix=0x%x\n",
				     raw, word, prefix);
		return -EIO;
	}

	*value = word & ADC1283_RESULT_MASK;

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
		 * IIO voltage scale is reported in millivolts.
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
	adc->preclock_cycles = ADC1283_DEFAULT_PRECLOCK_CYCLES;
	adc->din_hold_delay_ns = ADC1283_DEFAULT_DIN_HOLD_NS;

	device_property_read_u32(dev, "daphne,cs-setup-delay-ns",
				 &adc->cs_setup_delay_ns);
	device_property_read_u32(dev, "daphne,cs-hold-delay-ns",
				 &adc->cs_hold_delay_ns);
	device_property_read_u32(dev, "daphne,dout-sample-delay-ns",
				 &adc->dout_sample_delay_ns);

	/*
	 * Current property name. This is the total low-phase delay budget.
	 */
	device_property_read_u32(dev, "daphne,sclk-low-delay-ns",
				 &adc->sclk_low_delay_ns);

	/*
	 * Backward compatibility with the previous overlay property.
	 *
	 * Old "low-tail" semantics were different. If this property is still
	 * used, treat it as the new total low delay to avoid double counting.
	 */
	device_property_read_u32(dev, "daphne,sclk-low-tail-delay-ns",
				 &adc->sclk_low_delay_ns);

	device_property_read_u32(dev, "daphne,sclk-high-delay-ns",
				 &adc->sclk_high_delay_ns);
	device_property_read_u32(dev, "daphne,preclock-cycles",
				 &adc->preclock_cycles);
	device_property_read_u32(dev, "daphne,din-hold-delay-ns",
				 &adc->din_hold_delay_ns);

	adc->disable_preempt =
		device_property_read_bool(dev, "daphne,disable-preempt");
	adc->disable_irqs =
		device_property_read_bool(dev, "daphne,disable-irqs");

	if (adc->dout_sample_delay_ns > adc->sclk_low_delay_ns)
		dev_warn(dev,
			 "dout_sample_delay_ns=%u exceeds sclk_low_delay_ns=%u; sample point will be clamped\n",
			 adc->dout_sample_delay_ns,
			 adc->sclk_low_delay_ns);

	if (adc->din_hold_delay_ns > adc->sclk_high_delay_ns)
		dev_warn(dev,
			 "din_hold_delay_ns=%u exceeds sclk_high_delay_ns=%u; DIN update point will be clamped\n",
			 adc->din_hold_delay_ns,
			 adc->sclk_high_delay_ns);
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
		 "ADC1283 GPIO driver active: cs_setup=%u ns cs_hold=%u ns dout_sample=%u ns low=%u ns high=%u ns din_hold=%u ns preclock=%u preempt=%u irqs=%u\n",
		 adc->cs_setup_delay_ns,
		 adc->cs_hold_delay_ns,
		 adc->dout_sample_delay_ns,
		 adc->sclk_low_delay_ns,
		 adc->sclk_high_delay_ns,
		 adc->din_hold_delay_ns,
		 adc->preclock_cycles,
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