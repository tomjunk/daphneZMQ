#!/bin/sh
#
# Runtime ADC1283 GPIO/IIO overlays for selected DAPHNE mezzanine ports.
#
# Usage:
#   sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh apply
#   sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh apply mezz0
#   sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh apply mezz0 mezz3
#   sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh remove mezz0
#   sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh remove all
#   sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh status
#   sudo sh scripts/apply_mezz_adc1283_gpio_overlays.sh status all
#
# Accepted mezz names:
#   mezz0 mezz1 mezz2 mezz3 mezz4 all
#
# Tunables:
#   TARGET_SCLK_HZ=800000 \
#   CS_SETUP_DELAY_NS=20 \
#   CS_HOLD_DELAY_NS=100 \
#   DOUT_SAMPLE_DELAY_NS=100 \
#   SCLK_LOW_DELAY_NS=100 \
#   SCLK_HIGH_DELAY_NS=360 \
#   DIN_HOLD_DELAY_NS=20 \
#   PRECLOCK_CYCLES=2 \
#   VREF_UV=3300000 \
#   sudo -E sh scripts/apply_mezz_adc1283_gpio_overlays.sh apply mezz0 mezz3

set -eu

ACTION="${1:-apply}"
if [ "$#" -gt 0 ]; then
    shift
fi

OVERLAY_ROOT="/sys/kernel/config/device-tree/overlays"

BASE_OVERLAY_NAME="${BASE_OVERLAY_NAME:-daphne_adc1283_mezz_base}"
BASE_OVERLAY_DIR="${OVERLAY_ROOT}/${BASE_OVERLAY_NAME}"
BASE_DTS="${BASE_DTS:-/tmp/${BASE_OVERLAY_NAME}.dtso}"
BASE_DTBO="${BASE_DTBO:-/tmp/${BASE_OVERLAY_NAME}.dtbo}"

MEZZ_OVERLAY_PREFIX="${MEZZ_OVERLAY_PREFIX:-daphne_adc1283}"

TARGET_SCLK_HZ="${TARGET_SCLK_HZ:-800000}"
CS_SETUP_DELAY_NS="${CS_SETUP_DELAY_NS:-20}"
CS_HOLD_DELAY_NS="${CS_HOLD_DELAY_NS:-100}"
DOUT_SAMPLE_DELAY_NS="${DOUT_SAMPLE_DELAY_NS:-100}"
SCLK_LOW_DELAY_NS="${SCLK_LOW_DELAY_NS:-100}"
SCLK_HIGH_DELAY_NS="${SCLK_HIGH_DELAY_NS:-360}"
DIN_HOLD_DELAY_NS="${DIN_HOLD_DELAY_NS:-20}"
PRECLOCK_CYCLES="${PRECLOCK_CYCLES:-2}"
VREF_UV="${VREF_UV:-3300000}"

PINCTRL_DEBUG="/sys/kernel/debug/pinctrl/firmware:zynqmp-firmware:pinctrl-zynqmp_pinctrl/pinmux-pins"

ALL_MEZZ="mezz0 mezz1 mezz2 mezz3 mezz4"

MEZZ_GPIOS_PATTERN="gpio-38|gpio-39|gpio-40|gpio-50|gpio-41|gpio-42|gpio-43|gpio-61|gpio-62|gpio-63|gpio-73|gpio-74|gpio-69|gpio-68|gpio-67|gpio-57|gpio-65|gpio-64|gpio-46|gpio-45"
MEZZ_PINS_PATTERN="pin 38|pin 39|pin 40|pin 50|pin 41|pin 42|pin 43|pin 61|pin 62|pin 63|pin 73|pin 74|pin 69|pin 68|pin 67|pin 57|pin 65|pin 64|pin 46|pin 45"

KNOWN_CONFLICTING_OVERLAYS="
mezz0_spigpio
mezz0_sclk_led
mezz_gpio63_led
mezz_gpio63
daphne_mezz_spigpio
daphne_mezz0_custom_spigpio
daphne_mezz0_adc1283_gpio
daphne_mezz_adc1283_gpio
daphne_mezz0_custom_spigpio
"

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
    modprobe adc1283-gpio 2>/dev/null || modprobe adc1283_gpio 2>/dev/null || true

    if [ ! -d /sys/bus/platform/drivers/adc1283-gpio ]; then
        echo "warning: adc1283-gpio platform driver is not visible" >&2
        echo "         load adc1283-gpio.ko or boot a kernel with the driver built in" >&2
    fi
}

