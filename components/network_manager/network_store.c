#include "network_manager_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include <string.h>
#include "esp_mac.h"
#include "nvs.h"
#include "lwip/inet.h"

#define CONFIG_NAMESPACE "jsp32_config"
#define MODE_KEY "wifi_mode"

static bool parse_ipv4(const char *text, esp_ip4_addr_t *address)
{
    uint32_t value = 0;
    for (unsigned part = 0; part < 4; ++part) {
        unsigned octet = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            octet = octet * 10 + (unsigned)(*text++ - '0');
            if (++digits > 3 || octet > 255) return false;
        }
        if (!digits || (part < 3 ? *text++ != '.' : *text != '\0')) return false;
        value = (value << 8) | octet;
    }
    address->addr = htonl(value);
    return true;
}

esp_err_t network_store_load_ap_ip(esp_netif_ip_info_t *ip_info)
{
    if (!ip_info) return ESP_ERR_INVALID_ARG;
    *ip_info = (esp_netif_ip_info_t){
        .ip = {.addr = inet_addr(NETWORK_AP_DEFAULT_IP)},
        .gw = {.addr = inet_addr(NETWORK_AP_DEFAULT_IP)},
        .netmask = {.addr = inet_addr(NETWORK_AP_DEFAULT_NETMASK)},
    };
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    char ip[16] = NETWORK_AP_DEFAULT_IP, mask[16] = NETWORK_AP_DEFAULT_NETMASK;
    size_t size = sizeof(ip);
    err = nvs_get_str(handle, "ap_ip", ip, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) {
        size = sizeof(mask);
        err = nvs_get_str(handle, "ap_netmask", mask, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    nvs_close(handle);
    if (err != ESP_OK) return err;
    esp_netif_ip_info_t configured = {0};
    if (!parse_ipv4(ip, &configured.ip) || !parse_ipv4(mask, &configured.netmask))
        return ESP_ERR_INVALID_ARG;
    uint32_t address = ntohl(configured.ip.addr);
    uint32_t hosts = ~ntohl(configured.netmask.addr);
    unsigned first = address >> 24;
    if (!first || first == 127 || first >= 224 || hosts < 3 || hosts == UINT32_MAX ||
        (hosts & (hosts + 1)) || !(address & hosts) || (address & hosts) == hosts)
        return ESP_ERR_INVALID_ARG;
    configured.gw = configured.ip;
    *ip_info = configured;
    return ESP_OK;
}

bool network_credentials_valid(const char *ssid, const char *password)
{
    if (!ssid || !password) return false;
    size_t ssid_length = strnlen(ssid, 33);
    size_t password_length = strnlen(password, 65);
    if (!ssid_length || ssid_length > 32 || password_length > 64) return false;
    if (password_length == 64) {
        for (size_t i = 0; i < 64; ++i) {
            if (!isxdigit((unsigned char)password[i])) return false;
        }
        return true;
    }
    return password_length == 0 || password_length >= 8;
}

bool network_settings_valid(const network_settings_t *settings)
{
    if (!settings ||
        settings->mode < NETWORK_MODE_AP || settings->mode > NETWORK_MODE_APSTA ||
        strnlen(settings->ap_password, sizeof(settings->ap_password)) >=
            sizeof(settings->ap_password) ||
        !network_credentials_valid(settings->ap_ssid, settings->ap_password)) {
        return false;
    }
    if (!settings->sta_ssid[0]) {
        return settings->mode == NETWORK_MODE_AP && !settings->sta_password[0];
    }
    return network_credentials_valid(settings->sta_ssid, settings->sta_password);
}

void network_store_defaults(network_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->mode = NETWORK_MODE_AP;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(settings->ap_ssid, sizeof(settings->ap_ssid), "%.*s-%02X%02X",
             27, CONFIG_NETWORK_AP_PREFIX, mac[4], mac[5]);
}

/* NVS names and types are independent of the in-memory struct layout. */
typedef struct {
    const char *key;
    size_t offset;
    size_t capacity;
} string_key_t;

static const string_key_t STRING_KEYS[] = {
    {"ap_ssid", offsetof(network_settings_t, ap_ssid), sizeof(((network_settings_t *)0)->ap_ssid)},
    {"ap_pass", offsetof(network_settings_t, ap_password), sizeof(((network_settings_t *)0)->ap_password)},
    {"wifi_ssid", offsetof(network_settings_t, sta_ssid), sizeof(((network_settings_t *)0)->sta_ssid)},
    {"wifi_pass", offsetof(network_settings_t, sta_password), sizeof(((network_settings_t *)0)->sta_password)},
};
#define STRING_KEY_COUNT (sizeof(STRING_KEYS) / sizeof(STRING_KEYS[0]))

static char *setting_string(network_settings_t *settings, size_t index)
{
    return (char *)settings + STRING_KEYS[index].offset;
}

esp_err_t network_store_load(network_settings_t *settings, bool *mode_saved)
{
    if (!settings || !mode_saved) return ESP_ERR_INVALID_ARG;
    network_store_defaults(settings);
    *mode_saved = false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    network_settings_t record = *settings;
    uint8_t mode = NETWORK_MODE_AP;
    esp_err_t mode_err = nvs_get_u8(handle, MODE_KEY, &mode);
    if (mode_err != ESP_OK && mode_err != ESP_ERR_NVS_NOT_FOUND) {
        err = mode_err;
    } else if (mode_err == ESP_ERR_NVS_NOT_FOUND) {
        /* No mode choice: use anonymous AP defaults. */
        err = ESP_OK;
    } else {
        record.mode = (network_mode_t)mode;
        bool present[STRING_KEY_COUNT] = {0};
        for (size_t i = 0; i < STRING_KEY_COUNT && err == ESP_OK; ++i) {
            size_t size = STRING_KEYS[i].capacity;
            esp_err_t result = nvs_get_str(handle, STRING_KEYS[i].key, setting_string(&record, i), &size);
            present[i] = result == ESP_OK;
            if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) err = result;
        }
        /* A saved STA name without a password key is incomplete, even for open Wi-Fi. */
        if (err == ESP_OK && ((record.sta_ssid[0] && !present[3]) || !network_settings_valid(&record)))
            err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) {
            *settings = record;
            *mode_saved = true;
        }
    }
    nvs_close(handle);
    network_secure_clear(&record, sizeof(record));
    return err;
}

