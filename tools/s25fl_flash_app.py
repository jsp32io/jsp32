#!/usr/bin/env python3
"""Flash an OTA app to S25FL128S1 using its real 64 KiB erase geometry.

esptool's normal write-flash stub assumes 4 KiB erase sectors throughout the
chip when the image ends partway into a 64 KiB block. The bottom-parameter
tested S25FL128S1 board only cleared those below 0x10000. Pad the app to a
64 KiB multiple so the stub uses its 64 KiB erase path. esptool verifies the
written MD5.
The --raw option retains the slower raw-SPI path for troubleshooting.
"""

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from esptool.cmds import attach_flash, detect_chip

from s25fl_erase_blocks import (
    BLOCK_SIZE,
    CMD_CLEAR_STATUS,
    CMD_WRITE_ENABLE,
    STATUS_WEL,
    STATUS_WIP,
    erase_block,
    integer,
    read_status,
)


APP_PARTITIONS = {0x20000: "ota_0", 0x320000: "ota_1"}
APP_PARTITION_SIZE = 0x300000
PROGRAM_SIZE = 64  # esptool raw SPI commands permit at most 64 MOSI bytes.
CMD_READ_ID = 0x9F
CMD_PAGE_PROGRAM = 0x02
STATUS_PROGRAM_ERROR = 1 << 6
STATUS_ERASE_ERROR = 1 << 5


def load_image(path: Path) -> bytes:
    image = path.read_bytes()
    if not image or image[0] != 0xE9:
        raise ValueError(f"{path} is not an ESP application image")
    if len(image) > APP_PARTITION_SIZE:
        raise ValueError(f"image exceeds the {APP_PARTITION_SIZE:#x}-byte app partition")
    return image


def padded_image(image: bytes) -> bytes:
    return image + b"\xff" * (-len(image) % BLOCK_SIZE)


def check_chip(esp) -> None:
    # esptool can read four RDID bytes via the SPI register interface.
    chip_id = esp.run_spiflash_command(CMD_READ_ID, read_bits=32)
    id_bytes = chip_id.to_bytes(4, "little")
    if id_bytes != bytes.fromhex("01 20 18 4d"):
        raise RuntimeError(
            f"unexpected flash RDID {id_bytes.hex(' ')}; expected 01 20 18 4d"
        )


def program_chunk(esp, address: int, data: bytes, timeout: float) -> None:
    if not data or len(data) > PROGRAM_SIZE or address % PROGRAM_SIZE:
        raise ValueError("page program must start at a 64-byte boundary")
    if data == b"\xff" * len(data):
        return

    esp.run_spiflash_command(CMD_CLEAR_STATUS)
    esp.run_spiflash_command(CMD_WRITE_ENABLE)
    status = read_status(esp)
    if not status & STATUS_WEL:
        raise RuntimeError(f"write enable failed at {address:#010x}: SR1={status:#04x}")

    esp.run_spiflash_command(
        CMD_PAGE_PROGRAM, data=data, addr=address, addr_len=24
    )
    deadline = time.monotonic() + timeout
    while True:
        status = read_status(esp)
        if not status & STATUS_WIP:
            break
        if time.monotonic() >= deadline:
            raise TimeoutError(f"page program timed out at {address:#010x}")

    if status & (STATUS_PROGRAM_ERROR | STATUS_ERASE_ERROR):
        raise RuntimeError(f"page program failed at {address:#010x}: SR1={status:#04x}")


def first_mismatch(actual: bytes, expected: bytes) -> int | None:
    return next(
        (index for index, (a, b) in enumerate(zip(actual, expected)) if a != b),
        None,
    )


def flash_app(esp, address: int, image: bytes, erase_timeout: float) -> None:
    data = padded_image(image)
    blocks = len(data) // BLOCK_SIZE
    for index in range(blocks):
        block_address = address + index * BLOCK_SIZE
        expected = data[index * BLOCK_SIZE : (index + 1) * BLOCK_SIZE]
        print(f"[{index + 1}/{blocks}] Erasing {block_address:#010x}...", flush=True)
        erase_block(esp, block_address, erase_timeout)
        erased = esp.read_flash(block_address, BLOCK_SIZE)
        if erased != b"\xff" * BLOCK_SIZE:
            mismatch = first_mismatch(erased, b"\xff" * BLOCK_SIZE)
            raise RuntimeError(
                f"erase verification failed at {block_address + mismatch:#010x}"
            )

        print(f"[{index + 1}/{blocks}] Programming and verifying...", flush=True)
        for offset in range(0, BLOCK_SIZE, PROGRAM_SIZE):
            program_chunk(
                esp,
                block_address + offset,
                expected[offset : offset + PROGRAM_SIZE],
                1.0,
            )
        actual = esp.read_flash(block_address, BLOCK_SIZE)
        if actual != expected:
            mismatch = first_mismatch(actual, expected)
            raise RuntimeError(
                f"write verification failed at {block_address + mismatch:#010x}: "
                f"expected {expected[mismatch]:#04x}, got {actual[mismatch]:#04x}"
            )


def fast_flash(port: str, baud: int, address: int, image: bytes) -> None:
    with tempfile.TemporaryDirectory(prefix="s25fl-app-") as directory:
        padded_path = Path(directory) / "app-padded.bin"
        padded_path.write_bytes(padded_image(image))
        print(
            f"Padded to {padded_path.stat().st_size} bytes so every main-array "
            "erase is 64 KiB.",
            flush=True,
        )
        subprocess.run(
            [
                sys.executable,
                "-m",
                "esptool",
                "--port",
                port,
                "--baud",
                str(baud),
                "write-flash",
                "--no-compress",
                "--flash-mode",
                "dio",
                "--flash-size",
                "16MB",
                "--flash-freq",
                "40m",
                hex(address),
                str(padded_path),
            ],
            check=True,
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=integer, default=460800)
    parser.add_argument("--address", type=integer, required=True)
    parser.add_argument("--erase-timeout", type=float, default=15.0)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--raw", action="store_true", help="use the slow raw SPI path")
    parser.add_argument("image", type=Path)
    args = parser.parse_args()

    if args.address not in APP_PARTITIONS:
        parser.error("--address must be 0x20000 (ota_0) or 0x320000 (ota_1)")
    if args.erase_timeout <= 0:
        parser.error("--erase-timeout must be positive")
    try:
        image = load_image(args.image)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    blocks = len(padded_image(image)) // BLOCK_SIZE
    print(
        f"{APP_PARTITIONS[args.address]}: {len(image)} image bytes, "
        f"{blocks} erase blocks, SHA-256 {hashlib.sha256(image).hexdigest()}",
        flush=True,
    )
    if args.dry_run:
        return

    if not args.raw:
        fast_flash(args.port, args.baud, args.address, image)
        return

    with detect_chip(port=args.port, baud=args.baud) as esp:
        attach_flash(esp)
        check_chip(esp)
        flash_app(esp, args.address, image, args.erase_timeout)
        print("Flash readback matches the image and FF padding.", flush=True)
        esp.hard_reset()


if __name__ == "__main__":
    main()
