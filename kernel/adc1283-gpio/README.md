# ADC1283 GPIO/IIO Driver Bring-up and Reproduction Guide

This README describes how to build, deploy, validate, and reproduce the DAPHNE `adc1283-gpio` driver setup.

The driver is ADC1283-specific. It is not a generic SPI master and it does not create `/dev/spidev*`. It registers Linux IIO ADC devices and exposes raw channel values through sysfs:

```text
/sys/bus/iio/devices/iio:deviceX/in_voltage0_raw
/sys/bus/iio/devices/iio:deviceX/in_voltage1_raw
...
/sys/bus/iio/devices/iio:deviceX/in_voltage7_raw
```

The current validated target is a GPIO-bit-banged ADC1283 read at approximately 800 kHz SCLK using DAPHNE mezzanine PS MIO GPIO pins.

## 1. Repository layout

Recommended project layout:

```text
<repo>/
  kernel/
    adc1283-gpio/
      adc1283-gpio.c
      Makefile
      Kconfig
      README.md
  scripts/
    apply_mezz0_adc1283_gpio_overlay.sh          # legacy/single-MEZ0 helper
    apply_mezz_adc1283_gpio_overlays.sh          # preferred all/selective-mezz helper
```

The single-MEZ0 overlay script was used during the first oscilloscope validation. For current testing, prefer:

```text
scripts/apply_mezz_adc1283_gpio_overlays.sh
```

This script supports applying/removing one mezzanine at a time or all mezzanines.

## 2. Hardware and protocol assumptions

Target system:

```text
DAPHNE motherboard
Kria SOM
PetaLinux / Xilinx Linux kernel
PS MIO GPIO controller: zynqmp_gpio
```

ADC1283 transaction model implemented by the driver:

```text
SCLK idle high
CS low
32 SCLK cycles, falling edge first
DIN sends channel address during the first 8 cycles
DOUT is sampled during the SCLK low phase
first 16-bit DOUT word is discarded
second 16-bit DOUT word = 0000 + 12-bit ADC result
CS high
```

The ADC1283 control byte is:

```c
cmd = (channel & 0x7) << 3;
```

Channel command values:

```text
IN0 -> 0x00
IN1 -> 0x08
IN2 -> 0x10
IN3 -> 0x18
IN4 -> 0x20
IN5 -> 0x28
IN6 -> 0x30
IN7 -> 0x38
```

## 3. Mezzanine GPIO mapping

The all-mezzanine ADC1283 overlay uses this mapping:

```text
MEZ0:
  CS   -> MIO38 -> gpio38
  SCLK -> MIO39 -> gpio39
  DOUT -> MIO40 -> gpio40
  DIN  -> MIO50 -> gpio50

MEZ1:
  CS   -> MIO41 -> gpio41
  SCLK -> MIO42 -> gpio42
  DOUT -> MIO43 -> gpio43
  DIN  -> MIO61 -> gpio61

MEZ2:
  CS   -> MIO62 -> gpio62
  SCLK -> MIO63 -> gpio63
  DOUT -> MIO73 -> gpio73
  DIN  -> MIO74 -> gpio74

MEZ3:
  CS   -> MIO69 -> gpio69
  SCLK -> MIO68 -> gpio68
  DOUT -> MIO67 -> gpio67
  DIN  -> MIO57 -> gpio57

MEZ4:
  CS   -> MIO65 -> gpio65
  SCLK -> MIO64 -> gpio64
  DOUT -> MIO46 -> gpio46
  DIN  -> MIO45 -> gpio45
```

For the ADC1283 driver, `cs-gpios` should use logical GPIO semantics with active-low polarity. Example for MEZ0:

```dts
cs-gpios   = <&gpio 38 1>;
sclk-gpios = <&gpio 39 0>;
dout-gpios = <&gpio 40 0>;
din-gpios  = <&gpio 50 0>;
```

The driver should use:

```c
gpiod_set_value(adc->cs, 1); /* assert CS, physical low */
gpiod_set_value(adc->cs, 0); /* deassert CS, physical high */
```

