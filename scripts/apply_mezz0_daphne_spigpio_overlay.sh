#!/bin/sh
#
# Create and apply a runtime overlay for the DAPHNE custom calibrated MEZ0
# GPIO SPI master.
#
# Usage:
#   sudo sh scripts/apply_mezz0_daphne_spigpio_overlay.sh apply
#   sudo sh scripts/apply_mezz0_daphne_spigpio_overlay.sh status
#   sudo sh scripts/apply_mezz0_daphne_spigpio_overlay.sh remove
#
# Tunables:
#   HIGH_DELAY_NS=250 LOW_DELAY_NS=250 SETUP_DELAY_NS=0 sudo sh ... apply

set -eu

ACTION="${1:-apply}"
OVERLAY_NAME="${OVERLAY_NAME:-daphne_mezz0_custom_spigpio}"
OVERLAY_ROOT="/sys/kernel/config/device-tree/overlays"
OVERLAY_DIR="${OVERLAY_ROOT}/${OVERLAY_NAME}"
DTS="${DTS:-/tmp/${OVERLAY_NAME}.dtso}"
DTBO="${DTBO:-/tmp/${OVERLAY_NAME}.dtbo}"
SPI_MAX_FREQUENCY="${SPI_MAX_FREQUENCY:-800000}"
HIGH_DELAY_NS="${HIGH_DELAY_NS:-250}"
LOW_DELAY_NS="${LOW_DELAY_NS:-250}"
SETUP_DELAY_NS="${SETUP_DELAY_NS:-0}"
CRITICAL_MAX_BYTES="${CRITICAL_MAX_BYTES:-4}"
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
    modprobe spi-daphne-mezz-gpio 2>/dev/null || true

    if [ ! -d /sys/bus/platform/drivers/daphne-mezz-spi-gpio ]; then
        echo "warning: daphne-mezz-spi-gpio platform driver is not visible" >&2
        echo "         load spi-daphne-mezz-gpio.ko or boot a kernel with the driver built in" >&2
    fi
}

check_conflicts() {
    for name in mezz0_spigpio mezz0_sclk_led daphne_mezz_spigpio; do
        if [ -d "${OVERLAY_ROOT}/${name}" ]; then
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
            mezz0_spi_pins: mezz0-spi-pins {
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
            spi_mezz0 {
                compatible = "daphne,mezz-spi-gpio";
                pinctrl-names = "default";
                pinctrl-0 = <&mezz0_spi_pins>;

                sck-gpios  = <&gpio 39 0>;
                mosi-gpios = <&gpio 50 0>;
                miso-gpios = <&gpio 40 0>;
                cs-gpios   = <&gpio 38 0>;

                daphne,sclk-high-delay-ns = <${HIGH_DELAY_NS}>;
                daphne,sclk-low-delay-ns = <${LOW_DELAY_NS}>;
                daphne,mosi-setup-delay-ns = <${SETUP_DELAY_NS}>;
                daphne,critical-max-bytes = <${CRITICAL_MAX_BYTES}>;
                daphne,disable-preempt;
                daphne,disable-irqs;

                num-chipselects = <1>;
                #address-cells = <1>;
                #size-cells = <0>;

                spidev@0 {
                    compatible = "rohm,dh2228fv";
                    reg = <0>;
                    spi-max-frequency = <${SPI_MAX_FREQUENCY}>;
                };
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

status_overlay() {
    echo "overlay_dir=${OVERLAY_DIR}"
    if [ -d "${OVERLAY_DIR}" ]; then
        echo "overlay_active=True"
    else
        echo "overlay_active=False"
    fi

    echo
    echo "[driver]"
    if [ -d /sys/bus/platform/drivers/daphne-mezz-spi-gpio ]; then
        echo "daphne-mezz-spi-gpio=present"
    else
        echo "daphne-mezz-spi-gpio=<missing>"
    fi

    echo
    echo "[spidev]"
    ls -l /dev/spidev* 2>/dev/null || echo "<none>"

    echo
    echo "[spi devices]"
    ls -l /sys/bus/spi/devices 2>/dev/null || echo "<none>"

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
