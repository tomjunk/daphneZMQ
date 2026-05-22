#!/usr/bin/env python3
"""
Small /dev/spidev test utility for DAPHNE MEZ0 spi-gpio bring-up.

Default target:
  /dev/spidev4.0, expected to be the MEZ0 spi-gpio bus.

This script intentionally uses Linux spidev ioctls directly, so it does not
require the third-party Python "spidev" package on the board.
"""

from __future__ import annotations

import argparse
import array
import fcntl
import os
import struct
import sys
import time
from pathlib import Path


SPI_IOC_MAGIC = ord("k")
SPI_CPHA = 0x01
SPI_CPOL = 0x02

IOC_NRBITS = 8
IOC_TYPEBITS = 8
IOC_SIZEBITS = 14
IOC_DIRBITS = 2

IOC_NRSHIFT = 0
IOC_TYPESHIFT = IOC_NRSHIFT + IOC_NRBITS
IOC_SIZESHIFT = IOC_TYPESHIFT + IOC_TYPEBITS
IOC_DIRSHIFT = IOC_SIZESHIFT + IOC_SIZEBITS

IOC_WRITE = 1
IOC_READ = 2


def ioc(direction: int, type_: int, nr: int, size: int) -> int:
    return (
        (direction << IOC_DIRSHIFT)
        | (type_ << IOC_TYPESHIFT)
        | (nr << IOC_NRSHIFT)
        | (size << IOC_SIZESHIFT)
    )


def iow(type_: int, nr: int, size: int) -> int:
    return ioc(IOC_WRITE, type_, nr, size)


def ior(type_: int, nr: int, size: int) -> int:
    return ioc(IOC_READ, type_, nr, size)


SPI_IOC_WR_MODE = iow(SPI_IOC_MAGIC, 1, 1)
SPI_IOC_RD_MODE = ior(SPI_IOC_MAGIC, 1, 1)
SPI_IOC_WR_BITS_PER_WORD = iow(SPI_IOC_MAGIC, 3, 1)
SPI_IOC_RD_BITS_PER_WORD = ior(SPI_IOC_MAGIC, 3, 1)
SPI_IOC_WR_MAX_SPEED_HZ = iow(SPI_IOC_MAGIC, 4, 4)
SPI_IOC_RD_MAX_SPEED_HZ = ior(SPI_IOC_MAGIC, 4, 4)

SPI_IOC_TRANSFER_SIZE = 32


def spi_ioc_message(count: int) -> int:
    return iow(SPI_IOC_MAGIC, 0, SPI_IOC_TRANSFER_SIZE * count)


def parse_hex_bytes(text: str) -> list[int]:
    cleaned = text.replace(",", " ").replace("0x", "").replace("\\x", " ")
    values: list[int] = []
    for token in cleaned.split():
        value = int(token, 16)
        if not 0 <= value <= 0xFF:
            raise argparse.ArgumentTypeError(f"byte out of range: {token}")
        values.append(value)
    if not values:
        raise argparse.ArgumentTypeError("empty byte pattern")
    return values


def adc1283_control_byte(channel: int) -> int:
    if not 0 <= channel <= 7:
        raise ValueError("ADC1283 channel must be in range 0..7")
    return channel << 3


def decode_adc1283_words(rx: list[int]) -> list[int]:
    samples: list[int] = []
    for index in range(0, len(rx) - 1, 2):
        raw16 = (rx[index] << 8) | rx[index + 1]
        samples.append(raw16 & 0x0FFF)
    return samples


class SpiDev:
    def __init__(self, device: Path, mode: int, speed_hz: int, bits_per_word: int) -> None:
        self.device = device
        self.fd = os.open(str(device), os.O_RDWR)
        try:
            self.set_u8(SPI_IOC_WR_MODE, mode)
            self.set_u8(SPI_IOC_WR_BITS_PER_WORD, bits_per_word)
            self.set_u32(SPI_IOC_WR_MAX_SPEED_HZ, speed_hz)
        except Exception:
            os.close(self.fd)
            raise

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self) -> "SpiDev":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def set_u8(self, request: int, value: int) -> None:
        data = array.array("B", [value & 0xFF])
        fcntl.ioctl(self.fd, request, data, True)

    def set_u32(self, request: int, value: int) -> None:
        data = array.array("I", [value & 0xFFFFFFFF])
        fcntl.ioctl(self.fd, request, data, True)

    def transfer(self, tx: list[int], speed_hz: int, bits_per_word: int) -> list[int]:
        tx_buf = array.array("B", tx)
        rx_buf = array.array("B", [0] * len(tx))

        transfer = struct.pack(
            "<QQIIHBBBBBB",
            tx_buf.buffer_info()[0],
            rx_buf.buffer_info()[0],
            len(tx),
            speed_hz,
            0,  # delay_usecs
            bits_per_word,
            0,  # cs_change
            0,  # tx_nbits
            0,  # rx_nbits
            0,  # word_delay_usecs
            0,  # pad
        )
        fcntl.ioctl(self.fd, spi_ioc_message(1), transfer)
        return list(rx_buf)


