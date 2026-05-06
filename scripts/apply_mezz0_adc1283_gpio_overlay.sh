#!/bin/sh
#
# Create and apply a runtime overlay for the DAPHNE MEZ0 ADC1283
# GPIO/IIO driver.
#
# Usage:
#   sudo sh scripts/apply_mezz0_adc1283_gpio_overlay.sh apply
#   sudo sh scripts/apply_mezz0_adc1283_gpio_overlay.sh status
#   sudo sh scripts/apply_mezz0_adc1283_gpio_overlay.sh remove
#
# Tunables:
#   TARGET_SCLK_HZ=800000 \
#   SCLK_HIGH_DELAY_NS=350 \
#   DOUT_SAMPLE_DELAY_NS=250 \
#   SCLK_LOW_TAIL_DELAY_NS=150 \
#   sudo -E sh scripts/apply_mezz0_adc1283_gpio_overlay.sh apply

set -eu

ACTION="${1:-apply}"

OVERLAY_NAME="${OVERLAY_NAME:-daphne_mezz0_adc1283_gpio}"
OVERLAY_ROOT="/sys/kernel/config/device-tree/overlays"
OVERLAY_DIR="${OVERLAY_ROOT}/${OVERLAY_NAME}"
DTS="${DTS:-/tmp/${OVERLAY_NAME}.dtso}"
DTBO="${DTBO:-/tmp/${OVERLAY_NAME}.dtbo}"

TARGET_SCLK_HZ="${TARGET_SCLK_HZ:-800000}"
CS_SETUP_DELAY_NS="${CS_SETUP_DELAY_NS:-100}"
CS_HOLD_DELAY_NS="${CS_HOLD_DELAY_NS:-100}"

# DOUT sample point after SCLK falling edge.
# This is inside the low phase, not an extra low delay.
DOUT_SAMPLE_DELAY_NS="${DOUT_SAMPLE_DELAY_NS:-150}"

# Total programmed low-phase delay budget.
# The driver waits:
#   DOUT_SAMPLE_DELAY_NS
#   read DOUT
#   SCLK_LOW_DELAY_NS - DOUT_SAMPLE_DELAY_NS
SCLK_LOW_DELAY_NS="${SCLK_LOW_DELAY_NS:-350}"

# Programmed high-phase delay budget.
SCLK_HIGH_DELAY_NS="${SCLK_HIGH_DELAY_NS:-350}"

VREF_UV="${VREF_UV:-3300000}"

PINCTRL_DEBUG="/sys/kernel/debug/pinctrl/firmware:zynqmp-firmware:pinctrl-zynqmp_pinctrl/pinmux-pins"

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "error: run as root, for example with sudo" >&2
        exit 1
    fi
}

check_tools() {
    command -v dtc >/dev/null 2>&1 || {
        echo "error: dtc not found" >&2
        exit 1
    }

    [ -d "${OVERLAY_ROOT}" ] || {
        echo "error: ${OVERLAY_ROOT} does not exist; configfs overlays are unavailable" >&2
        exit 1
    }
}

check_driver() {
    modprobe adc1283-gpio 2>/dev/null || true

    if [ ! -d /sys/bus/platform/drivers/adc1283-gpio ]; then
        echo "warning: adc1283-gpio platform driver is not visible" >&2
        echo "         load adc1283-gpio.ko or boot a kernel with the driver built in" >&2
    fi
}

check_conflicts() {
    for name in \
        mezz0_spigpio \
        mezz0_sclk_led \
        daphne_mezz_spigpio \
        daphne_mezz0_custom_spigpio \
        daphne_mezz0_adc1283_gpio
    do
        if [ "${name}" != "${OVERLAY_NAME}" ] && [ -d "${OVERLAY_ROOT}/${name}" ]; then
            echo "error: conflicting overlay is active: ${OVERLAY_ROOT}/${name}" >&2
            echo "       remove it first with: rmdir ${OVERLAY_ROOT}/${name}" >&2
            exit 1
        fi
    done
}