## 4. Important driver fixes already applied

### 4.1 SCLK duty-cycle fix

The initial driver had an SCLK duty-cycle issue. The low phase was effectively:

```text
dout_sample_delay_ns + DOUT GPIO read overhead + sclk_low_tail_delay_ns
```

while the high phase was only:

```text
sclk_high_delay_ns + overhead
```

This made SCLK low roughly twice as long as SCLK high.

The patched driver changed the timing model:

```text
sclk_low_tail_delay_ns -> sclk_low_delay_ns
```

`dout_sample_delay_ns` is now an offset inside the low phase, not an additional full low-phase delay.

The bit loop was also restructured so DIN/MOSI is updated while SCLK is high. This avoids stretching the SCLK low phase.

Intended bit-cycle shape:

```text
SCLK high
set DIN for next bit
delay high phase
SCLK low
delay to DOUT sample point
sample DOUT
delay remaining low phase
SCLK high
```

This means DIN may toggle before SCLK goes low. That was checked on the oscilloscope and is acceptable because the ADC1283 latches DIN on the following SCLK rising edge.

### 4.2 First-cycle low-pulse anomaly fix

A first-cycle low-pulse anomaly was observed: after CS went low, only the first SCLK low pulse was longer; from the second cycle onward timing normalized.

The mitigation is to prime the GPIO path while CS is inactive before the real ADC1283 frame:

```c
static void adc1283_prime_gpio_path(struct adc1283_gpio *adc)
{
    gpiod_set_value(adc->cs, 0);

    gpiod_set_raw_value(adc->din, 0);
    gpiod_set_raw_value(adc->sclk, 1);

    gpiod_set_raw_value(adc->sclk, 0);
    gpiod_get_raw_value(adc->dout);
    gpiod_set_raw_value(adc->sclk, 1);

    gpiod_set_raw_value(adc->din, 0);
}
```

Call this before asserting CS for the real conversion.

### 4.3 Invalid ADC1283 frame detection

When nothing is connected, or if the ADC1283 DOUT line is floating, readings can appear as `0` or `4095`. This is not necessarily a valid conversion. It may simply reflect a DOUT line floating low or high.

For a valid ADC1283 selected-channel word, the second 16-bit word should be:

```text
0000 D11 D10 D9 D8 D7 D6 D5 D4 D3 D2 D1 D0
```

Therefore, bits `[15:12]` of the selected word should be zero.

Add this check at the end of `adc1283_read_channel()`, replacing the simple mask-only return.

Look for this original block:

```c
mutex_unlock(&adc->lock);

/*
 * Captured 32 bits:
 *
 * raw[31:16] = first word, normally discarded
 * raw[15:0]  = selected-channel word = 0000 DB11..DB0
 */
*value = raw & ADC1283_RESULT_MASK;

return 0;
```

Replace it with:

```c
mutex_unlock(&adc->lock);

/*
 * Captured 32 bits:
 *
 * raw[31:16] = first word, normally discarded
 * raw[15:0]  = selected-channel word = 0000 DB11..DB0
 *
 * Validate the upper nibble of the selected 16-bit word.
 * For a valid ADC1283 frame, bits [15:12] must be zero.
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
```

Also add these declarations near the top of `adc1283_read_channel()`:

```c
u16 word;
u16 prefix;
```

The function header should then look like:

```c
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
```

This prevents a floating-high DOUT line from silently appearing as a valid `4095` sample. A DOUT line stuck low can still produce a valid-looking zero frame (`0x0000`), so hardware validation is still required.

## 5. Current accepted timing checkpoint

Target:

```text
SCLK = 800 kHz
period = 1250 ns
ideal high = 625 ns
ideal low = 625 ns
```

Accepted scope measurement on MEZ0:

```text
SCLK rise time, 0%-100%: 24 ns
SCLK high time:          685 ns
SCLK low time:           564 ns
SCLK period:             1249 ns
SCLK frequency:          approximately 800.6 kHz
SCLK high duty:          approximately 54.8 %
SCLK low duty:           approximately 45.2 %
```

