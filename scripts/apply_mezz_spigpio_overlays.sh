#!/bin/sh
#
# Create and apply a runtime spi-gpio device-tree overlay for all DAPHNE
# mezzanine MIO SPI-style ports.
#
# Usage:
#   sudo ./apply_mezz_spigpio_overlays.sh apply
#   sudo ./apply_mezz_spigpio_overlays.sh status
#   sudo ./apply_mezz_spigpio_overlays.sh remove
#
# Optional:
#   SPI_MAX_FREQUENCY=1000000 sudo ./apply_mezz_spigpio_overlays.sh apply

set -eu

ACTION="${1:-apply}"
OVERLAY_NAME="${OVERLAY_NAME:-daphne_mezz_spigpio}"
OVERLAY_ROOT="/sys/kernel/config/device-tree/overlays"
OVERLAY_DIR="${OVERLAY_ROOT}/${OVERLAY_NAME}"
DTS="${DTS:-/tmp/${OVERLAY_NAME}.dtso}"
DTBO="${DTBO:-/tmp/${OVERLAY_NAME}.dtbo}"
SPI_MAX_FREQUENCY="${SPI_MAX_FREQUENCY:-1000000}"
PINCTRL_DEBUG="/sys/kernel/debug/pinctrl/firmware:zynqmp-firmware:pinctrl-zynqmp_pinctrl/pinmux-pins"
KNOWN_PIN_OWNING_TEST_OVERLAYS="mezz0_spigpio mezz0_sclk_led mezz_gpio63_led mezz_gpio63"

MEZZ_GPIOS_PATTERN="gpio-38|gpio-39|gpio-40|gpio-50|gpio-41|gpio-42|gpio-43|gpio-61|gpio-62|gpio-63|gpio-73|gpio-74|gpio-69|gpio-68|gpio-67|gpio-57|gpio-65|gpio-64|gpio-46|gpio-45"
MEZZ_PINS_PATTERN="pin 38|pin 39|pin 40|pin 50|pin 41|pin 42|pin 43|pin 61|pin 62|pin 63|pin 73|pin 74|pin 69|pin 68|pin 67|pin 57|pin 65|pin 64|pin 46|pin 45"

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "error: run as root, for example with sudo" >&2
        exit 1
    fi
}