write_dts() {
    cat >"${DTS}" <<EOF
/dts-v1/;
/plugin/;

/ {
    fragment@0 {
        target-path = "/axi/ethernet@ff0c0000";
        __overlay__ {
            status = "disabled";
        };
    };

    fragment@1 {
        target-path = "/firmware/zynqmp-firmware/pinctrl";
        __overlay__ {
            mezz0_adc1283_pins: mezz0-adc1283-pins {
                mux {
                    groups = "gpio0_38_grp", "gpio0_39_grp",
                             "gpio0_40_grp", "gpio0_50_grp";
                    function = "gpio0";
                };
            };
        };
    };

    fragment@2 {
        target-path = "/";
        __overlay__ {
            adc1283_mezz0 {
                compatible = "daphne,adc1283-gpio";
                pinctrl-names = "default";
                pinctrl-0 = <&mezz0_adc1283_pins>;

                /*
                 * MEZ0 mapping:
                 *   CS   -> MIO38 -> gpio38, active low
                 *   SCLK -> MIO39 -> gpio39
                 *   DOUT -> MIO40 -> gpio40
                 *   DIN  -> MIO50 -> gpio50
                 */
                cs-gpios   = <&gpio 38 1>;
                sclk-gpios = <&gpio 39 0>;
                dout-gpios = <&gpio 40 0>;
                din-gpios  = <&gpio 50 0>;

                daphne,target-sclk-hz = <${TARGET_SCLK_HZ}>;
                daphne,cs-setup-delay-ns = <${CS_SETUP_DELAY_NS}>;
                daphne,cs-hold-delay-ns = <${CS_HOLD_DELAY_NS}>;
                daphne,dout-sample-delay-ns = <${DOUT_SAMPLE_DELAY_NS}>;
                daphne,sclk-low-tail-delay-ns = <${SCLK_LOW_TAIL_DELAY_NS}>;
                daphne,sclk-high-delay-ns = <${SCLK_HIGH_DELAY_NS}>;

                daphne,disable-preempt;
                daphne,disable-irqs;

                /*
                 * ADC1283 uses AVCC as reference. Replace this with
                 * vref-supply later if the board has a regulator node.
                 */
                daphne,vref-microvolt = <${VREF_UV}>;
            };
        };
    };
};
EOF
}

apply_overlay() {
    require_root
    check_tools
    check_driver
    check_conflicts

    if [ -d "${OVERLAY_DIR}" ]; then
        echo "overlay already active: ${OVERLAY_DIR}"
        status_overlay
        return 0
    fi

    write_dts
    dtc -@ -I dts -O dtb -o "${DTBO}" "${DTS}"

    mkdir "${OVERLAY_DIR}"
    if ! cat "${DTBO}" >"${OVERLAY_DIR}/dtbo"; then
        rmdir "${OVERLAY_DIR}" 2>/dev/null || true
        exit 1
    fi

    echo "loaded ${OVERLAY_NAME}"
    status_overlay
}

remove_overlay() {
    require_root

    if [ ! -d "${OVERLAY_DIR}" ]; then
        echo "overlay not active: ${OVERLAY_DIR}"
        return 0
    fi

    rmdir "${OVERLAY_DIR}"
    echo "removed ${OVERLAY_NAME}"
}

find_iio_adc1283() {
    for dev in /sys/bus/iio/devices/iio:device*; do
        [ -e "${dev}/name" ] || continue
        if [ "$(cat "${dev}/name" 2>/dev/null)" = "adc1283-gpio" ]; then
            echo "${dev}"
            return 0
        fi
    done

    return 1
}

status_overlay() {
    echo "overlay_dir=${OVERLAY_DIR}"
    if [ -d "${OVERLAY_DIR}" ]; then
        echo "overlay_active=True"
    else
        echo "overlay_active=False"
    fi

    echo
    echo "[driver]"
    if [ -d /sys/bus/platform/drivers/adc1283-gpio ]; then
        echo "adc1283-gpio=present"
    else
        echo "adc1283-gpio=<missing>"
    fi

    echo
    echo "[iio]"
    IIO_DEV="$(find_iio_adc1283 2>/dev/null || true)"
    if [ -n "${IIO_DEV}" ]; then
        echo "adc1283_iio=${IIO_DEV}"
        ls "${IIO_DEV}"/in_voltage*_raw 2>/dev/null || true
        [ -r "${IIO_DEV}/in_voltage_scale" ] && {
            echo -n "scale="
            cat "${IIO_DEV}/in_voltage_scale"
        }
    else
        echo "adc1283_iio=<none>"
        ls -l /sys/bus/iio/devices 2>/dev/null || true
    fi

    if [ -r "${PINCTRL_DEBUG}" ]; then
        echo
        echo "[pinctrl MEZ0]"
        grep -E 'pin 38|pin 39|pin 40|pin 50' "${PINCTRL_DEBUG}" || true
    fi

    if [ -r /sys/kernel/debug/gpio ]; then
        echo
        echo "[gpio MEZ0]"
        grep -E 'gpio-38|gpio-39|gpio-40|gpio-50' /sys/kernel/debug/gpio || true
    fi
}

case "${ACTION}" in
    apply)
        apply_overlay
        ;;
    remove)
        remove_overlay
        ;;
    status)
        status_overlay
        ;;
    *)
        echo "usage: $0 [apply|remove|status]" >&2
        exit 2
        ;;
esac