This was accepted because the frequency is essentially at 800 kHz and the duty cycle is inside the ADC1283 40% to 60% SCLK duty-cycle range.

Use these timing defaults:

```sh
TARGET_SCLK_HZ=800000
CS_SETUP_DELAY_NS=20
CS_HOLD_DELAY_NS=100
DOUT_SAMPLE_DELAY_NS=100
SCLK_LOW_DELAY_NS=100
SCLK_HIGH_DELAY_NS=360
VREF_UV=3300000
```

The overlay must use:

```dts
daphne,sclk-low-delay-ns = <100>;
```

Do not use the old property name:

```dts
daphne,sclk-low-tail-delay-ns = <...>;
```

Expected patched driver log:

```text
ADC1283 GPIO driver active: cs_setup=20 ns cs_hold=100 ns dout_sample=100 ns low=100 ns high=360 ns preempt=1 irqs=1
```

If the log still says `low_tail`, the old `.ko` is still loaded or the source was not rebuilt/copied correctly.

## 6. Build the driver out of tree

Use a matching PetaLinux/Xilinx kernel build tree. The kernel build tree must match the kernel running on the target board.

Set the kernel build directory:

```sh
export KDIR=<path-to-linux-xlnx-build-tree>
```

Typical examples:

```sh
export KDIR=<petalinux-project>/build/tmp/work/<machine>-xilinx-linux/linux-xlnx/<version>/linux-xlnx-<version>
```

or, if building directly on a target with headers installed:

```sh
export KDIR=/lib/modules/$(uname -r)/build
```

Validate the kernel build directory:

```sh
ls "$KDIR/Makefile"
ls "$KDIR/include/generated/autoconf.h"
ls "$KDIR/Module.symvers"
```

Export the cross-compile environment. Adjust the toolchain path to your PetaLinux project or SDK:

```sh
export ARCH=arm64
export CROSS_COMPILE=aarch64-xilinx-linux-
export PATH=<path-to-xilinx-cross-toolchain-bin>:$PATH
```

Build:

```sh
cd <repo>/kernel/adc1283-gpio
make clean
make KDIR="$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE"
```

Expected result:

```text
adc1283-gpio.ko
```

Check the module metadata:

```sh
ls -lh adc1283-gpio.ko
modinfo adc1283-gpio.ko | grep vermagic
strings adc1283-gpio.ko | grep -E "low_tail|low=%u|ADC1283 GPIO driver active"
```

The patched module should show `low=%u` and should not show `low_tail` in the active status string.

## 7. Copy and load the driver on the target

Copy the module to the target board:

```sh
scp adc1283-gpio.ko <user>@<target-host>:/tmp/adc1283-gpio.ko
```

On the target:

```sh
cd <repo-on-target>/scripts

sudo sh apply_mezz0_adc1283_gpio_overlay.sh remove 2>/dev/null || true
sudo sh apply_mezz_adc1283_gpio_overlays.sh remove all 2>/dev/null || true
sudo rmmod adc1283_gpio 2>/dev/null || true
sudo insmod /tmp/adc1283-gpio.ko
```

Check:

```sh
lsmod | grep adc1283
dmesg | grep -i adc1283 | tail -30
```

If `insmod` reports `invalid module format`, compare:

```sh
uname -r
modinfo /tmp/adc1283-gpio.ko | grep vermagic
```

The running kernel and module vermagic must match.

For persistent installation instead of temporary `/tmp` loading:

```sh
sudo mkdir -p /lib/modules/$(uname -r)/extra
sudo cp adc1283-gpio.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
sudo modprobe adc1283-gpio
```

During development, temporary `/tmp` loading with `insmod` is usually clearer because it avoids accidentally using an older installed module.

## 8. Apply ADC1283 overlays

Preferred script:

```text
scripts/apply_mezz_adc1283_gpio_overlays.sh
```

