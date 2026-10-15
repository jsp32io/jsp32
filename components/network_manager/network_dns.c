#include "network_manager_internal.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_BUFFER_SIZE 512

static const char *TAG = "network_dns";

static size_t dns_question_end(const uint8_t *packet, size_t length)
{
    size_t offset = 12;
    while (offset < length) {
        uint8_t label_length = packet[offset++];
        if (label_length == 0) {
            return offset + 4 <= length ? offset + 4 : 0;
        }
        if ((label_length & 0xc0) != 0 ||
            offset + label_length > length) {
            return 0;
        }
        offset += label_length;
    }
    return 0;
}

static size_t build_dns_response(uint8_t *packet, size_t length, uint32_t ap_ip)
{
    if (length < 12 || packet[4] != 0 || packet[5] != 1) {
        return 0;
    }
    size_t question_end = dns_question_end(packet, length);
    if (!question_end) {
        return 0;
    }
    size_t type_offset = question_end - 4;
    uint16_t query_type = ((uint16_t)packet[type_offset] << 8) |
                          packet[type_offset + 1];

    packet[2] = 0x81;
    packet[3] = 0x80;
    packet[6] = 0;
    packet[7] = query_type == 1 || query_type == 255 ? 1 : 0;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    if (packet[7] == 0) {
        return question_end;
    }
    if (question_end + 16 > DNS_BUFFER_SIZE) {
        return 0;
    }

    uint8_t answer[] = {
        0xc0, 0x0c,       /* compressed name */
        0x00, 0x01,       /* A */
        0x00, 0x01,       /* IN */
        0x00, 0x00, 0x00, 0x1e, /* TTL 30 seconds */
        0x00, 0x04,
        0, 0, 0, 0,
    };
    memcpy(answer + sizeof(answer) - 4, &ap_ip, 4);
    memcpy(packet + question_end, answer, sizeof(answer));
    return question_end + sizeof(answer);
}

static void dns_task(void *argument)
{
    network_manager_t *manager = argument;
    uint8_t packet[DNS_BUFFER_SIZE];
    while (!manager->dns_stop_requested) {
        struct sockaddr_storage client_address;
        socklen_t client_length = sizeof(client_address);
        int received = recvfrom(manager->dns_socket, packet, sizeof(packet), 0,
                                (struct sockaddr *)&client_address,
                                &client_length);
        if (received < 0) {
            if (manager->dns_stop_requested) {
                break;
            }
            continue;
        }
        size_t response_length = build_dns_response(packet, (size_t)received, manager->dns_ip);
        if (response_length) {
            sendto(manager->dns_socket, packet, response_length, 0,
                   (struct sockaddr *)&client_address, client_length);
        }
    }

    close(manager->dns_socket);
    manager->dns_socket = -1;
    manager->dns_task = NULL;
    xSemaphoreGive(manager->dns_stopped);
    vTaskDelete(NULL);
}

esp_err_t network_dns_start(network_manager_t *manager)
{
    if (manager->dns_task) {
        return ESP_OK;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(manager->ap_netif, &ip_info);
    if (err != ESP_OK) return err;
    if (!ip_info.ip.addr) return ESP_ERR_INVALID_STATE;
    manager->dns_ip = ip_info.ip.addr;
    if (!manager->dns_stopped) {
        manager->dns_stopped = xSemaphoreCreateBinary();
        if (!manager->dns_stopped) {
            return ESP_ERR_NO_MEM;
        }
    }
    while (xSemaphoreTake(manager->dns_stopped, 0) == pdTRUE) {
    }
    manager->dns_stop_requested = false;
    /* Bind only the hotspot address; never answer DNS on the STA interface. */
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = manager->dns_ip,
    };
    manager->dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (manager->dns_socket < 0) return ESP_FAIL;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    if (setsockopt(manager->dns_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        bind(manager->dns_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        ESP_LOGE(TAG, "Cannot bind captive DNS: errno=%d", errno);
        close(manager->dns_socket);
        manager->dns_socket = -1;
        return ESP_FAIL;
    }
    if (xTaskCreate(dns_task, "captive_dns", 4096, manager, 4,
                    &manager->dns_task) != pdPASS) {
        close(manager->dns_socket);
        manager->dns_socket = -1;
        manager->dns_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void network_dns_stop(network_manager_t *manager)
{
    if (!manager->dns_task) {
        return;
    }
    manager->dns_stop_requested = true;
    /* recvfrom wakes within 250 ms; wait for ownership to be released. */
    xSemaphoreTake(manager->dns_stopped, portMAX_DELAY);
}
