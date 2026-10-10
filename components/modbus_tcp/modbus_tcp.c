#include "modbus_tcp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

#define CONFIG_NAMESPACE "jsp32_config"
#define IDLE_TIMEOUT_US (30LL * 1000000)

typedef struct {
    int socket;
    uint8_t data[MODBUS_TCP_MAX_ADU];
    size_t length;
    int64_t active_at;
} client_t;

struct modbus_tcp {
    app_registers_t *registers;
    int listener;
    client_t clients[CONFIG_MODBUS_TCP_MAX_CLIENTS];
};

static const char *TAG = "modbus_tcp";

static void close_client(client_t *client)
{
    if (client->socket >= 0) close(client->socket);
    client->socket = -1;
    client->length = 0;
}

static bool send_all(int socket, const uint8_t *data, size_t length)
{
    while (length) {
        int sent = send(socket, data, length, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool process_input(modbus_tcp_t *server, client_t *client)
{
    while (client->length >= 6) {
        size_t declared = ((size_t)client->data[4] << 8) | client->data[5];
        if (client->data[2] || client->data[3] || declared < 2 ||
            declared > MODBUS_TCP_MAX_ADU - 6) return false;
        size_t frame_length = declared + 6;
        if (client->length < frame_length) break;
        uint8_t reply[MODBUS_TCP_MAX_ADU];
        size_t reply_length = modbus_tcp_process(client->data, frame_length,
                                                  reply, server->registers);
        if (!reply_length || !send_all(client->socket, reply, reply_length))
            return false;
        client->length -= frame_length;
        memmove(client->data, client->data + frame_length, client->length);
    }
    return client->length < sizeof(client->data);
}

static void server_task(void *argument)
{
    modbus_tcp_t *server = argument;
    for (;;) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(server->listener, &readers);
        int maximum = server->listener;
        for (size_t i = 0; i < CONFIG_MODBUS_TCP_MAX_CLIENTS; ++i) {
            int socket = server->clients[i].socket;
            if (socket >= 0) {
                FD_SET(socket, &readers);
                if (socket > maximum) maximum = socket;
            }
        }
        struct timeval wait = {.tv_sec = 1};
        int ready = select(maximum + 1, &readers, NULL, NULL, &wait);
        if (ready < 0 && errno != EINTR) {
            ESP_LOGW(TAG, "select failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (ready > 0 && FD_ISSET(server->listener, &readers)) {
            int socket = accept(server->listener, NULL, NULL);
            if (socket >= 0) {
                client_t *slot = NULL;
                for (size_t i = 0; i < CONFIG_MODBUS_TCP_MAX_CLIENTS; ++i) {
                    if (server->clients[i].socket < 0) {
                        slot = &server->clients[i]; break;
                    }
                }
                if (slot) {
                    struct timeval timeout = {.tv_sec = 2};
                    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                               &timeout, sizeof(timeout));
                    slot->socket = socket;
                    slot->length = 0;
                    slot->active_at = esp_timer_get_time();
                } else close(socket);
            }
        }
        int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < CONFIG_MODBUS_TCP_MAX_CLIENTS; ++i) {
            client_t *client = &server->clients[i];
            if (client->socket < 0) continue;
            if (now - client->active_at >= IDLE_TIMEOUT_US) {
                close_client(client); continue;
            }
            if (ready <= 0 || !FD_ISSET(client->socket, &readers)) continue;
            int count = recv(client->socket, client->data + client->length,
                             sizeof(client->data) - client->length, 0);
            if (count <= 0) { close_client(client); continue; }
            client->length += (size_t)count;
            client->active_at = now;
            if (!process_input(server, client)) close_client(client);
        }
    }
}

static esp_err_t load_config(bool *enabled, uint16_t *port)
{
    *enabled = true;
    *port = 502;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t saved_enabled;
    err = nvs_get_u8(handle, "modbus_enabled", &saved_enabled);
    if (err == ESP_OK) {
        if (saved_enabled > 1) ESP_LOGW(TAG, "invalid enabled value; using default");
        else *enabled = saved_enabled != 0;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "cannot read enabled setting: %s", esp_err_to_name(err));
    }
    uint16_t saved_port;
    err = nvs_get_u16(handle, "modbus_port", &saved_port);
    if (err == ESP_OK) {
        if (!saved_port) ESP_LOGW(TAG, "invalid port; using default");
        else *port = saved_port;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "cannot read port setting: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t modbus_tcp_start(app_registers_t *registers, modbus_tcp_t **out_server)
{
    if (!registers || !out_server) return ESP_ERR_INVALID_ARG;
    *out_server = NULL;
    bool enabled;
    uint16_t port;
    esp_err_t err = load_config(&enabled, &port);
    if (err != ESP_OK) return err;
    if (!enabled) {
        ESP_LOGI(TAG, "disabled in NVS");
        return ESP_OK;
    }
    modbus_tcp_t *server = calloc(1, sizeof(*server));
    if (!server) return ESP_ERR_NO_MEM;
    server->registers = registers;
    for (size_t i = 0; i < CONFIG_MODBUS_TCP_MAX_CLIENTS; ++i)
        server->clients[i].socket = -1;
    server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->listener < 0) { free(server); return ESP_FAIL; }
    int reuse = 1;
    setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(server->listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server->listener, CONFIG_MODBUS_TCP_MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "cannot listen on port %u: %d", port, errno);
        close(server->listener); free(server); return ESP_FAIL;
    }
    if (xTaskCreate(server_task, "modbus_tcp", CONFIG_MODBUS_TCP_TASK_STACK_SIZE,
                    server, 4, NULL) != pdPASS) {
        close(server->listener); free(server); return ESP_ERR_NO_MEM;
    }
    *out_server = server;
    ESP_LOGI(TAG, "listening on port %u", port);
    return ESP_OK;
}
