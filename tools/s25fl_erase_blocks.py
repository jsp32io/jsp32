#!/usr/bin/env python3
"""Erase S25FL-S main-array blocks with the 64 KiB erase command."""

import argparse
import time

from esptool.cmds import attach_flash, detect_chip


BLOCK_SIZE = 64 * 1024
CMD_READ_STATUS = 0x05
CMD_WRITE_ENABLE = 0x06
CMD_CLEAR_STATUS = 0x30
CMD_BLOCK_ERASE = 0xD8
STATUS_WIP = 1 << 0
STATUS_WEL = 1 << 1
STATUS_ERASE_ERROR = 1 << 5


def integer(value: str) -> int:
    return int(value, 0)


def read_status(esp) -> int:
    return esp.run_spiflash_command(CMD_READ_STATUS, read_bits=8)


def erase_block(esp, address: int, timeout: float) -> None:
    esp.run_spiflash_command(CMD_CLEAR_STATUS)
    esp.run_spiflash_command(CMD_WRITE_ENABLE)
    status = read_status(esp)
    if not status & STATUS_WEL:
        raise RuntimeError(
            f"write enable failed at {address:#010x}: status={status:#04x}"
        )

    esp.run_spiflash_command(
        CMD_BLOCK_ERASE, addr=address, addr_len=24
    )
    deadline = time.monotonic() + timeout
    while True:
        status = read_status(esp)
        if not status & STATUS_WIP:
            break
        if time.monotonic() >= deadline:
            raise TimeoutError(f"erase timed out at {address:#010x}")
        time.sleep(0.05)

    if status & STATUS_ERASE_ERROR:
        raise RuntimeError(
            f"erase failed at {address:#010x}: status={status:#04x}"
        )

    erased = esp.read_flash(address, BLOCK_SIZE)
    if erased != b"\xff" * BLOCK_SIZE:
        offset = next(index for index, byte in enumerate(erased) if byte != 0xff)
        raise RuntimeError(
            f"erase verification failed at {address + offset:#010x}: "
            f"{erased[offset]:#04x}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="erase an aligned S25FL-S range using 64 KiB 0xD8 blocks"
    )
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=integer, default=115200)
    parser.add_argument("--start", type=integer, required=True)
    parser.add_argument("--length", type=integer, required=True)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    if args.start < 0 or args.start % BLOCK_SIZE:
        parser.error("--start must be non-negative and 64 KiB aligned")
    if args.length <= 0 or args.length % BLOCK_SIZE:
        parser.error("--length must be positive and 64 KiB aligned")
    if args.start + args.length > 0x1000000:
        parser.error("erase range exceeds the detected 16 MiB flash")

    block_count = args.length // BLOCK_SIZE
    with detect_chip(port=args.port, baud=args.baud) as esp:
        attach_flash(esp)
        for index, address in enumerate(
            range(args.start, args.start + args.length, BLOCK_SIZE), 1
        ):
            print(
                f"Erasing {address:#010x} "
                f"({index}/{block_count})...",
                flush=True,
            )
            erase_block(esp, address, args.timeout)
        print("Erase completed and full-block readback passed.")
        esp.hard_reset()


if __name__ == "__main__":
    main()
