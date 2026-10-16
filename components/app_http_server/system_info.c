#include "app_http_server_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#if CONFIG_ESPTOOLPY_FLASHMODE_QIO
#define FLASH_MODE_NAME "qio"
#elif CONFIG_ESPTOOLPY_FLASHMODE_QOUT
#define FLASH_MODE_NAME "qout"
#elif CONFIG_ESPTOOLPY_FLASHMODE_OPI
#define FLASH_MODE_NAME "opi"
#elif CONFIG_ESPTOOLPY_FLASHMODE_DOUT
#define FLASH_MODE_NAME "dout"
#else
#define FLASH_MODE_NAME "dio"
#endif

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN: return "unknown";
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "other";
    }
}

static void add_mac(cJSON *object, const char *name, esp_mac_type_t type)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, type) != ESP_OK) {
        return;
    }
    char value[18];
    snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(object, name, value);
}

static void add_partitions(cJSON *root)
{
    cJSON *partitions = cJSON_AddArrayToObject(root, "partitions");
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            break;
        }
        cJSON_AddStringToObject(item, "label", partition->label);
        cJSON_AddNumberToObject(item, "type", partition->type);
        cJSON_AddNumberToObject(item, "subtype", partition->subtype);
        cJSON_AddNumberToObject(item, "address", partition->address);
        cJSON_AddNumberToObject(item, "size", partition->size);
        cJSON_AddBoolToObject(item, "encrypted", partition->encrypted);
        cJSON_AddItemToArray(partitions, item);
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);
}

static void add_heap(cJSON *root)
{
    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(heap, "free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(heap, "minimumFree",
                            esp_get_minimum_free_heap_size());

    cJSON *internal = cJSON_AddObjectToObject(heap, "internal");
    cJSON_AddNumberToObject(internal, "total",
        heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(internal, "free",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(internal, "minimumFree",
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(internal, "largestBlock",
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    cJSON *psram = cJSON_AddObjectToObject(heap, "psram");
    cJSON_AddNumberToObject(psram, "total",
        heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(psram, "free",
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(psram, "minimumFree",
        heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(psram, "largestBlock",
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static esp_err_t system_info_handler(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }

    cJSON_AddStringToObject(root, "board", "ESP32-S3 N16R8");
    cJSON_AddStringToObject(root, "target", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(root, "uptimeMs", esp_timer_get_time() / 1000);
    struct timeval now;
    gettimeofday(&now, NULL);
    cJSON_AddNumberToObject(root, "currentTimeMs", (double)now.tv_sec * 1000 + now.tv_usec / 1000);
    esp_reset_reason_t reset_reason = esp_reset_reason();
    cJSON_AddStringToObject(root, "resetReason",
                            reset_reason_name(reset_reason));
    cJSON_AddNumberToObject(root, "resetReasonCode", reset_reason);

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON *chip = cJSON_AddObjectToObject(root, "chip");
    cJSON_AddStringToObject(chip, "model", "ESP32-S3");
    cJSON_AddNumberToObject(chip, "modelCode", chip_info.model);
    cJSON_AddNumberToObject(chip, "cores", chip_info.cores);
    cJSON_AddNumberToObject(chip, "revision", chip_info.revision);
    cJSON_AddNumberToObject(chip, "cpuFrequencyMHz",
                            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON *features = cJSON_AddObjectToObject(chip, "features");
    cJSON_AddBoolToObject(features, "wifi2_4GHz",
                          chip_info.features & CHIP_FEATURE_WIFI_BGN);
    cJSON_AddBoolToObject(features, "bluetoothLE",
                          chip_info.features & CHIP_FEATURE_BLE);
    cJSON_AddBoolToObject(features, "bluetoothClassic",
                          chip_info.features & CHIP_FEATURE_BT);
    cJSON_AddBoolToObject(features, "embeddedFlash",
                          chip_info.features & CHIP_FEATURE_EMB_FLASH);
    cJSON_AddBoolToObject(features, "embeddedPsram",
                          chip_info.features & CHIP_FEATURE_EMB_PSRAM);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    cJSON *flash = cJSON_AddObjectToObject(root, "flash");
    cJSON_AddNumberToObject(flash, "size", flash_size);
    cJSON_AddNumberToObject(flash, "configuredSize", 16 * 1024 * 1024);
    cJSON_AddStringToObject(flash, "mode", FLASH_MODE_NAME);
    cJSON_AddStringToObject(flash, "frequency", CONFIG_ESPTOOLPY_FLASHFREQ);

    cJSON *psram = cJSON_AddObjectToObject(root, "psram");
    cJSON_AddBoolToObject(psram, "initialized", esp_psram_is_initialized());
    cJSON_AddNumberToObject(psram, "size",
        esp_psram_is_initialized() ? esp_psram_get_size() : 0);
#if CONFIG_SPIRAM_MODE_OCT
    cJSON_AddStringToObject(psram, "mode", "octal");
#else
    cJSON_AddStringToObject(psram, "mode", "quad");
#endif
    cJSON_AddNumberToObject(psram, "frequencyMHz", CONFIG_SPIRAM_SPEED);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON *firmware = cJSON_AddObjectToObject(root, "firmware");
    cJSON_AddStringToObject(firmware, "project", app_desc->project_name);
    cJSON_AddStringToObject(firmware, "version", app_desc->version);
    cJSON_AddStringToObject(firmware, "buildDate", app_desc->date);
    cJSON_AddStringToObject(firmware, "buildTime", app_desc->time);
    cJSON_AddStringToObject(firmware, "idfVersion", app_desc->idf_ver);
    cJSON_AddNumberToObject(firmware, "secureVersion",
                            app_desc->secure_version);
    char sha256[65];
    for (size_t i = 0; i < sizeof(app_desc->app_elf_sha256); ++i) {
        snprintf(&sha256[i * 2], 3, "%02x", app_desc->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(firmware, "elfSha256", sha256);

    cJSON *mac = cJSON_AddObjectToObject(root, "mac");
    add_mac(mac, "station", ESP_MAC_WIFI_STA);
    add_mac(mac, "softAp", ESP_MAC_WIFI_SOFTAP);
    add_mac(mac, "bluetooth", ESP_MAC_BT);

    cJSON_AddObjectToObject(root, "network");
    app_http_server_append_info(request->user_ctx, root);
    add_heap(root);
    add_partitions(root);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return err;
}

esp_err_t system_info_register(app_http_server_t *server)
{
    const httpd_uri_t handlers[] = {
        {
            .uri = "/api/v1/system/info",
            .method = HTTP_GET,
            .handler = system_info_handler,
            .user_ctx = server,
        },
    };
    return app_http_server_register(server, "system", handlers,
                                    sizeof(handlers) / sizeof(handlers[0]));
}