def format_bytes(values: list[int]) -> str:
    return " ".join(f"{value:02X}" for value in values)


def describe_spidev(device: Path) -> None:
    name = device.name
    if name.startswith("spidev"):
        spi_name = name.removeprefix("spidev")
        spi_dev_dir = Path("/sys/bus/spi/devices") / f"spi{spi_name}"
        if spi_dev_dir.exists():
            print(f"sysfs_device={spi_dev_dir}")
            try:
                print(f"driver={Path(os.path.realpath(spi_dev_dir / 'driver')).name}")
            except OSError:
                print("driver=<none>")
            print(f"resolved={os.path.realpath(spi_dev_dir)}")
            return
    print(f"sysfs_spidev=<not found for {name}>")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Test DAPHNE MEZ0 through its spi-gpio /dev/spidev device."
    )
    parser.add_argument("--device", default="/dev/spidev4.0", help="spidev device path.")
    parser.add_argument("--speed", type=int, default=100_000, help="SPI speed in Hz.")
    parser.add_argument("--mode", type=int, choices=range(4), default=0, help="SPI mode 0..3.")
    parser.add_argument("--bits", type=int, default=8, help="Bits per word.")
    parser.add_argument(
        "--tx",
        type=parse_hex_bytes,
        default=parse_hex_bytes("AA 55 00 FF"),
        help='Hex bytes to send, e.g. "AA 55 00 FF".',
    )
    parser.add_argument(
        "--burst-bytes",
        type=int,
        default=0,
        help="Replace --tx with this many 0xAA bytes to make a long clock burst.",
    )
    parser.add_argument("--loop", type=int, default=1, help="Number of transfers. Use 0 forever.")
    parser.add_argument("--delay-ms", type=float, default=100.0, help="Delay between transfers.")
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Do not print each received transfer. Useful for continuous scope captures.",
    )
    parser.add_argument(
        "--status-every",
        type=int,
        default=0,
        help="With --quiet, print a compact transfer count every N transfers. Default: disabled.",
    )
    parser.add_argument("--adc1283-channel", type=int, choices=range(8), help="Send ADC1283 channel frame.")
    parser.add_argument(
        "--adc1283-clocks",
        type=int,
        choices=(16, 32),
        default=16,
        help="ADC1283 clocks to issue when --adc1283-channel is used.",
    )
    parser.add_argument(
        "--decode-adc1283",
        action="store_true",
        help="Print 12-bit ADC1283 samples decoded from received 16-bit words.",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    device = Path(args.device)

    if args.adc1283_channel is not None:
        control = adc1283_control_byte(args.adc1283_channel)
        byte_count = args.adc1283_clocks // 8
        tx = [control] + [0x00] * (byte_count - 1)
    elif args.burst_bytes:
        tx = [0xAA] * args.burst_bytes
    else:
        tx = args.tx

    print(f"device={device} speed={args.speed}Hz mode={args.mode} bits={args.bits}")
    if device.exists():
        describe_spidev(device)
    else:
        print("device_exists=False")
    print(f"tx={format_bytes(tx)}")

    count = 0
    with SpiDev(device, args.mode, args.speed, args.bits) as spi:
        while args.loop == 0 or count < args.loop:
            rx = spi.transfer(tx, args.speed, args.bits)
            count += 1
            if args.quiet:
                if args.status_every and count % args.status_every == 0:
                    print(f"transfers={count}", flush=True)
            else:
                line = f"rx={format_bytes(rx)}"
                if args.decode_adc1283:
                    samples = decode_adc1283_words(rx)
                    line += " adc1283=" + " ".join(f"{sample:04d}/0x{sample:03X}" for sample in samples)
                print(line, flush=True)
            if args.loop == 0 or count < args.loop:
                time.sleep(args.delay_ms / 1000.0)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OSError as exc:
        raise SystemExit(f"{exc.strerror}: {exc.filename or ''}".strip())