It should support selected mezzanines:

```sh
# Apply only MEZ0
sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh apply mezz0

# Apply MEZ0 and MEZ3
sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh apply mezz0 mezz3

# Remove only MEZ3
sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh remove mezz3

# Apply all mezzanines
sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh apply all

# Remove all mezzanines
sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh remove all

# Status
sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh status all
```

Apply with the accepted timing checkpoint:

```sh
cd <repo-on-target>/scripts

CS_SETUP_DELAY_NS=20 \
DOUT_SAMPLE_DELAY_NS=100 \
SCLK_LOW_DELAY_NS=100 \
SCLK_HIGH_DELAY_NS=360 \
sudo -E sh apply_mezz_adc1283_gpio_overlays.sh apply mezz0
```

For all mezzanines:

```sh
CS_SETUP_DELAY_NS=20 \
DOUT_SAMPLE_DELAY_NS=100 \
SCLK_LOW_DELAY_NS=100 \
SCLK_HIGH_DELAY_NS=360 \
sudo -E sh apply_mezz_adc1283_gpio_overlays.sh apply all
```

The all-mezz script should create independent configfs overlays per mezzanine, plus one shared base overlay for common PS node disables. This allows removing `mezz3` without removing `mezz0`.

After a clean boot, the expected all-mezz status is:

```text
mezz0: overlay=active dt=present platform=present driver=bound
mezz1: overlay=active dt=present platform=present driver=bound
mezz2: overlay=active dt=present platform=present driver=bound
mezz3: overlay=active dt=present platform=present driver=bound
mezz4: overlay=active dt=present platform=present driver=bound
```

Expected platform devices:

```text
adc1283_mezz0
adc1283_mezz1
adc1283_mezz2
adc1283_mezz3
adc1283_mezz4
```

## 9. Avoiding `/__symbols__` overlay warnings

Runtime overlays may print warnings like:

```text
OF: overlay: WARNING: memory leak will occur if overlay removed, property: /__symbols__/mezz0_adc1283_pins
```

This is caused by labels in the overlay DTS combined with `dtc -@`, which emits a `/__symbols__` node. The warning is not caused by the ADC1283 driver.

One practical mitigation is to strip `/__symbols__` from the compiled `.dtbo` before loading it into configfs.

Add this helper to the overlay script:

```sh
strip_overlay_symbols() {
    DTBO_FILE="$1"

    if command -v fdtdel >/dev/null 2>&1; then
        fdtdel "${DTBO_FILE}" /__symbols__ 2>/dev/null || true
        return 0
    fi

    if command -v fdtput >/dev/null 2>&1; then
        fdtput -r "${DTBO_FILE}" /__symbols__ 2>/dev/null || true
        return 0
    fi

    echo "warning: neither fdtdel nor fdtput found; /__symbols__ cannot be stripped" >&2
}
```

Then call it after each `dtc` invocation:

```sh
dtc -@ -I dts -O dtb -o "${BASE_DTBO}" "${BASE_DTS}"
strip_overlay_symbols "${BASE_DTBO}"
```

and:

```sh
dtc -@ -I dts -O dtb -o "${DTBO}" "${DTS}"
strip_overlay_symbols "${DTBO}"
```

If neither `fdtdel` nor `fdtput` is available on the target, either copy the tool into the image, generate/strip the `.dtbo` on the host, or accept the warning during runtime-overlay development.

For production, a static device-tree integration is cleaner than configfs runtime overlays.

## 10. Verify overlay and IIO devices

Check status:

```sh
sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh status all
```

List ADC1283 IIO devices:

```sh
for d in /sys/bus/iio/devices/iio:device*; do
    [ -r "$d/name" ] || continue
    if [ "$(cat "$d/name")" = "adc1283-gpio" ]; then
        echo "---- $d"
        ls "$d"/in_voltage*_raw
    fi
done
```

IIO numbering is not guaranteed to be stable across boots. Do not assume `iio:device1` is always `mezz0`.

Map platform devices to IIO devices:

```sh
for p in /sys/bus/platform/devices/adc1283_mezz*; do
    echo "---- $(basename "$p")"
    find "$p" -maxdepth 4 -type d -name 'iio:device*' -print
 done
```

Read CH0 from all mapped devices:

```sh
for p in /sys/bus/platform/devices/adc1283_mezz*; do
    mezz="$(basename "$p")"
    iio="$(find "$p" -maxdepth 4 -type d -name 'iio:device*' | head -1)"

    echo "---- $mezz -> $iio"
    [ -n "$iio" ] || continue

    printf "CH0 = "
    cat "$iio/in_voltage0_raw"
done
```

Read all channels from one IIO device:

```sh
ADC_DEV=/sys/bus/iio/devices/iio:deviceX

for ch in 0 1 2 3 4 5 6 7; do
    printf "CH%d = " "$ch"
    cat "$ADC_DEV/in_voltage${ch}_raw"
done
```

Replace `iio:deviceX` with the actual device path.

## 11. Interpreting `0` and `4095` with nothing connected

If no ADC1283/mezzanine is connected, or if DOUT is floating, readings like these are expected:

```text
0
4095
```

They are not necessarily valid analog conversions.

The driver samples the DOUT GPIO bits and returns the lower 12 bits of the selected 16-bit word. If DOUT is effectively low, the result tends to `0`. If DOUT is effectively high, the result tends to `4095`.

Check DOUT idle state:

```sh
sudo cat /sys/kernel/debug/gpio | grep -E 'gpio-40|gpio-43|gpio-73|gpio-67|gpio-46'
```

DOUT mapping:

```text
MEZ0 DOUT -> gpio40
MEZ1 DOUT -> gpio43
MEZ2 DOUT -> gpio73
MEZ3 DOUT -> gpio67
MEZ4 DOUT -> gpio46
```

Use the invalid-frame check described above so a floating-high DOUT frame such as `0xffff` returns `-EIO` instead of being silently reported as `4095`.

A stuck-low DOUT line can still look like a valid zero frame, so hardware/scope validation is still required.

## 12. Scope validation procedure

Use MEZ0 first, because this is the already validated reference.

Scope points:

```text
CS   -> gpio38
SCLK -> gpio39
DOUT -> gpio40
DIN  -> gpio50
```

Expected waveform:

```text
SCLK idles high
CS goes low
first real SCLK transition after CS is falling edge
32 SCLK cycles occur
DIN sends channel << 3 during the first 8 cycles
DOUT is sampled during SCLK low
CS returns high after the 32nd clock
```

At the accepted checkpoint, expect approximately:

```text
SCLK high time: 685 ns
SCLK low time:  564 ns
SCLK period:    1249 ns
Frequency:      800.6 kHz
```

The first-cycle low-pulse anomaly should be absent if GPIO priming is present in the driver.

## 13. Tuning rules

Do not retune unless the waveform changes after rebuild/reboot or actual ADC readback fails.

If tuning is required:

```text
If frequency is too low:
    decrease SCLK_LOW_DELAY_NS + SCLK_HIGH_DELAY_NS

If frequency is too high:
    increase SCLK_LOW_DELAY_NS + SCLK_HIGH_DELAY_NS

If LOW is too long:
    decrease SCLK_LOW_DELAY_NS or increase SCLK_HIGH_DELAY_NS

If HIGH is too long:
    decrease SCLK_HIGH_DELAY_NS or increase SCLK_LOW_DELAY_NS
```

Keep:

```text
DOUT_SAMPLE_DELAY_NS <= SCLK_LOW_DELAY_NS
```

Accepted starting point:

```sh
CS_SETUP_DELAY_NS=20
DOUT_SAMPLE_DELAY_NS=100
SCLK_LOW_DELAY_NS=100
SCLK_HIGH_DELAY_NS=360
```

## 14. CPU priority for polling

The kernel module is not a userspace process. The bit-banged read runs in the context of whichever userspace process reads the IIO sysfs file, for example `cat`, a shell loop, Python, or `daphne-server`.