normalize_mezz_list() {
    if [ "$#" -eq 0 ]; then
        set -- all
    fi

    OUT=""

    for arg in "$@"; do
        case "${arg}" in
            all)
                OUT="${OUT} ${ALL_MEZZ}"
                ;;
            0|mezz0)
                OUT="${OUT} mezz0"
                ;;
            1|mezz1)
                OUT="${OUT} mezz1"
                ;;
            2|mezz2)
                OUT="${OUT} mezz2"
                ;;
            3|mezz3)
                OUT="${OUT} mezz3"
                ;;
            4|mezz4)
                OUT="${OUT} mezz4"
                ;;
            *)
                echo "error: invalid mezzanine '${arg}'" >&2
                echo "       valid: mezz0 mezz1 mezz2 mezz3 mezz4 all" >&2
                exit 2
                ;;
        esac
    done

    echo "${OUT}"
}

mezz_overlay_name() {
    echo "${MEZZ_OVERLAY_PREFIX}_${1}"
}

mezz_overlay_dir() {
    echo "${OVERLAY_ROOT}/$(mezz_overlay_name "$1")"
}

adc_node_name() {
    echo "adc1283_${1}"
}

check_conflicting_overlays() {
    for name in ${KNOWN_CONFLICTING_OVERLAYS}; do
        if [ -d "${OVERLAY_ROOT}/${name}" ]; then
            echo "error: conflicting overlay is active: ${OVERLAY_ROOT}/${name}" >&2
            echo "       remove it first, for example:" >&2
            echo "       rmdir ${OVERLAY_ROOT}/${name}" >&2
            exit 1
        fi
    done
}

set_mezz_params() {
    MEZZ="$1"

    case "${MEZZ}" in
        mezz0)
            CS_GPIO=38
            SCLK_GPIO=39
            DOUT_GPIO=40
            DIN_GPIO=50
            PINCTRL_GROUPS='"gpio0_38_grp", "gpio0_39_grp", "gpio0_40_grp", "gpio0_50_grp"'
            ;;
        mezz1)
            CS_GPIO=41
            SCLK_GPIO=42
            DOUT_GPIO=43
            DIN_GPIO=61
            PINCTRL_GROUPS='"gpio0_41_grp", "gpio0_42_grp", "gpio0_43_grp", "gpio0_61_grp"'
            ;;
        mezz2)
            CS_GPIO=62
            SCLK_GPIO=63
            DOUT_GPIO=73
            DIN_GPIO=74
            PINCTRL_GROUPS='"gpio0_62_grp", "gpio0_63_grp", "gpio0_73_grp", "gpio0_74_grp"'
            ;;
        mezz3)
            CS_GPIO=69
            SCLK_GPIO=68
            DOUT_GPIO=67
            DIN_GPIO=57
            PINCTRL_GROUPS='"gpio0_69_grp", "gpio0_68_grp", "gpio0_67_grp", "gpio0_57_grp"'
            ;;
        mezz4)
            CS_GPIO=65
            SCLK_GPIO=64
            DOUT_GPIO=46
            DIN_GPIO=45
            PINCTRL_GROUPS='"gpio0_65_grp", "gpio0_64_grp", "gpio0_46_grp", "gpio0_45_grp"'
            ;;
        *)
            echo "error: internal invalid mezz '${MEZZ}'" >&2
            exit 2
            ;;
    esac

    PIN_LABEL="${MEZZ}_adc1283_pins"
    PIN_NODE="${MEZZ}-adc1283-pins"
    ADC_NODE="$(adc_node_name "${MEZZ}")"
}

write_base_dts() {
    cat >"${BASE_DTS}" <<EOF
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
};
EOF
}

