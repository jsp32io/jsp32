/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-IDF chip driver for the Infineon/Cypress (Spansion) S25FL128S1.
 * Behaviour is based on the vendor data sheet and the Linux spi-nor
 * Spansion/Cypress driver.  No Linux source code is copied here.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_flash_chips/spi_flash_chip_driver.h"
#include "esp_flash_chips/spi_flash_chip_generic.h"
#include "esp_flash_chips/spi_flash_defs.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define S25FL128S_JEDEC_ID          0x012018U
#define S25FL128S_EXT_ID_MSB        0x4dU
#define S25FL128S_EXT_ID_LSB        0x01U
#define S25FL128S_FAMILY_ID         0x80U
#define S25FL128S_PARAMETER_END     0x00010000U
#define S25FL128S_BLOCK_SIZE        (64U * 1024U)
#define S25FL128S_ECC_UNIT_SIZE     16U
#define S25FL128S_PROGRAM_RETRIES   3U
#define S25FL128S_ERASE_RETRIES     3U
#define S25FL128S_P_ERR             (1U << 6)
#define S25FL128S_E_ERR             (1U << 5)
#define S25FL128S_WEL               (1U << 1)
#define S25FL128S_CLSR              0x30U

static esp_err_t s25fl128s_command(esp_flash_t *chip, uint16_t command,
                                  uint8_t *input, uint8_t input_length)
{
    spi_flash_trans_t transaction = {
        .command = command,
        .miso_data = input,
        .miso_len = input_length,
        .flags = SPI_FLASH_TRANS_FLAG_IGNORE_BASEIO,
        .io_mode = SPI_FLASH_SLOWRD,
    };
    return chip->host->driver->common_command(chip->host, &transaction);
}

static esp_err_t s25fl128s_read_raw(esp_flash_t *chip, void *output,
                                    uint32_t address, uint8_t length)
{
    spi_flash_trans_t transaction = {
        .command = CMD_READ,
        .address = address,
        .address_bitlen = 24,
        .miso_data = output,
        .miso_len = length,
        .flags = SPI_FLASH_TRANS_FLAG_IGNORE_BASEIO,
        .io_mode = SPI_FLASH_SLOWRD,
    };
    return chip->host->driver->common_command(chip->host, &transaction);
}