check_kernel_config() {
    if [ -r /proc/config.gz ]; then
        if zcat /proc/config.gz | grep -q '^# CONFIG_SPI_GPIO is not set'; then
            echo "error: CONFIG_SPI_GPIO is not enabled in this kernel" >&2
            echo "       enable Device Drivers -> SPI support -> GPIO-based bitbanging SPI Master" >&2
            exit 1
        fi
        if zcat /proc/config.gz | grep -q '^CONFIG_SPI_GPIO=m'; then
            modprobe spi-gpio 2>/dev/null || true
        fi
        if zcat /proc/config.gz | grep -q '^CONFIG_SPI_SPIDEV=m'; then
            modprobe spidev 2>/dev/null || true
        fi
    else
        echo "warning: /proc/config.gz is unavailable; cannot verify CONFIG_SPI_GPIO" >&2
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

check_conflicting_overlays() {
    for name in ${KNOWN_PIN_OWNING_TEST_OVERLAYS}; do
        if [ -d "${OVERLAY_ROOT}/${name}" ]; then
            echo "error: conflicting test overlay is active: ${OVERLAY_ROOT}/${name}" >&2
            echo "       remove it first, for example:" >&2
            echo "       rmdir ${OVERLAY_ROOT}/${name}" >&2
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
        target-path = "/axi/usb@ff9d0000";
        __overlay__ {
            status = "disabled";
        };
    };

    fragment@2 {
        target-path = "/axi/usb@ff9d0000/usb@fe200000";
        __overlay__ {
            status = "disabled";
        };
    };

    fragment@3 {
        target-path = "/axi/usb@ff9e0000";
        __overlay__ {
            status = "disabled";
        };
    };

    fragment@4 {
        target-path = "/axi/usb@ff9e0000/usb@fe300000";
        __overlay__ {
            status = "disabled";
        };
    };

    fragment@5 {
        target-path = "/firmware/zynqmp-firmware/pinctrl";
        __overlay__ {
            mezz0_spi_pins: mezz0-spi-pins {
                mux {
                    groups = "gpio0_38_grp", "gpio0_39_grp",
                             "gpio0_40_grp", "gpio0_50_grp";
                    function = "gpio0";
                };
            };

            mezz1_spi_pins: mezz1-spi-pins {
                mux {
                    groups = "gpio0_41_grp", "gpio0_42_grp",
                             "gpio0_43_grp", "gpio0_61_grp";
                    function = "gpio0";
                };
            };

            mezz2_spi_pins: mezz2-spi-pins {
                mux {
                    groups = "gpio0_62_grp", "gpio0_63_grp",
                             "gpio0_73_grp", "gpio0_74_grp";
                    function = "gpio0";
                };
            };

            mezz3_spi_pins: mezz3-spi-pins {
                mux {
                    groups = "gpio0_69_grp", "gpio0_68_grp",
                             "gpio0_67_grp", "gpio0_57_grp";
                    function = "gpio0";
                };
            };

            mezz4_spi_pins: mezz4-spi-pins {
                mux {
                    groups = "gpio0_65_grp", "gpio0_64_grp",
                             "gpio0_46_grp", "gpio0_45_grp";
                    function = "gpio0";
                };
            };
        };
    };

    fragment@6 {
        target-path = "/";
        __overlay__ {
            spi_mezz0 {
                compatible = "spi-gpio";
                pinctrl-names = "default";
                pinctrl-0 = <&mezz0_spi_pins>;

                sck-gpios  = <&gpio 39 0>;
                mosi-gpios = <&gpio 50 0>;
                miso-gpios = <&gpio 40 0>;
                cs-gpios   = <&gpio 38 0>;

                num-chipselects = <1>;
                #address-cells = <1>;
                #size-cells = <0>;

                spidev@0 {
                    compatible = "rohm,dh2228fv";
                    reg = <0>;
                    spi-max-frequency = <${SPI_MAX_FREQUENCY}>;
                };
            };

            spi_mezz1 {
                compatible = "spi-gpio";
                pinctrl-names = "default";
                pinctrl-0 = <&mezz1_spi_pins>;

                sck-gpios  = <&gpio 42 0>;
                mosi-gpios = <&gpio 61 0>;
                miso-gpios = <&gpio 43 0>;
                cs-gpios   = <&gpio 41 0>;

                num-chipselects = <1>;
                #address-cells = <1>;
                #size-cells = <0>;

                spidev@0 {
                    compatible = "rohm,dh2228fv";
                    reg = <0>;
                    spi-max-frequency = <${SPI_MAX_FREQUENCY}>;
                };
            };

            spi_mezz2 {
                compatible = "spi-gpio";
                pinctrl-names = "default";
                pinctrl-0 = <&mezz2_spi_pins>;

                sck-gpios  = <&gpio 63 0>;
                mosi-gpios = <&gpio 74 0>;
                miso-gpios = <&gpio 73 0>;
                cs-gpios   = <&gpio 62 0>;

                num-chipselects = <1>;
                #address-cells = <1>;
                #size-cells = <0>;

                spidev@0 {
                    compatible = "rohm,dh2228fv";
                    reg = <0>;
                    spi-max-frequency = <${SPI_MAX_FREQUENCY}>;
                };
            };

            spi_mezz3 {
                compatible = "spi-gpio";
                pinctrl-names = "default";
                pinctrl-0 = <&mezz3_spi_pins>;

                sck-gpios  = <&gpio 68 0>;
                mosi-gpios = <&gpio 57 0>;
                miso-gpios = <&gpio 67 0>;
                cs-gpios   = <&gpio 69 0>;

                num-chipselects = <1>;
                #address-cells = <1>;
                #size-cells = <0>;

                spidev@0 {
                    compatible = "rohm,dh2228fv";
                    reg = <0>;
                    spi-max-frequency = <${SPI_MAX_FREQUENCY}>;
                };
            };

            spi_mezz4 {
                compatible = "spi-gpio";
                pinctrl-names = "default";
                pinctrl-0 = <&mezz4_spi_pins>;

                sck-gpios  = <&gpio 64 0>;
                mosi-gpios = <&gpio 45 0>;
                miso-gpios = <&gpio 46 0>;
                cs-gpios   = <&gpio 65 0>;

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
    check_kernel_config
    check_conflicting_overlays

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
    echo "[spidev]"
    ls -l /dev/spidev* 2>/dev/null || echo "<none>"

    echo
    echo "[spi masters]"
    ls -l /sys/class/spi_master 2>/dev/null || echo "<none>"

    echo
    echo "[spi devices]"
    ls -l /sys/bus/spi/devices 2>/dev/null || echo "<none>"

    if [ -r "${PINCTRL_DEBUG}" ]; then
        echo
        echo "[pinctrl mezz pins]"
        grep -E "${MEZZ_PINS_PATTERN}" "${PINCTRL_DEBUG}" || true
    fi

    if [ -r /sys/kernel/debug/gpio ]; then
        echo
        echo "[gpio mezz lines]"
        grep -E "${MEZZ_GPIOS_PATTERN}" /sys/kernel/debug/gpio || true
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