apply_base_overlay() {
    if [ -d "${BASE_OVERLAY_DIR}" ]; then
        return 0
    fi

    write_base_dts
    dtc -@ -I dts -O dtb -o "${BASE_DTBO}" "${BASE_DTS}"
    strip_overlay_symbols "${BASE_DTBO}"

    mkdir "${BASE_OVERLAY_DIR}"
    if ! cat "${BASE_DTBO}" >"${BASE_OVERLAY_DIR}/dtbo"; then
        rmdir "${BASE_OVERLAY_DIR}" 2>/dev/null || true
        exit 1
    fi

    echo "loaded base overlay ${BASE_OVERLAY_NAME}"
}

write_mezz_dts() {
    MEZZ="$1"
    set_mezz_params "${MEZZ}"

    OVERLAY_NAME="$(mezz_overlay_name "${MEZZ}")"
    DTS="/tmp/${OVERLAY_NAME}.dtso"

    cat >"${DTS}" <<EOF
/dts-v1/;
/plugin/;

/ {
    fragment@0 {
        target-path = "/firmware/zynqmp-firmware/pinctrl";
        __overlay__ {
            ${PIN_LABEL}: ${PIN_NODE} {
                mux {
                    groups = ${PINCTRL_GROUPS};
                    function = "gpio0";
                };
            };
        };
    };

    fragment@1 {
        target-path = "/";
        __overlay__ {
            ${ADC_NODE} {
                compatible = "daphne,adc1283-gpio";
                pinctrl-names = "default";
                pinctrl-0 = <&${PIN_LABEL}>;

                /*
                 * ${MEZZ} mapping:
                 *   CS   -> gpio${CS_GPIO}, active low
                 *   SCLK -> gpio${SCLK_GPIO}
                 *   DOUT -> gpio${DOUT_GPIO}
                 *   DIN  -> gpio${DIN_GPIO}
                 */
                cs-gpios   = <&gpio ${CS_GPIO} 1>;
                sclk-gpios = <&gpio ${SCLK_GPIO} 0>;
                dout-gpios = <&gpio ${DOUT_GPIO} 0>;
                din-gpios  = <&gpio ${DIN_GPIO} 0>;

                daphne,target-sclk-hz = <${TARGET_SCLK_HZ}>;
                daphne,cs-setup-delay-ns = <${CS_SETUP_DELAY_NS}>;
                daphne,cs-hold-delay-ns = <${CS_HOLD_DELAY_NS}>;
                daphne,dout-sample-delay-ns = <${DOUT_SAMPLE_DELAY_NS}>;
                daphne,sclk-low-delay-ns = <${SCLK_LOW_DELAY_NS}>;
                daphne,sclk-high-delay-ns = <${SCLK_HIGH_DELAY_NS}>;
                daphne,din-hold-delay-ns = <${DIN_HOLD_DELAY_NS}>;
                daphne,preclock-cycles = <${PRECLOCK_CYCLES}>;

                daphne,disable-preempt;
                daphne,disable-irqs;

                daphne,vref-microvolt = <${VREF_UV}>;
            };
        };
    };
};
EOF
}

platform_device_exists() {
    [ -e "/sys/bus/platform/devices/$1" ]
}

platform_device_bound() {
    [ -e "/sys/bus/platform/drivers/adc1283-gpio/$1" ]
}

bind_platform_device_if_needed() {
    ADC_NODE="$1"

    if platform_device_bound "${ADC_NODE}"; then
        return 0
    fi

    if platform_device_exists "${ADC_NODE}"; then
        echo "${ADC_NODE}: platform device exists but is not bound; trying bind"
        echo "${ADC_NODE}" > /sys/bus/platform/drivers/adc1283-gpio/bind 2>/dev/null || true
    fi
}

