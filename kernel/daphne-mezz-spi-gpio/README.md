# DAPHNE Mezzanine GPIO SPI Kernel Driver

This is an experimental Linux SPI master for the DAPHNE mezzanine PS MIO
SPI-style pins. It is intended to improve timing control over the generic
Linux `spi-gpio` driver.

The driver binds to:

```dts
compatible = "daphne,mezz-spi-gpio";
```

and still exposes normal Linux SPI children such as:

```text
/dev/spidevX.0
```

## Initial Scope

- SPI mode 0 only
- 8-bit words only
- one chip select
- calibrated delay properties for SCLK high and low phases
- optional preemption/IRQ masking for short ADC1283-sized transfers

The low-jitter path is meant for short transfers such as:

```text
ADC1283 16 clocks -> 2 bytes
ADC1283 32 clocks -> 4 bytes
```

Do not disable interrupts around long scope bursts.

## Build Out Of Tree

On the board or on a matching PetaLinux build host with kernel headers:

```bash
cd kernel/daphne-mezz-spi-gpio
make KDIR=/path/to/kernel/build
```

On a board with matching `/lib/modules/$(uname -r)/build`:

```bash
make
sudo insmod spi-daphne-mezz-gpio.ko
```

Check:

```bash
dmesg | grep -i 'DAPHNE mezz SPI GPIO'
ls /sys/bus/platform/drivers/daphne-mezz-spi-gpio
```

## MEZ0 Runtime Overlay

From the repo root on the board:

```bash
sudo sh scripts/apply_mezz0_daphne_spigpio_overlay.sh apply
```

Status:

```bash
sudo sh scripts/apply_mezz0_daphne_spigpio_overlay.sh status
```

Remove:

```bash
sudo sh scripts/apply_mezz0_daphne_spigpio_overlay.sh remove
```

## Timing Properties

The overlay supports:

```dts
daphne,sclk-high-delay-ns = <250>;
daphne,sclk-low-delay-ns = <250>;
daphne,mosi-setup-delay-ns = <0>;
daphne,critical-max-bytes = <4>;
daphne,disable-preempt;
daphne,disable-irqs;
```

These are delay-loop values, not guaranteed physical high/low durations. The
physical timing is:

```text
configured delay + GPIO operation overhead + residual kernel jitter
```

For an 800 kHz target:

```text
period = 1250 ns
ideal high = 625 ns
ideal low = 625 ns
```

Start with:

```bash
HIGH_DELAY_NS=250 LOW_DELAY_NS=250 \
  sudo sh scripts/apply_mezz0_daphne_spigpio_overlay.sh apply
```

Then run a short transfer:

```bash
python client/mezz0_spidev_test.py \
  --device /dev/spidev4.0 \
  --speed 800000 \
  --tx "AA" \
  --loop 0 \
  --delay-ms 0 \
  --quiet
```

Measure SCLK high time, low time, and jitter on the scope. Tune
`HIGH_DELAY_NS` and `LOW_DELAY_NS`, reload the overlay, and repeat.

## PetaLinux In-Tree Integration Sketch

Copy:

```text
spi-daphne-mezz-gpio.c -> drivers/spi/
```

Add to `drivers/spi/Kconfig`:

```text
config SPI_DAPHNE_MEZZ_GPIO
	tristate "DAPHNE calibrated mezzanine GPIO SPI master"
	depends on GPIOLIB
	help
	  SPI master driver for the DAPHNE mezzanine PS MIO SPI-style lines.
```

Add to `drivers/spi/Makefile`:

```make
obj-$(CONFIG_SPI_DAPHNE_MEZZ_GPIO) += spi-daphne-mezz-gpio.o
```

Enable:

```text
CONFIG_SPI_DAPHNE_MEZZ_GPIO=y
CONFIG_SPI_SPIDEV=y
```

Then rebuild and boot the kernel.