For one-shot or infrequent polling, it is acceptable to use real-time FIFO priority:

```sh
sudo chrt -f 99 cat "$ADC_DEV/in_voltage0_raw"
```

This is safe for infrequent reads because `cat` enters the driver, performs one 32-clock read, prints, and exits. The risk with `SCHED_FIFO` priority 99 is starvation if a process stays runnable continuously or loops without sleeping.

For a polling loop, include a sleep or another blocking operation:

```sh
sudo chrt -f 99 sh -c '
while true; do
    cat "$0/in_voltage0_raw"
    sleep 1
done
' "$ADC_DEV"
```

For all channels:

```sh
sudo chrt -f 99 sh -c '
while true; do
    for ch in 0 1 2 3 4 5 6 7; do
        printf "CH%d=%s " "$ch" "$(cat "$0/in_voltage${ch}_raw")"
    done
    echo
    sleep 1
done
' "$ADC_DEV"
```

Check a running process:

```sh
PID=<pid>
chrt -p "$PID"
ps -o pid,cls,rtprio,ni,psr,comm -p "$PID"
```

This priority mainly reduces scheduling latency before entering the driver. It should not significantly change the timing inside the 32-clock frame when the overlay has:

```dts
daphne,disable-preempt;
daphne,disable-irqs;
```

## 15. CPU frequency governor note

Trying to force `performance` on the target produced `Invalid argument` on all CPU governors. This means the running kernel/cpufreq configuration does not expose or accept the `performance` governor.

Check available governors:

```sh
for d in /sys/devices/system/cpu/cpu*/cpufreq; do
    echo "---- $d"
    echo -n "current: "
    cat "$d/scaling_governor" 2>/dev/null || true
    echo -n "available: "
    cat "$d/scaling_available_governors" 2>/dev/null || true
done
```

If `performance` is not listed, do not assume it can be enabled without rebuilding or reconfiguring the kernel.

If `userspace` is available, the maximum frequency can be forced manually:

```sh
for d in /sys/devices/system/cpu/cpu*/cpufreq; do
    echo userspace > "$d/scaling_governor"
    MAXFREQ=$(cat "$d/scaling_max_freq")
    echo "$MAXFREQ" > "$d/scaling_setspeed"
done
```

## 16. Common troubleshooting

### Overlay applies but no IIO device appears

Check the driver is loaded:

```sh
lsmod | grep adc1283
ls /sys/bus/platform/drivers/adc1283-gpio
```

Check recent logs:

```sh
dmesg | grep -i adc1283 | tail -50
```

### Overlay directory exists but device is not bound

Check actual state:

```sh
for m in 0 1 2 3 4; do
    dev="adc1283_mezz$m"
    ov="/sys/kernel/config/device-tree/overlays/daphne_adc1283_mezz$m"

    echo "---- mezz$m"
    [ -d "$ov" ] && echo "overlay_dir=present" || echo "overlay_dir=missing"
    [ -e "/sys/firmware/devicetree/base/$dev" ] && echo "dt_node=present" || echo "dt_node=missing"
    [ -e "/sys/bus/platform/devices/$dev" ] && echo "platform_device=present" || echo "platform_device=missing"
    [ -e "/sys/bus/platform/drivers/adc1283-gpio/$dev" ] && echo "driver_bound=yes" || echo "driver_bound=no"
done
```

Runtime overlays on this kernel have shown OF refcount/memory-leak warnings after repeated apply/remove cycles. If configfs overlay state becomes inconsistent, reboot before final validation.

### `rmmod adc1283_gpio` says module is in use

Remove active ADC1283 overlays first and close any process reading `in_voltage*_raw`:

```sh
cd <repo-on-target>/scripts
sudo sh apply_mezz_adc1283_gpio_overlays.sh remove all
sudo rmmod adc1283_gpio
```

### `insmod` says invalid module format

The module was built against a different kernel:

```sh
uname -r
modinfo /tmp/adc1283-gpio.ko | grep vermagic
```