verify_mezz_bound() {
    MEZZ="$1"
    ADC_NODE="$(adc_node_name "${MEZZ}")"

    sleep 0.2

    if [ ! -e "/sys/bus/platform/devices/${ADC_NODE}" ]; then
        echo "error: ${MEZZ}: platform device ${ADC_NODE} was not created" >&2
        echo "[recent kernel messages]" >&2
        dmesg | grep -Ei "adc1283|${ADC_NODE}|pinctrl|gpio|overlay" | tail -60 >&2 || true
        exit 1
    fi

    if [ ! -e "/sys/bus/platform/drivers/adc1283-gpio/${ADC_NODE}" ]; then
        echo "error: ${MEZZ}: platform device ${ADC_NODE} exists but is not bound" >&2
        echo "[recent kernel messages]" >&2
        dmesg | grep -Ei "adc1283|${ADC_NODE}|pinctrl|gpio|overlay" | tail -60 >&2 || true
        exit 1
    fi
}

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

apply_mezz_overlay() {
    MEZZ="$1"
    OVERLAY_NAME="$(mezz_overlay_name "${MEZZ}")"
    OVERLAY_DIR="$(mezz_overlay_dir "${MEZZ}")"
    DTS="/tmp/${OVERLAY_NAME}.dtso"
    DTBO="/tmp/${OVERLAY_NAME}.dtbo"
    ADC_NODE="$(adc_node_name "${MEZZ}")"

    if [ -d "${OVERLAY_DIR}" ]; then
        if platform_device_bound "${ADC_NODE}"; then
            echo "${MEZZ}: already active at ${OVERLAY_DIR}"
            return 0
        fi

        bind_platform_device_if_needed "${ADC_NODE}"

        if platform_device_bound "${ADC_NODE}"; then
            echo "${MEZZ}: overlay was active and device is now bound"
            return 0
        fi

        echo "${MEZZ}: stale overlay directory exists without bound device; removing ${OVERLAY_DIR}"
        rmdir "${OVERLAY_DIR}" 2>/dev/null || {
            echo "error: could not remove stale overlay ${OVERLAY_DIR}" >&2
            echo "       reboot is recommended to clear inconsistent configfs overlay state" >&2
            exit 1
        }
    fi

    write_mezz_dts "${MEZZ}"
    dtc -@ -I dts -O dtb -o "${DTBO}" "${DTS}"
    strip_overlay_symbols "${DTBO}"

    mkdir "${OVERLAY_DIR}"
    if ! cat "${DTBO}" >"${OVERLAY_DIR}/dtbo"; then
        rmdir "${OVERLAY_DIR}" 2>/dev/null || true
        exit 1
    fi

    echo "${MEZZ}: loaded ${OVERLAY_NAME}"
    verify_mezz_bound "${MEZZ}"
}

remove_mezz_overlay() {
    MEZZ="$1"
    OVERLAY_NAME="$(mezz_overlay_name "${MEZZ}")"
    OVERLAY_DIR="$(mezz_overlay_dir "${MEZZ}")"
    ADC_NODE="$(adc_node_name "${MEZZ}")"

    if [ -d "${OVERLAY_DIR}" ]; then
        if rmdir "${OVERLAY_DIR}"; then
            echo "${MEZZ}: removed ${OVERLAY_NAME}"
        else
            echo "warning: ${MEZZ}: could not remove ${OVERLAY_DIR}" >&2
            echo "         attempting driver unbind fallback" >&2
        fi
    else
        echo "${MEZZ}: overlay not active: ${OVERLAY_DIR}"
    fi

    if [ -e "/sys/bus/platform/drivers/adc1283-gpio/${ADC_NODE}" ]; then
        echo "${MEZZ}: device still bound; unbinding ${ADC_NODE}"
        echo "${ADC_NODE}" > /sys/bus/platform/drivers/adc1283-gpio/unbind 2>/dev/null || true
    fi
}

any_mezz_overlay_active() {
    for MEZZ in ${ALL_MEZZ}; do
        if [ -d "$(mezz_overlay_dir "${MEZZ}")" ]; then
            return 0
        fi
    done

    return 1
}

remove_base_if_unused() {
    if any_mezz_overlay_active; then
        return 0
    fi

    if [ -d "${BASE_OVERLAY_DIR}" ]; then
        rmdir "${BASE_OVERLAY_DIR}" 2>/dev/null && \
            echo "removed base overlay ${BASE_OVERLAY_NAME}" || \
            echo "warning: could not remove base overlay ${BASE_OVERLAY_NAME}" >&2
    fi
}

