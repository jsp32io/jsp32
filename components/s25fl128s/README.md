# S25FL128S1 flash driver

Project-local ESP-IDF chip driver for the Infineon/Cypress S25FL128S1 with
extended RDID `01 20 18 4d 01 80`.

The component is discovered automatically from `components/`.  It replaces
ESP-IDF's flash driver table when
`CONFIG_SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST=y`; no system-wide installation
or IDF source patch is required.

After building and booting, successful selection is visible in the startup
log:

```text
spi_flash: detected chip: s25fl128s1
```

The first deployment must be made over serial because the currently running
firmware cannot use a driver contained only in the new OTA image.  Subsequent
OTA writes use this driver.

Important geometry constraints:

- Page programming uses a conservative 256-byte page size.
- Program operations are sliced into 16-byte ECC units, read back immediately,
  and retried only if the complete unit is still erased. Retrying partially
  programmed data is deliberately prohibited to preserve the flash's internal
  ECC.
- On the tested board, a 4 KiB erase below `0x10000` worked, while one at
  `0x10000` left the old 64 KiB block unchanged. The driver conservatively
  accepts 4 KiB erases only below `0x10000` and uses 64 KiB blocks elsewhere.
- `otadata` is at `0x9000`, with both 4 KiB metadata sectors below `0x10000`.
  Both OTA app slots start on 64 KiB boundaries.
- Main-array 64 KiB erases clear sticky flash errors first and are read back
  before reporting success. A block that remains programmed is retried up to
  three times; firmware writes stop if it cannot be cleared.
- Main-array erase and page program use the ESP-IDF host's dedicated commands.
  The driver waits after WREN, checks status through single-I/O reads, and
  verifies erased blocks and each 16-byte program unit by single-I/O reads.
- The tested board uses QIO reads at 40 MHz and limits Wi-Fi transmit power
  to 13 dBm. One complete 64 KiB sequential-erase OTA upload, reboot, and
  firmware confirmation succeeded without Flash warnings at this setting.
  At 15 dBm, an upload failed after repeated block erase and lost program
  commands. QIO and DIO at 80 MHz booted, but OTA metadata confirmation
  failed in both configurations.
  Enabling ESP-IDF HPM at 80 MHz does not run a chip-specific HPM procedure.
  A 120 MHz/HPM build with the board's 80 MHz Octal PSRAM is rejected by
  ESP-IDF's MSPI clock checks; S25FL128S is not in its HPM support table.
  At unrestricted transmit power, OTA erase and program operations also failed
  during HTTP uploads; this points to a board power or RF interaction, but
  does not establish which one. Other boards need their own OTA test.
- LittleFS uses 64 KiB erase blocks in the main array; this flash driver does
  not make 4 KiB main-array erases possible.

The implementation follows behaviour documented by the Infineon data sheet
and Linux's `drivers/mtd/spi-nor/spansion.c`, but contains no copied Linux
source.