/* Keep exact previous values to restore them after an ordinary write failure.
   Separate NVS keys do not form a power-loss-atomic transaction. */
typedef struct {
    char *strings[STRING_KEY_COUNT];
    uint8_t mode;
    bool mode_present;
} store_snapshot_t;

static esp_err_t snapshot_read(nvs_handle_t handle, store_snapshot_t *snapshot)
{
    esp_err_t err = nvs_get_u8(handle, MODE_KEY, &snapshot->mode);
    snapshot->mode_present = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    for (size_t i = 0; i < STRING_KEY_COUNT; ++i) {
        size_t size = 0;
        err = nvs_get_str(handle, STRING_KEYS[i].key, NULL, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND) continue;
        if (err != ESP_OK) return err;
        if (!size) return ESP_ERR_INVALID_STATE;
        size_t capacity = size;
        snapshot->strings[i] = malloc(capacity);
        if (!snapshot->strings[i]) return ESP_ERR_NO_MEM;
        err = nvs_get_str(handle, STRING_KEYS[i].key, snapshot->strings[i], &size);
        if (err != ESP_OK) {
            /* A failed read may not have produced a terminated string. */
            network_secure_clear(snapshot->strings[i], capacity);
            free(snapshot->strings[i]);
            snapshot->strings[i] = NULL;
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t erase_optional(nvs_handle_t handle, const char *key)
{
    esp_err_t err = nvs_erase_key(handle, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t snapshot_restore(nvs_handle_t handle, const store_snapshot_t *snapshot)
{
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < STRING_KEY_COUNT; ++i) {
        esp_err_t err = snapshot->strings[i] ? nvs_set_str(handle, STRING_KEYS[i].key, snapshot->strings[i]) :
                                             erase_optional(handle, STRING_KEYS[i].key);
        if (result == ESP_OK) result = err;
    }
    esp_err_t err = snapshot->mode_present ? nvs_set_u8(handle, MODE_KEY, snapshot->mode) : erase_optional(handle, MODE_KEY);
    if (result == ESP_OK) result = err;
    err = nvs_commit(handle);
    return result == ESP_OK ? err : result;
}

esp_err_t network_store_save(const network_settings_t *settings)
{
    if (!network_settings_valid(settings)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    store_snapshot_t snapshot = {0};
    err = snapshot_read(handle, &snapshot);
    if (err == ESP_OK) {
        /* An interrupted update without the final mode key boots anonymous AP. */
        err = erase_optional(handle, MODE_KEY);
        for (size_t i = 0; i < STRING_KEY_COUNT && err == ESP_OK; ++i)
            err = nvs_set_str(handle, STRING_KEYS[i].key, (const char *)settings + STRING_KEYS[i].offset);
        /* Select the mode only after both interface configurations are written. */
        if (err == ESP_OK) err = nvs_set_u8(handle, MODE_KEY, (uint8_t)settings->mode);
        if (err == ESP_OK) err = nvs_commit(handle);
        if (err != ESP_OK) {
            esp_err_t restore_err = snapshot_restore(handle, &snapshot);
            if (restore_err != ESP_OK)
                ESP_LOGW("network_store", "Cannot restore previous settings: %s", esp_err_to_name(restore_err));
        }
    }
    for (size_t i = 0; i < STRING_KEY_COUNT; ++i) {
        if (snapshot.strings[i]) {
            network_secure_clear(snapshot.strings[i], strlen(snapshot.strings[i]));
            free(snapshot.strings[i]);
        }
    }
    nvs_close(handle);
    return err;
}