list_adc1283_iio_devices() {
    for dev in /sys/bus/iio/devices/iio:device*; do
        [ -e "${dev}/name" ] || continue

        if [ "$(cat "${dev}/name" 2>/dev/null)" = "adc1283-gpio" ]; then
            echo "${dev}"
        fi
    done
}

show_real_mezz_state() {
    echo
    echo "[selected mezz overlays and binding]"

    for MEZZ in ${MEZZ_LIST}; do
        OVERLAY_DIR="$(mezz_overlay_dir "${MEZZ}")"
        ADC_NODE="$(adc_node_name "${MEZZ}")"

        if [ -d "${OVERLAY_DIR}" ]; then
            overlay_state="active"
        else
            overlay_state="inactive"
        fi

        if [ -e "/sys/firmware/devicetree/base/${ADC_NODE}" ]; then
            dt_state="present"
        else
            dt_state="missing"
        fi

        if [ -e "/sys/bus/platform/devices/${ADC_NODE}" ]; then
            platform_state="present"
        else
            platform_state="missing"
        fi

        if [ -e "/sys/bus/platform/drivers/adc1283-gpio/${ADC_NODE}" ]; then
            bound_state="bound"
        else
            bound_state="unbound"
        fi

        echo "${MEZZ}: overlay=${overlay_state} dt=${dt_state} platform=${platform_state} driver=${bound_state}"
    done
}

apply_selected() {
    require_root
    check_tools
    check_driver
    check_conflicting_overlays

    apply_base_overlay

    for MEZZ in ${MEZZ_LIST}; do
        apply_mezz_overlay "${MEZZ}"
    done

    status_selected
}

remove_selected() {
    require_root

    for MEZZ in ${MEZZ_LIST}; do
        remove_mezz_overlay "${MEZZ}"
    done

    remove_base_if_unused
    status_selected
}

status_selected() {
    echo "base_overlay_dir=${BASE_OVERLAY_DIR}"
    if [ -d "${BASE_OVERLAY_DIR}" ]; then
        echo "base_overlay_active=True"
    else
        echo "base_overlay_active=False"
    fi

    show_real_mezz_state

    echo
    echo "[driver]"
    if [ -d /sys/bus/platform/drivers/adc1283-gpio ]; then
        echo "adc1283-gpio=present"
    else
        echo "adc1283-gpio=<missing>"
    fi

    echo
    echo "[all adc1283 platform devices]"
    ls -1 /sys/bus/platform/devices 2>/dev/null | grep '^adc1283_mezz' || echo "<none>"

    echo
    echo "[bound adc1283 platform devices]"
    ls -1 /sys/bus/platform/drivers/adc1283-gpio 2>/dev/null | grep '^adc1283_mezz' || echo "<none>"

    echo
    echo "[iio adc1283 devices]"
    FOUND=0
    for dev in $(list_adc1283_iio_devices 2>/dev/null || true); do
        FOUND=1
        echo "device=${dev}"
        echo -n "name="
        cat "${dev}/name" 2>/dev/null || true

        if [ -e "${dev}/device" ]; then
            echo -n "platform="
            basename "$(readlink -f "${dev}/device")" 2>/dev/null || true
        fi

        ls "${dev}"/in_voltage*_raw 2>/dev/null || true

        if [ -r "${dev}/in_voltage_scale" ]; then
            echo -n "scale="
            cat "${dev}/in_voltage_scale"
        fi

        echo
    done

    if [ "${FOUND}" -eq 0 ]; then
        echo "<none>"
        ls -l /sys/bus/iio/devices 2>/dev/null || true
    fi

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

    echo
    echo "[recent adc1283 dmesg]"
    dmesg | grep -i "adc1283" | tail -30 || true
}

MEZZ_LIST="$(normalize_mezz_list "$@")"

case "${ACTION}" in
    apply)
        apply_selected
        ;;
    remove)
        remove_selected
        ;;
    status)
        status_selected
        ;;
    *)
        echo "usage: $0 [apply|remove|status] [all|mezz0 mezz1 mezz2 mezz3 mezz4]" >&2
        exit 2
        ;;
esac
