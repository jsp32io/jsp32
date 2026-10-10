#include "device_discovery.h"

#include <stdio.h>

#include "esp_mac.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "device_discovery";

esp_err_t device_discovery_start(uint16_t http_port)
{
    if (http_port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }

    char id[sizeof("XXXXXXXXXXXX")];
    char hostname[sizeof("jsp32-XXXXXXXXXXXX")];
    char instance[sizeof("JSP32-XXXXXXXXXXXX")];
    snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(hostname, sizeof(hostname), "jsp32-%s", id);
    snprintf(instance, sizeof(instance), "JSP32-%s", id);

    err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_hostname_set(hostname);
    if (err == ESP_OK) {
        mdns_txt_item_t txt[] = {
            {"api", "/api/v1"},
            {"id", id},
        };
        err = mdns_service_add(instance, "_jsp32", "_tcp", http_port,
                               txt, sizeof(txt) / sizeof(txt[0]));
    }
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }

    ESP_LOGI(TAG, "Advertising %s._jsp32._tcp.local on port %u",
             instance, (unsigned)http_port);
    return ESP_OK;
}