static esp_err_t s25fl128s_write_enable(esp_flash_t *chip)
{
    esp_err_t err = s25fl128s_command(chip, CMD_WREN, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t status = 0;
    err = s25fl128s_command(chip, CMD_RDSR, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    /* FL-S requires CS# to remain high between program/erase instructions.
     * Serial flashing naturally leaves a much longer gap than back-to-back
     * application MSPI transactions. */
    esp_rom_delay_us(10);
    return (status & S25FL128S_WEL) ? ESP_OK : ESP_FAIL;
}

static esp_err_t s25fl128s_read_reg(esp_flash_t *chip,
                                    spi_flash_register_t reg_id,
                                    uint32_t *out_reg)
{
    if (reg_id != SPI_FLASH_REG_STATUS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t status = 0;
    esp_err_t err = s25fl128s_command(chip, CMD_RDSR, &status, 1);
    if (err == ESP_OK) {
        *out_reg = status;
    }
    return err;
}

static esp_err_t s25fl128s_probe(esp_flash_t *chip, uint32_t flash_id)
{
    if (flash_id != S25FL128S_JEDEC_ID) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 01 20 18 alone is shared by several incompatible S25FL parts. */
    uint8_t id[6] = {0};
    esp_err_t err = s25fl128s_command(chip, CMD_RDID, id, sizeof(id));
    if (err != ESP_OK) {
        return err;
    }
    if (id[0] != 0x01 || id[1] != 0x20 || id[2] != 0x18 ||
        id[3] != S25FL128S_EXT_ID_MSB ||
        id[4] != S25FL128S_EXT_ID_LSB ||
        id[5] != S25FL128S_FAMILY_ID) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Linux uses USE_CLSR for this family.  Start from a known status so a
     * stale P_ERR/E_ERR from an earlier boot is not attributed to our first
     * operation. */
    return s25fl128s_command(chip, S25FL128S_CLSR, NULL, 0);
}

static esp_err_t s25fl128s_detect_size(esp_flash_t *chip, uint32_t *size)
{
    (void)chip;
    *size = 16U * 1024U * 1024U;
    return ESP_OK;
}

static esp_err_t s25fl128s_wait_idle(esp_flash_t *chip, uint32_t timeout_us)
{
    bool operation_pending = chip->busy != 0;
    esp_err_t err = spi_flash_chip_generic_wait_idle(chip, timeout_us);
    if (err != ESP_OK || !operation_pending) {
        return err;
    }

    uint32_t status = 0;
    err = s25fl128s_read_reg(chip, SPI_FLASH_REG_STATUS, &status);
    if (err != ESP_OK) {
        return err;
    }
    if ((status & (S25FL128S_P_ERR | S25FL128S_E_ERR)) != 0) {
        ESP_DRAM_LOGE("s25fl128s", "operation error, SR1=0x%02x\n",
                      (unsigned)status);
        (void)s25fl128s_command(chip, S25FL128S_CLSR, NULL, 0);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t s25fl128s_erase_sector(esp_flash_t *chip,
                                       uint32_t address)
{
    /* This board's 4 KiB erase was verified below 0x10000, but an erase at
     * 0x10000 was ignored. Reject unverified addresses: opcode 0x20 can
     * otherwise report success without clearing any data. */
    if (address >= S25FL128S_PARAMETER_END) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return spi_flash_chip_generic_erase_sector(chip, address);
}

static bool s25fl128s_equal(const uint8_t *left, const uint8_t *right,
                           uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

static bool s25fl128s_all_ff(const uint8_t *data, uint32_t length)
{
    for (uint32_t i = 0; i < length; ++i) {
        if (data[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static esp_err_t s25fl128s_erase_block(esp_flash_t *chip,
                                      uint32_t address)
{
    uint8_t verify[64];
    for (uint32_t attempt = 0; attempt < S25FL128S_ERASE_RETRIES; ++attempt) {
        /* FL-S keeps P_ERR/E_ERR set until CLSR. The serial block-erase path
         * clears them before WREN; do the same before every firmware attempt. */
        esp_err_t err = s25fl128s_command(chip, S25FL128S_CLSR, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
        err = chip->chip_drv->wait_idle(chip, chip->chip_drv->timeout->idle_timeout);
        if (err != ESP_OK) {
            return err;
        }
        err = s25fl128s_write_enable(chip);
        if (err != ESP_OK) {
            return err;
        }
        /* SPI1's dedicated block-erase command matches the path used by
         * ESP-IDF's normal 24-bit flash driver and the serial flasher. */
        chip->host->driver->erase_block(chip->host, address);
        chip->busy = 1;
        esp_rom_delay_us(10);
        err = chip->chip_drv->wait_idle(chip,
                                        chip->chip_drv->timeout->block_erase_timeout);
        if (err != ESP_OK) {
            ESP_DRAM_LOGE("s25fl128s",
                          "erase command failed at 0x%08x: err=0x%x\n",
                          (unsigned)address, (unsigned)err);
            return err;
        }

        bool erased = true;
        for (uint32_t offset = 0; offset < S25FL128S_BLOCK_SIZE;
             offset += sizeof(verify)) {
            err = s25fl128s_read_raw(chip, verify, address + offset,
                                     sizeof(verify));
            if (err != ESP_OK) {
                return err;
            }
            if (!s25fl128s_all_ff(verify, sizeof(verify))) {
                erased = false;
                uint32_t bad = 0;
                while (verify[bad] == 0xff) {
                    ++bad;
                }
                uint8_t status = 0;
                (void)s25fl128s_command(chip, CMD_RDSR, &status, 1);
                ESP_DRAM_LOGW("s25fl128s",
                              "erase did not clear 0x%08x: data=%02x SR1=%02x, attempt %u/%u\n",
                              (unsigned)(address + offset + bad),
                              (unsigned)verify[bad], (unsigned)status,
                              (unsigned)(attempt + 1),
                              (unsigned)S25FL128S_ERASE_RETRIES);
                break;
            }
        }
        if (erased) {
            return ESP_OK;
        }
    }
    ESP_DRAM_LOGE("s25fl128s", "erase retries exhausted at 0x%08x\n",
                  (unsigned)address);
    return ESP_FAIL;
}

static esp_err_t s25fl128s_program_page(esp_flash_t *chip,
                                       const void *buffer,
                                       uint32_t address, uint32_t length)
{
    /* Program exactly one ECC unit at a time.  Apart from shortening the MSPI
     * data transaction, this guarantees that a successful OTA stream never
     * programs the same 16-byte ECC unit twice.  Read it back immediately.
     * A retry is safe only while the complete unit is still erased. */
    if (length > S25FL128S_ECC_UNIT_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t verify[S25FL128S_ECC_UNIT_SIZE];
    for (uint32_t attempt = 0; attempt < S25FL128S_PROGRAM_RETRIES; ++attempt) {
        /* Page Program clears WEL after every accepted command, including a
         * command which completes without changing the array.  Re-issue WREN
         * for every retry instead of relying on generic_write()'s initial
         * WREN. */
        esp_err_t err = chip->chip_drv->wait_idle(
            chip, chip->chip_drv->timeout->idle_timeout);
        if (err != ESP_OK) {
            return err;
        }
        err = s25fl128s_command(chip, S25FL128S_CLSR, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
        err = s25fl128s_write_enable(chip);
        if (err != ESP_OK) {
            return err;
        }
        chip->host->driver->program_page(chip->host, buffer, address, length);
        chip->busy = 1;
        esp_rom_delay_us(10);
        err = chip->chip_drv->wait_idle(
            chip, chip->chip_drv->timeout->page_program_timeout);
        if (err != ESP_OK) {
            return err;
        }
        err = s25fl128s_read_raw(chip, verify, address, length);
        if (err != ESP_OK) {
            return err;
        }
        if (s25fl128s_equal(verify, buffer, length)) {
            return ESP_OK;
        }
        if (!s25fl128s_all_ff(verify, length)) {
            uint32_t bad = 0;
            while (verify[bad] == ((const uint8_t *)buffer)[bad]) {
                ++bad;
            }
            ESP_DRAM_LOGE("s25fl128s",
                          "partial program at 0x%08x, length=%u, mismatch 0x%08x: expected=%02x actual=%02x\n",
                          (unsigned)address, (unsigned)length,
                          (unsigned)(address + bad),
                          ((const uint8_t *)buffer)[bad], verify[bad]);
            return ESP_FAIL;
        }
        ESP_DRAM_LOGW("s25fl128s",
                      "program command lost at 0x%08x, retry %u/%u\n",
                      (unsigned)address, (unsigned)(attempt + 1),
                      (unsigned)S25FL128S_PROGRAM_RETRIES);
    }
    ESP_DRAM_LOGE("s25fl128s",
                  "program retries exhausted at 0x%08x, length=%u\n",
                  (unsigned)address, (unsigned)length);
    return ESP_FAIL;
}

static esp_err_t s25fl128s_write(esp_flash_t *chip, const void *buffer,
                                uint32_t address, uint32_t length)
{
    const uint8_t *source = buffer;
    while (length > 0) {
        uint32_t ecc_remaining = S25FL128S_ECC_UNIT_SIZE -
                                 (address & (S25FL128S_ECC_UNIT_SIZE - 1U));
        uint32_t chunk = length;
        if (chunk > S25FL128S_ECC_UNIT_SIZE) {
            chunk = S25FL128S_ECC_UNIT_SIZE;
        }
        if (chunk > ecc_remaining) {
            chunk = ecc_remaining;
        }

        /* Keep each Page Program within one 16-byte ECC unit.  The OTA layer
         * coalesces network fragments so its sequential stream reaches this
         * driver as complete units. */
        esp_err_t err = s25fl128s_program_page(chip, source, address, chunk);
        if (err != ESP_OK) {
            return err;
        }
        source += chunk;
        address += chunk;
        length -= chunk;
    }
    return ESP_OK;
}

static spi_flash_caps_t s25fl128s_get_caps(esp_flash_t *chip)
{
    (void)chip;
    return 0;
}

static const char s25fl128s_name[] = "s25fl128s1";

static const spi_flash_chip_t esp_flash_chip_s25fl128s = {
    .name = s25fl128s_name,
    .timeout = &spi_flash_chip_generic_timeout,
    .probe = s25fl128s_probe,
    .reset = spi_flash_chip_generic_reset,
    .detect_size = s25fl128s_detect_size,
    .erase_chip = spi_flash_chip_generic_erase_chip,
    .erase_sector = s25fl128s_erase_sector,
    .erase_block = s25fl128s_erase_block,
    .sector_size = 4U * 1024U,
    .block_erase_size = S25FL128S_BLOCK_SIZE,
    .get_chip_write_protect = spi_flash_chip_generic_get_write_protect,
    .set_chip_write_protect = spi_flash_chip_generic_set_write_protect,
    .num_protectable_regions = 0,
    .protectable_regions = NULL,
    .get_protected_regions = NULL,
    .set_protected_regions = NULL,
    .read = spi_flash_chip_generic_read,
    .write = s25fl128s_write,
    .program_page = s25fl128s_program_page,
    /* Linux deliberately uses the minimum common configuration: programming
     * 256 bytes is safe even if a particular device is configured for 512. */
    .page_size = 256,
    .write_encrypted = spi_flash_chip_generic_write_encrypted,
    .wait_idle = s25fl128s_wait_idle,
    .set_io_mode = spi_flash_chip_generic_set_io_mode,
    .get_io_mode = spi_flash_chip_generic_get_io_mode,
    .read_reg = s25fl128s_read_reg,
    .yield = spi_flash_chip_generic_yield,
    .sus_setup = spi_flash_chip_generic_suspend_cmd_conf,
    .read_unique_id = spi_flash_chip_generic_read_unique_id_none,
    .get_chip_caps = s25fl128s_get_caps,
    .config_host_io_mode = spi_flash_chip_generic_config_host_io_mode,
};

/* Overrides ESP-IDF's table when CONFIG_SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST
 * is enabled.  Generic remains the final fallback for development boards. */
const spi_flash_chip_t *default_registered_chips[] = {
    &esp_flash_chip_s25fl128s,
    &esp_flash_chip_generic,
    NULL,
};