### Old timing string still appears

If dmesg still reports `low_tail`, the old module is active or was copied again by mistake:

```sh
strings /tmp/adc1283-gpio.ko | grep -E "low_tail|low=%u|ADC1283 GPIO driver active"
lsmod | grep adc1283
sudo rmmod adc1283_gpio
sudo insmod /tmp/adc1283-gpio.ko
```

### Pinctrl/GPIO ownership

Check that the overlay device owns the relevant pins:

```sh
sudo cat /sys/kernel/debug/gpio | grep -E 'gpio-38|gpio-39|gpio-40|gpio-50|gpio-41|gpio-42|gpio-43|gpio-61|gpio-62|gpio-63|gpio-73|gpio-74|gpio-69|gpio-68|gpio-67|gpio-57|gpio-65|gpio-64|gpio-46|gpio-45'

sudo cat /sys/kernel/debug/pinctrl/firmware:zynqmp-firmware:pinctrl-zynqmp_pinctrl/pinmux-pins | grep -E 'pin 38|pin 39|pin 40|pin 50|pin 41|pin 42|pin 43|pin 61|pin 62|pin 63|pin 73|pin 74|pin 69|pin 68|pin 67|pin 57|pin 65|pin 64|pin 46|pin 45'
```

## 17. Minimal reproduction sequence

Build on the host:

```sh
export KDIR=<path-to-linux-xlnx-build-tree>
export ARCH=arm64
export CROSS_COMPILE=aarch64-xilinx-linux-
export PATH=<path-to-xilinx-cross-toolchain-bin>:$PATH

cd <repo>/kernel/adc1283-gpio
make clean
make KDIR="$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE"
scp adc1283-gpio.ko <user>@<target-host>:/tmp/adc1283-gpio.ko
```

Run on the target:

```sh
cd <repo-on-target>/scripts

sudo sh apply_mezz_adc1283_gpio_overlays.sh remove all 2>/dev/null || true
sudo rmmod adc1283_gpio 2>/dev/null || true
sudo insmod /tmp/adc1283-gpio.ko

CS_SETUP_DELAY_NS=20 \
DOUT_SAMPLE_DELAY_NS=100 \
SCLK_LOW_DELAY_NS=100 \
SCLK_HIGH_DELAY_NS=360 \
sudo -E sh apply_mezz_adc1283_gpio_overlays.sh apply mezz0

sudo sh apply_mezz_adc1283_gpio_overlays.sh status mezz0

dmesg | grep -i "ADC1283 GPIO driver active" | tail -5
```

Map the IIO device and read CH0:

```sh
ADC_DEV=$(for p in /sys/bus/platform/devices/adc1283_mezz0; do
    find "$p" -maxdepth 4 -type d -name 'iio:device*' | head -1
 done)

echo "$ADC_DEV"
sudo chrt -f 99 cat "$ADC_DEV/in_voltage0_raw"
```

Then confirm the accepted waveform on the oscilloscope.

## 18. Current status

Current accepted status:

```text
adc1283-gpio driver compiles out of tree against the Xilinx/PetaLinux kernel.
The module loads on DAPHNE.
MEZ0 overlay binds and exposes an IIO ADC device.
The all-mezzanine overlay script can apply all five mezzanines with independent overlay directories.
All five platform devices can bind simultaneously:
  adc1283_mezz0
  adc1283_mezz1
  adc1283_mezz2
  adc1283_mezz3
  adc1283_mezz4
The SCLK waveform was scope-tested on MEZ0.
Accepted SCLK timing is approximately 800.6 kHz.
Duty cycle is inside the ADC1283 40% to 60% requirement.
First-cycle low-pulse anomaly is addressed by GPIO priming before CS assertion.
DIN toggling before SCLK falling edge is accepted.
Invalid-frame detection is recommended so floating-high DOUT does not appear as valid 4095.
Next work: validate actual ADC1283 conversion readback values on all channels and all mezzanine ports with real hardware connected.
```