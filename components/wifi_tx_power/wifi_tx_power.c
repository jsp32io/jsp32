#include "wifi_tx_power.h"

#include "nvs.h"
#include "sdkconfig.h"

#define CONFIG_NAMESPACE "jsp32_config"
#define POWER_KEY "wifi_tx_qdbm"

esp_err_t wifi_tx_power_load(int8_t *power)
{
    if (!power) return ESP_ERR_INVALID_ARG;
    *power = CONFIG_NETWORK_MANAGER_MAX_TX_POWER_QDBM;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t saved = 0;
    err = nvs_get_u8(handle, POWER_KEY, &saved);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (saved < 8 || saved > 84) return ESP_ERR_INVALID_STATE;
    *power = (int8_t)saved;
    return ESP_OK;
}

esp_err_t wifi_tx_power_save(int8_t power)
{
    if (power < 8 || power > 84) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, POWER_KEY, (uint8_t)power);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
